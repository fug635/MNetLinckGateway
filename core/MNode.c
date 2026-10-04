#include "MNode.h"

#include <string.h>

/* Записи таблицы всегда влезают в один пакет REGISTER или страницу GET. */
MNET_STATIC_ASSERT(MNODE_MAX_ROUTES + 1 <= MNET_ROUTE_PAGE_MAX, "MNODE_MAX_ROUTES must fit one route page");
MNET_STATIC_ASSERT(MNODE_MAX_LINKS < MNODE_LINK_NONE, "MNODE_MAX_LINKS too large");

#define MNODE_MISSES 3

#pragma pack(push, 1)
typedef struct _MNODE_UPDATE_MESSAGE {
    MNET_RPC_HEADER   Rpc;
    MNET_ROUTE_UPDATE Update;
} MNODE_UPDATE_MESSAGE;

typedef struct _MNODE_GET_MESSAGE {
    MNET_RPC_HEADER        Rpc;
    MNET_ROUTE_GET_REQUEST Get;
} MNODE_GET_MESSAGE;
#pragma pack(pop)

static void MNodeRouteService(void* ctx, const MNET_HEADER* packet, MNODE_LINK from);

/* ---- Общее ---------------------------------------------------------------------------------- */

static void MNodeEmit(MNODE* node, MNODE_EVENT_KIND kind, MNODE_LINK link, MNET_NODE who, int32_t status,
                      const uint8_t* data, uint32_t length) {
    if (node->Event) {
        MNODE_EVENT event = {kind, link, who, status, data, length};
        node->Event(node->EventCtx, &event);
    }
}

static bool MNodeReached(uint32_t now, uint32_t due) {
    return (int32_t)(now - due) >= 0;
}

static MNODE_LINK_SLOT* MNodeSlot(MNODE* node, MNODE_LINK link) {
    return (link < MNODE_MAX_LINKS && node->Links[link].Open) ? &node->Links[link] : NULL;
}

static uint32_t MNodeNextRequestId(MNODE* node) {
    uint32_t id = node->NextRequestId++;
    return id ? id : node->NextRequestId++;
}

static uint32_t MNodeNewEpoch(MNODE* node) {
    if (++node->Epoch == 0) {
        ++node->Epoch;
    }
    return node->Epoch;
}

/* ---- Маршруты ------------------------------------------------------------------------------- */

static MNODE_ROUTE* MNodeRouteEntry(MNODE* node, MNET_NODE dst) {
    for (uint32_t i = 0; i < MNODE_MAX_ROUTES; i++) {
        if (node->Routes[i].Node == dst) {
            return &node->Routes[i];
        }
    }
    return NULL;
}

static void MNodeChanged(MNODE* node) {
    node->Generation++;
    node->Dirty = true;
}

static bool MNodeRouteSet(MNODE* node, MNET_NODE dst, MNODE_LINK link, MNODE_ROUTE_KIND kind, uint8_t hops) {
    MNODE_ROUTE* route = MNodeRouteEntry(node, dst);
    if (!route) {
        route = MNodeRouteEntry(node, MNET_NODE_NONE);
    }
    if (!route) {
        return false;
    }
    if (route->Node != dst || route->Link != link || route->Kind != kind || route->Hops != hops) {
        MNodeChanged(node);
    }
    route->Node = dst;
    route->Link = link;
    route->Kind = (uint8_t)kind;
    route->Hops = hops;
    return true;
}

static void MNodeRouteClear(MNODE* node, MNODE_ROUTE* route) {
    if (route->Node != MNET_NODE_NONE) {
        route->Node = MNET_NODE_NONE;
        MNodeChanged(node);
    }
}

static MNODE_LINK MNodeRouteFind(const MNODE* node, MNET_NODE dst) {
    if (dst == MNET_NODE_NONE || dst == MNET_NODE_LINK || dst == node->Self) {
        return MNODE_LINK_NONE;
    }
    for (uint32_t i = 0; i < MNODE_MAX_ROUTES; i++) {
        if (node->Routes[i].Node != dst) {
            continue;
        }
        // Маршрут через упавший линк (закрытый COM с предположением «там hv») не мешает маршруту
        // по умолчанию: узел может быть доступен и через него.
        MNODE_LINK link = node->Routes[i].Link;
        MNODE_LINK fallback = node->DefaultLink;
        if (!node->Links[link].Up && fallback != MNODE_LINK_NONE && fallback != link && node->Links[fallback].Up) {
            return fallback;
        }
        return link;
    }
    return node->DefaultLink;
}

/* Предположения хозяина и узлы, узнанные по пакетам, уступают регистрации через другой линк. */
static bool MNodeReplaceable(const MNODE_ROUTE* route) {
    return route->Kind == MNODE_ROUTE_BOOTSTRAP || route->Kind == MNODE_ROUTE_LEARNED;
}

static bool MNodeAcceptRoute(MNODE* node, MNET_NODE dst, MNODE_LINK link, uint32_t hops, MNODE_ROUTE_KIND kind,
                             uint32_t epoch) {
    if (dst == MNET_NODE_NONE || dst == MNET_NODE_LINK || dst == node->Self || hops >= MNET_DEFAULT_HOP_LIMIT) {
        return false;
    }

    MNODE_ROUTE* route = MNodeRouteEntry(node, dst);
    if (route && route->Link != link && !MNodeReplaceable(route)) {
        return false;
    }
    if (!route || route->Kind != MNODE_ROUTE_CONFIG) {
        if (!MNodeRouteSet(node, dst, link, kind, (uint8_t)hops)) {
            return false;
        }
        route = MNodeRouteEntry(node, dst);
    }
    route->Epoch = epoch;
    return true;
}

static void MNodeSweep(MNODE* node, MNODE_LINK link, uint32_t epoch) {
    for (uint32_t i = 0; i < MNODE_MAX_ROUTES; i++) {
        MNODE_ROUTE* route = &node->Routes[i];
        if (route->Node != MNET_NODE_NONE && route->Link == link && route->Kind != MNODE_ROUTE_CONFIG &&
            route->Epoch != epoch) {
            MNodeRouteClear(node, route);
        }
    }
}

/* Что узел объявляет соседу за линком to: себя (withSelf) и узлы за другими работающими линками. */
static uint32_t MNodeView(MNODE* node, MNODE_LINK to, bool withSelf, MNET_ROUTE_ENTRY* entries) {
    uint32_t count = 0;
    if (withSelf) {
        memset(&entries[count], 0, sizeof(entries[count]));
        entries[count++].Node = node->Self;
    }
    for (uint32_t i = 0; i < MNODE_MAX_ROUTES; i++) {
        const MNODE_ROUTE* route = &node->Routes[i];
        if (route->Node == MNET_NODE_NONE || route->Link == to || !node->Links[route->Link].Up) {
            continue;
        }
        memset(&entries[count], 0, sizeof(entries[count]));
        entries[count].Node = route->Node;
        entries[count].Hops = route->Hops;
        count++;
    }
    return count;
}

/* Узлы от соседа source за линком link; sweep — список полный. false — не принят сам сосед. */
static bool MNodeApply(MNODE* node, MNODE_LINK link, MNET_NODE source, const uint8_t* entries, uint32_t count,
                       uint32_t entrySize, uint32_t epoch, bool sweep) {
    MNODE_LINK_SLOT* slot = &node->Links[link];

    if (!MNodeAcceptRoute(node, source, link, 1, MNODE_ROUTE_NEIGHBOR, epoch)) {
        node->Stats.RouteRejected++;
        return false;
    }
    slot->Neighbor = source;
    slot->Speaks   = true;

    uint32_t rejected = 0;
    if (slot->Flags & MNODE_LINK_TRUST) {
        for (uint32_t i = 0; i < count; i++) {
            MNET_ROUTE_ENTRY entry;
            memcpy(&entry, entries + (size_t)i * entrySize, sizeof(entry));
            if (entry.Node == source ||
                MNodeAcceptRoute(node, entry.Node, link, entry.Hops + 1u, MNODE_ROUTE_REMOTE, epoch)) {
                continue;
            }
            rejected++;
            // Адрес занят за другим линком: пакеты этого узла через линк отбрасываются — сказать об
            // этом, иначе узел виден в таблицах, а запросы к нему молча пропадают.
            const MNODE_ROUTE* taken = MNodeRouteEntry(node, entry.Node);
            if (taken && taken->Link != link) {
                MNodeEmit(node, MNODE_EVENT_DENIED, link, entry.Node, MNET_E_DENIED, NULL, 0);
            }
        }
    }
    if (sweep) {
        MNodeSweep(node, link, epoch);
    }
    node->Stats.RouteUpdates++;
    node->Stats.RouteRejected += rejected;
    return true;
}

/* ---- Передача ------------------------------------------------------------------------------- */

static MNODE_TX MNodeTransmit(MNODE* node, MNODE_LINK link, const void* packet, uint32_t size) {
    MNODE_LINK_SLOT* slot = MNodeSlot(node, link);
    if (!slot || !slot->Up) {
        return MNODE_TX_DOWN;
    }

    MNODE_TX tx = slot->Ops->Send(slot->Ctx, packet, size);
    if (tx == MNODE_TX_OK) {
        slot->TxPackets++;
    } else if (tx == MNODE_TX_FULL) {
        slot->TxDropped++;
    }
    return tx;
}

static uint32_t MNodeBuild(MNODE* node, MNET_PROTO proto, MNET_NODE dst, MNET_ENDPOINT endpoint, MNET_ENDPOINT from,
                           const void* head, uint32_t headLength, const void* data, uint32_t length) {
    uint8_t*     buffer = node->Scratch;
    MNET_HEADER* header = (MNET_HEADER*)buffer;

    MNet_HeaderInit(header, proto, dst, endpoint, node->Self, from, headLength + length);
    if (headLength) {
        memcpy(buffer + sizeof(MNET_HEADER), head, headLength);
    }
    if (length) {
        memcpy(buffer + sizeof(MNET_HEADER) + headLength, data, length);
    }
    MNet_Seal(header);
    return (uint32_t)sizeof(MNET_HEADER) + headLength + length;
}

static bool MNodeLocalPush(MNODE* node, const uint8_t* packet, uint32_t size) {
    uint32_t record = (4u + size + 3u) & ~3u;
    if (MNODE_LOCAL_QUEUE - node->LocalTail < record) {
        node->Stats.LocalDropped++;
        return false;
    }
    memcpy(node->Local + node->LocalTail, &size, 4);
    memcpy(node->Local + node->LocalTail + 4, packet, size);
    node->LocalTail += record;
    return true;
}

/* via != NONE — соседу по линку via, иначе по маршруту до dst. */
static MNODE_STATUS MNodeSendTo(MNODE* node, MNODE_LINK via, MNET_NODE dst, MNET_ENDPOINT from,
                                MNET_ENDPOINT endpoint, MNET_PROTO proto, const void* head, uint32_t headLength,
                                const void* data, uint32_t length) {
    if (headLength > MNET_MAX_PAYLOAD || length > MNET_MAX_PAYLOAD - headLength) {
        return MNODE_TOO_LARGE;
    }

    if (via == MNODE_LINK_NONE && dst == node->Self) {
        uint32_t size = MNodeBuild(node, proto, dst, endpoint, from, head, headLength, data, length);
        return MNodeLocalPush(node, node->Scratch, size) ? MNODE_OK : MNODE_NO_BUFFERS;
    }

    MNODE_LINK link = (via != MNODE_LINK_NONE) ? via : MNodeRouteFind(node, dst);
    if (link == MNODE_LINK_NONE) {
        node->Stats.NoRoute++;
        return MNODE_NO_ROUTE;
    }

    uint32_t size = MNodeBuild(node, proto, dst, endpoint, from, head, headLength, data, length);
    switch (MNodeTransmit(node, link, node->Scratch, size)) {
    case MNODE_TX_OK:
        return MNODE_OK;
    case MNODE_TX_FULL:
        return MNODE_NO_BUFFERS;
    default:
        node->Stats.NoRoute++;
        return MNODE_NO_ROUTE;
    }
}

static const MNET_RPC_HEADER* MNodeRpcRequest(const MNET_HEADER* packet) {
    if (packet->Proto != MNET_PROTO_RPC || packet->Length < sizeof(MNET_RPC_HEADER)) {
        return NULL;
    }
    const MNET_RPC_HEADER* rpc = (const MNET_RPC_HEADER*)MNet_Payload(packet);
    return (rpc->Flags & MNET_RPC_RESPONSE) ? NULL : rpc;
}

static MNODE_STATUS MNodeAnswer(MNODE* node, MNET_ENDPOINT from, const MNET_HEADER* request, MNODE_LINK requestLink,
                                int32_t status, const void* data, uint32_t length) {
    const MNET_RPC_HEADER* rpc = MNodeRpcRequest(request);
    if (!rpc) {
        return MNODE_BAD_PARAM;
    }

    MNET_RPC_HEADER answer = {rpc->RequestId, rpc->Method, MNET_RPC_RESPONSE, status};
    if (request->DstNode == MNET_NODE_LINK) {
        if (requestLink == MNODE_LINK_NONE) {
            return MNODE_BAD_PARAM;
        }
        return MNodeSendTo(node, requestLink, MNET_NODE_LINK, from, request->SrcEndpoint, MNET_PROTO_RPC,
                           &answer, sizeof(answer), data, length);
    }
    return MNodeSendTo(node, MNODE_LINK_NONE, request->SrcNode, from, request->SrcEndpoint, MNET_PROTO_RPC,
                       &answer, sizeof(answer), data, length);
}

static void MNodeRefuse(MNODE* node, const MNET_HEADER* request, MNODE_LINK from, int32_t status) {
    if (MNodeRpcRequest(request)) {
        MNodeAnswer(node, request->DstEndpoint, request, from, status, NULL, 0);
    }
}

/* ---- Служба route: запросы соседям ---------------------------------------------------------- */

static void MNodeRegister(MNODE* node, MNODE_LINK link) {
    MNODE_LINK_SLOT*     slot = &node->Links[link];
    MNET_ROUTE_ENTRY     entries[MNODE_MAX_ROUTES + 1];
    MNODE_UPDATE_MESSAGE message;

    uint32_t count = MNodeView(node, link, false, entries);
    uint32_t id    = MNodeNextRequestId(node);
    slot->Request  = id;
    slot->ViewCrc  = MNet_Crc32c(entries, count * sizeof(MNET_ROUTE_ENTRY));
    slot->SentMs   = node->NowMs;

    memset(&message, 0, sizeof(message));
    message.Rpc.RequestId    = id;
    message.Rpc.Method       = MNET_ROUTE_REGISTER;
    message.Update.Flags     = MNET_ROUTE_FULL | ((slot->Flags & MNODE_LINK_TRUST) ? MNET_ROUTE_NOTIFY : 0);
    message.Update.Count     = (uint16_t)count;
    message.Update.EntrySize = sizeof(MNET_ROUTE_ENTRY);
    MNodeSendTo(node, link, MNET_NODE_LINK, MNET_ENDPOINT_ROUTE, MNET_ENDPOINT_ROUTE, MNET_PROTO_RPC,
                &message, sizeof(message), entries, count * sizeof(MNET_ROUTE_ENTRY));
}

static void MNodeGetPage(MNODE* node, MNODE_LINK link, uint32_t start) {
    MNODE_GET_MESSAGE message;
    memset(&message, 0, sizeof(message));
    message.Rpc.RequestId = MNodeNextRequestId(node);
    message.Rpc.Method    = MNET_ROUTE_GET;
    message.Get.Start     = start;
    node->Links[link].Request = message.Rpc.RequestId;
    MNodeSendTo(node, link, MNET_NODE_LINK, MNET_ENDPOINT_ROUTE, MNET_ENDPOINT_ROUTE, MNET_PROTO_RPC,
                &message, sizeof(message), NULL, 0);
}

/* ANNOUNCE: регистрация пора — отправить и назначить следующую. */
static void MNodeAnnounce(MNODE* node, MNODE_LINK link) {
    MNODE_LINK_SLOT* slot = &node->Links[link];

    if (slot->Request != 0 && slot->Sync == MNODE_SYNC_DONE && ++slot->Misses >= MNODE_MISSES) {
        slot->Sync = MNODE_SYNC_WAITING;
        MNodeEmit(node, MNODE_EVENT_SYNC, link, slot->Neighbor, MNET_E_TIMEOUT, NULL, 0);
    }

    uint32_t wait;
    if (slot->Sync == MNODE_SYNC_DONE) {
        wait = MNODE_REFRESH_MS;
    } else if (slot->Sync == MNODE_SYNC_UNSUPPORTED) {
        wait = MNODE_UNSUPPORTED_MS;
    } else {
        if (slot->BackoffMs < MNODE_RETRY_MIN_MS) {
            slot->BackoffMs = MNODE_RETRY_MIN_MS;
        }
        wait            = slot->BackoffMs;
        slot->BackoffMs = (wait * 2 < MNODE_RETRY_MAX_MS) ? wait * 2 : MNODE_RETRY_MAX_MS;
    }
    slot->DueMs = node->NowMs + wait;
    MNodeRegister(node, link);
}

/* Таблица изменилась: подписчикам и соседям, у которых узел регистрируется, — новая часть таблицы. */
static void MNodePush(MNODE* node) {
    for (uint32_t i = 0; i < MNODE_MAX_LINKS; i++) {
        MNODE_LINK_SLOT* slot = &node->Links[i];
        if (!slot->Open || !slot->Up) {
            continue;
        }
        bool announce = (slot->Flags & MNODE_LINK_ANNOUNCE) && slot->Sync != MNODE_SYNC_UNSUPPORTED;
        if (!announce && !slot->Subscribed) {
            continue;
        }

        MNET_ROUTE_ENTRY entries[MNODE_MAX_ROUTES + 1];
        uint32_t         count = MNodeView(node, (MNODE_LINK)i, false, entries);
        if (MNet_Crc32c(entries, count * sizeof(MNET_ROUTE_ENTRY)) != slot->ViewCrc) {
            MNodeRegister(node, (MNODE_LINK)i);
        }
    }
}

/* ---- Служба route: ответы соседям ----------------------------------------------------------- */

static void MNodeReplyTable(MNODE* node, const MNET_HEADER* request, MNODE_LINK link, uint32_t start) {
    MNET_ROUTE_ENTRY view[MNODE_MAX_ROUTES + 1];
    uint8_t          buffer[sizeof(MNET_ROUTE_TABLE) + sizeof(view)];

    uint32_t total = MNodeView(node, link, true, view);
    if (start > total) {
        start = total;
    }
    uint32_t count = total - start;
    if (count > MNET_ROUTE_PAGE_MAX) {
        count = MNET_ROUTE_PAGE_MAX;
    }

    MNET_ROUTE_TABLE table;
    table.Generation = node->Generation;
    table.Next       = (start + count < total) ? start + count : 0;
    table.Count      = (uint16_t)count;
    table.EntrySize  = sizeof(MNET_ROUTE_ENTRY);
    memcpy(buffer, &table, sizeof(table));
    memcpy(buffer + sizeof(table), view + start, count * sizeof(MNET_ROUTE_ENTRY));

    // Сосед получил свою часть таблицы целиком — рассылать её же снова незачем.
    MNODE_LINK_SLOT* slot = MNodeSlot(node, link);
    if (slot && start == 0 && table.Next == 0) {
        slot->ViewCrc = MNet_Crc32c(view + 1, (total - 1) * sizeof(MNET_ROUTE_ENTRY));
    }
    MNodeAnswer(node, MNET_ENDPOINT_ROUTE, request, link, MNET_OK, buffer,
                (uint32_t)sizeof(table) + count * (uint32_t)sizeof(MNET_ROUTE_ENTRY));
}

static bool MNodeParseUpdate(const uint8_t* data, uint32_t length, MNET_ROUTE_UPDATE* update, const uint8_t** entries) {
    if (length < sizeof(MNET_ROUTE_UPDATE)) {
        return false;
    }
    memcpy(update, data, sizeof(*update));
    *entries = data + sizeof(MNET_ROUTE_UPDATE);
    return update->Count == 0 ||
           (update->EntrySize >= sizeof(MNET_ROUTE_ENTRY) &&
            (uint64_t)update->Count * update->EntrySize <= length - sizeof(MNET_ROUTE_UPDATE));
}

static void MNodeOnRegister(MNODE* node, const MNET_HEADER* request, MNODE_LINK link, const uint8_t* data,
                            uint32_t length) {
    MNET_ROUTE_UPDATE update;
    const uint8_t*    entries;
    if (request->DstNode != MNET_NODE_LINK || link == MNODE_LINK_NONE ||
        !MNodeParseUpdate(data, length, &update, &entries)) {
        MNodeAnswer(node, MNET_ENDPOINT_ROUTE, request, link, MNET_E_BAD_REQUEST, NULL, 0);
        return;
    }

    MNODE_LINK_SLOT* slot   = &node->Links[link];
    MNET_NODE        source = request->SrcNode;
    MNET_NODE        before = slot->Neighbor;
    if (!MNodeApply(node, link, source, entries, update.Count, update.EntrySize, MNodeNewEpoch(node),
                    (update.Flags & MNET_ROUTE_FULL) != 0)) {
        MNodeEmit(node, MNODE_EVENT_DENIED, link, source, MNET_E_DENIED, NULL, 0);
        MNodeAnswer(node, MNET_ENDPOINT_ROUTE, request, link, MNET_E_DENIED, NULL, 0);
        return;
    }

    slot->Subscribed = (update.Flags & MNET_ROUTE_NOTIFY) != 0;
    if (before != source || !slot->Subscribed) {
        MNodeEmit(node, MNODE_EVENT_REGISTERED, link, source, MNET_OK, NULL, 0);
    }
    MNodeReplyTable(node, request, link, 0);
}

static void MNodeOnUnregister(MNODE* node, const MNET_HEADER* request, MNODE_LINK link, const uint8_t* data,
                              uint32_t length) {
    MNET_ROUTE_UPDATE update;
    const uint8_t*    entries;
    if (request->DstNode != MNET_NODE_LINK || link == MNODE_LINK_NONE ||
        !MNodeParseUpdate(data, length, &update, &entries)) {
        MNodeAnswer(node, MNET_ENDPOINT_ROUTE, request, link, MNET_E_BAD_REQUEST, NULL, 0);
        return;
    }

    MNODE_LINK_SLOT* slot = &node->Links[link];
    for (uint32_t i = 0; i < MNODE_MAX_ROUTES; i++) {
        MNODE_ROUTE* route = &node->Routes[i];
        if (route->Node == MNET_NODE_NONE || route->Link != link || route->Kind == MNODE_ROUTE_CONFIG) {
            continue;
        }
        bool listed = (update.Count == 0);
        for (uint32_t j = 0; j < update.Count && !listed; j++) {
            MNET_ROUTE_ENTRY entry;
            memcpy(&entry, entries + (size_t)j * update.EntrySize, sizeof(entry));
            listed = (entry.Node == route->Node);
        }
        if (listed) {
            MNodeRouteClear(node, route);
        }
    }
    if (update.Count == 0) {
        slot->Subscribed = false;
        slot->Neighbor   = slot->ConfigNeighbor;
    }
    MNodeAnswer(node, MNET_ENDPOINT_ROUTE, request, link, MNET_OK, NULL, 0);
}

static void MNodeOnAnswer(MNODE* node, const MNET_HEADER* packet, const MNET_RPC_HEADER* rpc, MNODE_LINK link,
                          const uint8_t* data, uint32_t length) {
    MNODE_LINK_SLOT* slot = MNodeSlot(node, link);
    if (!slot || rpc->RequestId == 0 || slot->Request != rpc->RequestId) {
        return;
    }
    slot->Request = 0;

    MNET_NODE source = packet->SrcNode;
    if (rpc->Status != MNET_OK) {
        if (rpc->Method == MNET_ROUTE_REGISTER) {
            slot->Sync  = MNODE_SYNC_UNSUPPORTED;
            slot->DueMs = node->NowMs + MNODE_UNSUPPORTED_MS;
            MNodeEmit(node, MNODE_EVENT_SYNC, link, source, rpc->Status, NULL, 0);
        }
        return;
    }

    MNET_ROUTE_TABLE table;
    memset(&table, 0, sizeof(table));
    const uint8_t* entries = data + sizeof(MNET_ROUTE_TABLE);
    if (length >= sizeof(MNET_ROUTE_TABLE)) {
        memcpy(&table, data, sizeof(table));
        if (table.Count && (table.EntrySize < sizeof(MNET_ROUTE_ENTRY) ||
                            (uint64_t)table.Count * table.EntrySize > length - sizeof(MNET_ROUTE_TABLE))) {
            return;
        }
    }

    // Первая страница — ответ на REGISTER, следующие — GET. Снимать маршруты, которых нет в таблице,
    // можно, только если она не менялась, пока шли страницы.
    uint32_t epoch;
    bool     sweep;
    if (rpc->Method == MNET_ROUTE_REGISTER) {
        epoch          = MNodeNewEpoch(node);
        slot->Epoch    = epoch;
        slot->TableGen = table.Generation;
        sweep          = (table.Next == 0);
    } else {
        epoch = slot->Epoch;
        sweep = (table.Next == 0 && table.Generation == slot->TableGen);
    }

    bool accepted = MNodeApply(node, link, source, entries, table.Count, table.EntrySize, epoch, sweep);
    if (table.Next != 0) {
        MNodeGetPage(node, link, table.Next);
    }
    if (rpc->Method == MNET_ROUTE_REGISTER) {
        slot->Sync      = MNODE_SYNC_DONE;
        slot->Misses    = 0;
        slot->BackoffMs = MNODE_RETRY_MIN_MS;
        if (slot->Flags & MNODE_LINK_ANNOUNCE) {
            slot->DueMs = node->NowMs + MNODE_REFRESH_MS;
        }
        MNodeEmit(node, MNODE_EVENT_SYNC, link, source, accepted ? MNET_OK : MNET_E_DENIED, NULL, 0);
    }
}

static void MNodeRouteService(void* ctx, const MNET_HEADER* packet, MNODE_LINK from) {
    MNODE* node = (MNODE*)ctx;
    if (packet->Proto != MNET_PROTO_RPC || packet->Length < sizeof(MNET_RPC_HEADER)) {
        return;
    }

    MNET_RPC_HEADER rpc;
    memcpy(&rpc, MNet_Payload(packet), sizeof(rpc));
    const uint8_t* data   = MNet_Payload(packet) + sizeof(MNET_RPC_HEADER);
    uint32_t       length = packet->Length - (uint32_t)sizeof(MNET_RPC_HEADER);

    if (rpc.Flags & MNET_RPC_RESPONSE) {
        MNodeOnAnswer(node, packet, &rpc, from, data, length);
        return;
    }

    switch (rpc.Method) {
    case MNET_ROUTE_GET: {
        MNET_ROUTE_GET_REQUEST get = {0};
        if (length >= sizeof(get)) {
            memcpy(&get, data, sizeof(get));
        }
        if (from != MNODE_LINK_NONE) {
            node->Links[from].Speaks = true;
        }
        MNodeReplyTable(node, packet, from, get.Start);
        break;
    }
    case MNET_ROUTE_REGISTER:
        MNodeOnRegister(node, packet, from, data, length);
        break;
    case MNET_ROUTE_UNREGISTER:
        MNodeOnUnregister(node, packet, from, data, length);
        break;
    default:
        MNodeAnswer(node, MNET_ENDPOINT_ROUTE, packet, from, MNET_E_NO_METHOD, NULL, 0);
        break;
    }
}

/* ---- Приём ---------------------------------------------------------------------------------- */

static MNODE_ENDPOINT* MNodeEndpoint(MNODE* node, MNET_ENDPOINT number) {
    for (uint32_t i = 0; i < MNODE_MAX_ENDPOINTS; i++) {
        if (node->Endpoints[i].Number == number && node->Endpoints[i].Handler) {
            return &node->Endpoints[i];
        }
    }
    return NULL;
}

static void MNodeDeliver(MNODE* node, const MNET_HEADER* packet, MNODE_LINK from) {
    MNODE_ENDPOINT* ep = (packet->DstEndpoint != MNET_ENDPOINT_NONE) ? MNodeEndpoint(node, packet->DstEndpoint) : NULL;
    if (!ep) {
        node->Stats.NoEndpoint++;
        MNodeRefuse(node, packet, from, MNET_E_NO_ENDPOINT);
        return;
    }
    node->Stats.Delivered++;
    ep->Handler(ep->Ctx, packet, from);
}

static void MNodeForward(MNODE* node, const MNET_HEADER* packet, MNODE_LINK from) {
    if (packet->HopLimit <= 1) {
        node->Stats.HopLimit++;
        return;
    }

    MNODE_LINK link = MNodeRouteFind(node, packet->DstNode);
    if (link == MNODE_LINK_NONE || link == from) {
        node->Stats.NoRoute++;
        MNodeRefuse(node, packet, from, MNET_E_NO_ROUTE);
        return;
    }

    uint32_t size = MNet_PacketSize(packet);
    memcpy(node->Scratch, packet, size);
    MNET_HEADER* copy = (MNET_HEADER*)node->Scratch;
    copy->HopLimit--;
    MNet_SealHeader(copy);

    MNODE_TX tx = MNodeTransmit(node, link, node->Scratch, size);
    if (tx == MNODE_TX_OK) {
        node->Stats.Forwarded++;
    } else if (tx == MNODE_TX_DOWN) {
        node->Stats.NoRoute++;
        MNodeRefuse(node, packet, from, MNET_E_NO_ROUTE);
    }
}

static bool MNodeSourceAllowed(MNODE* node, MNODE_LINK link, MNET_NODE source) {
    if (source == MNET_NODE_NONE || source == MNET_NODE_LINK || source == node->Self) {
        return false;
    }

    MNODE_ROUTE* route = MNodeRouteEntry(node, source);
    if (route) {
        // Источник с маршрутом через упавший линк принимается только с линка по умолчанию — тем же
        // путём, каким к нему сейчас идут пакеты (MNodeRouteFind).
        return route->Link == link || (link == node->DefaultLink && !node->Links[route->Link].Up);
    }

    // За линком по умолчанию — вся остальная сеть.
    MNODE_LINK_SLOT* slot = &node->Links[link];
    if (link == node->DefaultLink) {
        return true;
    }
    if ((slot->Flags & MNODE_LINK_LEARN) && slot->Neighbor == MNET_NODE_NONE) {
        if (!MNodeRouteSet(node, source, link, MNODE_ROUTE_NEIGHBOR, 1)) {
            return false;
        }
        slot->Neighbor = source;
        MNodeEmit(node, MNODE_EVENT_REGISTERED, link, source, MNET_OK, NULL, 0);
        return true;
    }
    if (slot->Flags & MNODE_LINK_ADOPT) {
        // Узел за соседом, о котором тот ещё не сообщил (REGISTER потерялся): принять и сверить таблицу.
        if (!MNodeRouteSet(node, source, link, MNODE_ROUTE_LEARNED, 2)) {
            return false;
        }
        node->Stats.RouteLearned++;
        if (!slot->RefreshPending) {
            slot->RefreshPending = true;
            slot->RefreshMs      = slot->SentMs + MNODE_LEARN_MIN_MS;
        }
        return true;
    }
    return false;
}

static void MNodeAccept(MNODE* node, MNODE_LINK link, const MNET_HEADER* packet) {
    MNET_NODE source = packet->SrcNode;

    if (packet->DstNode == MNET_NODE_LINK) {
        if (source == MNET_NODE_NONE || source == MNET_NODE_LINK || source == node->Self) {
            node->Stats.BadSource++;
            MNodeEmit(node, MNODE_EVENT_BAD_SOURCE, link, source, 0, NULL, 0);
            return;
        }
        MNodeDeliver(node, packet, link);
        return;
    }

    if (!MNodeSourceAllowed(node, link, source)) {
        node->Stats.BadSource++;
        MNodeEmit(node, MNODE_EVENT_BAD_SOURCE, link, source, 0, NULL, 0);
        return;
    }

    if (packet->DstNode == node->Self) {
        MNodeDeliver(node, packet, link);
        return;
    }

    // Источник только что стал известен — соседи узнают о нём раньше, чем получат его пакет.
    if (node->Dirty) {
        MNodePush(node);
    }
    MNodeForward(node, packet, link);
}

static void MNodeStreamDrain(MNODE* node, MNODE_LINK link, bool idle) {
    MNODE_LINK_SLOT* slot = &node->Links[link];

    while (slot->Open && slot->Parser) {
        const uint8_t* data;
        uint32_t       length;
        MNET_PARSE     kind = MNet_ParserRead(slot->Parser, &data, &length);
        if (kind == MNET_PARSE_MORE) {
            if (!idle) {
                return;
            }
            kind = MNet_ParserSkip(slot->Parser, &data, &length);
            if (kind == MNET_PARSE_MORE) {
                return;
            }
        }

        if (kind == MNET_PARSE_RAW) {
            // Один байт, на котором разборщик отказался от целого заголовка, — испорченный пакет.
            if (length == 1 && slot->Parser->Fill >= sizeof(MNET_HEADER) && MNetParserMayStartAt(slot->Parser, 0)) {
                slot->RxBad++;
                MNodeEmit(node, MNODE_EVENT_DAMAGED, link, MNET_NODE_NONE, 0, NULL, 0);
            }
            slot->RxRawBytes += length;
            MNodeEmit(node, MNODE_EVENT_RAW, link, MNET_NODE_NONE, 0, data, length);
            continue;
        }

        slot->RxPackets++;
        MNodeAccept(node, link, (const MNET_HEADER*)data);
    }
}

/* ---- Вход и выход из вызова ----------------------------------------------------------------- */

static void MNodeEnter(MNODE* node) {
    node->Depth++;
}

/* Конец самого внешнего вызова: пакеты себе, рассылка изменений таблицы, событие ROUTES. */
static void MNodeLeave(MNODE* node) {
    if (node->Depth > 1) {
        node->Depth--;
        return;
    }

    for (uint32_t round = 0; round < 8; round++) {
        while (node->LocalHead < node->LocalTail) {
            uint32_t size;
            memcpy(&size, node->Local + node->LocalHead, 4);
            const MNET_HEADER* packet = (const MNET_HEADER*)(node->Local + node->LocalHead + 4);
            node->LocalHead += (4u + size + 3u) & ~3u;
            MNodeDeliver(node, packet, MNODE_LINK_NONE);
        }
        node->LocalHead = 0;
        node->LocalTail = 0;

        if (!node->Dirty) {
            break;
        }
        node->Dirty = false;
        MNodePush(node);
        MNodeEmit(node, MNODE_EVENT_ROUTES, MNODE_LINK_NONE, MNET_NODE_NONE, 0, NULL, 0);
    }
    node->Depth = 0;
}

/* ---- API ------------------------------------------------------------------------------------ */

void MNode_Init(MNODE* node, MNET_NODE self, MNODE_EVENT_HANDLER event, void* eventCtx) {
    memset(node, 0, sizeof(*node));
    node->Self          = self;
    node->Event         = event;
    node->EventCtx      = eventCtx;
    node->DefaultLink   = MNODE_LINK_NONE;
    node->NextDynamic   = MNET_ENDPOINT_DYNAMIC_FIRST;
    node->NextRequestId = 1;
    MNode_Open(node, MNET_ENDPOINT_ROUTE, MNodeRouteService, node);
}

MNET_NODE MNode_Self(const MNODE* node) {
    return node->Self;
}

const MNODE_STATS* MNode_Stats(const MNODE* node) {
    return &node->Stats;
}

MNODE_LINK MNode_AddLink(MNODE* node, const MNODE_LINK_DESC* desc) {
    if (!desc || !desc->Ops || !desc->Ops->Send || !desc->Name || desc->Neighbor == node->Self ||
        desc->Neighbor == MNET_NODE_LINK || ((desc->Flags & MNODE_LINK_STREAM) && !desc->Parser)) {
        return MNODE_LINK_NONE;
    }

    MNODE_LINK link = MNODE_LINK_NONE;
    for (uint32_t i = 0; i < MNODE_MAX_LINKS && link == MNODE_LINK_NONE; i++) {
        if (!node->Links[i].Open) {
            link = (MNODE_LINK)i;
        }
    }
    if (link == MNODE_LINK_NONE) {
        return MNODE_LINK_NONE;
    }

    MNodeEnter(node);

    MNODE_LINK_SLOT* slot = &node->Links[link];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->Name, desc->Name, sizeof(slot->Name) - 1);
    slot->Ops             = desc->Ops;
    slot->Ctx             = desc->Ctx;
    slot->Flags           = desc->Flags;
    slot->Neighbor        = desc->Neighbor;
    slot->ConfigNeighbor  = desc->Neighbor;
    slot->ConfigBootstrap = desc->Bootstrap;
    slot->Parser          = (desc->Flags & MNODE_LINK_STREAM) ? desc->Parser : NULL;
    slot->Up              = desc->Up;
    slot->Sync            = (desc->Flags & MNODE_LINK_ANNOUNCE) ? MNODE_SYNC_WAITING : MNODE_SYNC_NONE;
    slot->BackoffMs       = MNODE_RETRY_MIN_MS;
    slot->LastRxMs        = node->NowMs;
    slot->Open            = true;
    if (slot->Parser) {
        MNet_ParserInit(slot->Parser);
    }
    if (desc->Neighbor != MNET_NODE_NONE &&
        !MNodeRouteSet(node, desc->Neighbor, link, desc->Bootstrap ? MNODE_ROUTE_BOOTSTRAP : MNODE_ROUTE_CONFIG, 1)) {
        memset(slot, 0, sizeof(*slot));
        MNodeLeave(node);
        return MNODE_LINK_NONE;
    }
    if (desc->Flags & MNODE_LINK_DEFAULT) {
        node->DefaultLink = link;
    }
    MNodeChanged(node);
    if (slot->Up && (slot->Flags & MNODE_LINK_ANNOUNCE)) {
        MNodeAnnounce(node, link);
    }

    MNodeLeave(node);
    return link;
}

void MNode_RemoveLink(MNODE* node, MNODE_LINK link) {
    MNODE_LINK_SLOT* slot = MNodeSlot(node, link);
    if (!slot) {
        return;
    }

    MNodeEnter(node);
    for (uint32_t i = 0; i < MNODE_MAX_ROUTES; i++) {
        if (node->Routes[i].Node != MNET_NODE_NONE && node->Routes[i].Link == link) {
            MNodeRouteClear(node, &node->Routes[i]);
        }
    }
    if (node->DefaultLink == link) {
        node->DefaultLink = MNODE_LINK_NONE;
    }
    memset(slot, 0, sizeof(*slot));
    MNodeChanged(node);
    MNodeLeave(node);
}

void MNode_SetLinkUp(MNODE* node, MNODE_LINK link, bool up) {
    MNODE_LINK_SLOT* slot = MNodeSlot(node, link);
    if (!slot || slot->Up == up) {
        return;
    }

    MNodeEnter(node);
    slot->Up             = up;
    slot->Request        = 0;
    slot->Misses         = 0;
    slot->BackoffMs      = MNODE_RETRY_MIN_MS;
    slot->RefreshPending = false;
    slot->Sync           = (slot->Flags & MNODE_LINK_ANNOUNCE) ? MNODE_SYNC_WAITING : MNODE_SYNC_NONE;

    if (up) {
        slot->LastRxMs = node->NowMs;
        // Предположение о соседе вернуть, если его маршрут, пока линк лежал, ушёл к другому линку.
        if (slot->ConfigNeighbor != MNET_NODE_NONE && !MNodeRouteEntry(node, slot->ConfigNeighbor)) {
            MNodeRouteSet(node, slot->ConfigNeighbor, link,
                          slot->ConfigBootstrap ? MNODE_ROUTE_BOOTSTRAP : MNODE_ROUTE_CONFIG, 1);
        }
        if (slot->Flags & MNODE_LINK_ANNOUNCE) {
            MNodeAnnounce(node, link);
        }
    } else {
        // Узнанное от соседа уходит вместе с линком; настроенное хозяином — остаётся до следующего подъёма.
        for (uint32_t i = 0; i < MNODE_MAX_ROUTES; i++) {
            MNODE_ROUTE* route = &node->Routes[i];
            if (route->Node != MNET_NODE_NONE && route->Link == link && route->Kind != MNODE_ROUTE_CONFIG &&
                route->Kind != MNODE_ROUTE_BOOTSTRAP) {
                MNodeRouteClear(node, route);
            }
        }
        if (slot->ConfigNeighbor != MNET_NODE_NONE && !MNodeRouteEntry(node, slot->ConfigNeighbor)) {
            MNodeRouteSet(node, slot->ConfigNeighbor, link,
                          slot->ConfigBootstrap ? MNODE_ROUTE_BOOTSTRAP : MNODE_ROUTE_CONFIG, 1);
        }
        slot->Neighbor   = slot->ConfigNeighbor;
        slot->Subscribed = false;
        slot->Speaks     = false;
        slot->ViewCrc    = 0;
        if (slot->Parser) {
            MNet_ParserInit(slot->Parser);
        }
    }
    MNodeChanged(node);
    MNodeLeave(node);
}

void MNode_SetDefaultLink(MNODE* node, MNODE_LINK link) {
    node->DefaultLink = MNodeSlot(node, link) ? link : MNODE_LINK_NONE;
}

bool MNode_GetLink(const MNODE* node, MNODE_LINK link, MNODE_LINK_INFO* info) {
    if (link >= MNODE_MAX_LINKS || !node->Links[link].Open || !info) {
        return false;
    }
    const MNODE_LINK_SLOT* slot = &node->Links[link];
    memcpy(info->Name, slot->Name, sizeof(info->Name));
    info->Flags      = slot->Flags;
    info->Up         = slot->Up;
    info->Neighbor   = slot->Neighbor;
    info->Sync       = slot->Sync;
    info->Subscribed = slot->Subscribed;
    info->RxPackets  = slot->RxPackets;
    info->RxBad      = slot->RxBad;
    info->RxRawBytes = slot->RxRawBytes;
    info->TxPackets  = slot->TxPackets;
    info->TxDropped  = slot->TxDropped;
    return true;
}

void MNode_InputBytes(MNODE* node, MNODE_LINK link, const void* data, uint32_t length) {
    MNODE_LINK_SLOT* slot = MNodeSlot(node, link);
    if (!slot || !slot->Parser) {
        return;
    }

    MNodeEnter(node);
    slot->LastRxMs = node->NowMs;
    const uint8_t* bytes = (const uint8_t*)data;
    while (length && slot->Open) {
        uint32_t taken = MNet_ParserWrite(slot->Parser, bytes, length);
        bytes += taken;
        length -= taken;
        MNodeStreamDrain(node, link, false);
    }
    MNodeLeave(node);
}

void MNode_InputPacket(MNODE* node, MNODE_LINK link, const void* packet, uint32_t length) {
    MNODE_LINK_SLOT* slot = MNodeSlot(node, link);
    if (!slot) {
        return;
    }

    const MNET_HEADER* header = (const MNET_HEADER*)packet;
    MNodeEnter(node);
    if (length >= sizeof(MNET_HEADER) && MNet_CheckPacket(header) == MNET_CHECK_OK && MNet_PacketSize(header) <= length) {
        slot->RxPackets++;
        MNodeAccept(node, link, header);
    } else {
        slot->RxBad++;
    }
    MNodeLeave(node);
}

void MNode_Poll(MNODE* node, uint32_t nowMs) {
    MNodeEnter(node);
    node->NowMs = nowMs;

    for (uint32_t i = 0; i < MNODE_MAX_LINKS; i++) {
        MNODE_LINK_SLOT* slot = &node->Links[i];
        if (!slot->Open) {
            continue;
        }
        if (slot->Parser && slot->Parser->Fill > 0) {
            // Текст без конца строки выходит через 100 мс тишины; пакет, у которого заголовок уже
            // пришёл целым, ждёт продолжения дольше: линия (UART под QEMU) бывает занята дольше 100 мс.
            const MNET_PARSER* parser = slot->Parser;
            uint32_t idle = (parser->Fill >= sizeof(MNET_HEADER) &&
                             MNet_CheckHeader((const MNET_HEADER*)parser->Buffer) == MNET_CHECK_OK)
                                ? MNODE_PACKET_IDLE_MS
                                : MNODE_STREAM_IDLE_MS;
            if (MNodeReached(nowMs, slot->LastRxMs + idle)) {
                MNodeStreamDrain(node, (MNODE_LINK)i, true);
            }
        }
        if (!slot->Open || !slot->Up) {
            continue;
        }
        if ((slot->Flags & MNODE_LINK_ANNOUNCE) && MNodeReached(nowMs, slot->DueMs)) {
            MNodeAnnounce(node, (MNODE_LINK)i);
        } else if (slot->RefreshPending && MNodeReached(nowMs, slot->RefreshMs)) {
            slot->RefreshPending = false;
            if (slot->Speaks || (slot->Flags & MNODE_LINK_ANNOUNCE)) {
                MNodeRegister(node, (MNODE_LINK)i);
            }
        }
    }

    MNodeLeave(node);
}

bool MNode_AddRoute(MNODE* node, MNET_NODE dst, MNODE_LINK link, bool bootstrap) {
    if (!MNodeSlot(node, link) || dst == MNET_NODE_NONE || dst == MNET_NODE_LINK || dst == node->Self) {
        return false;
    }
    MNodeEnter(node);
    bool ok = MNodeRouteSet(node, dst, link, bootstrap ? MNODE_ROUTE_BOOTSTRAP : MNODE_ROUTE_CONFIG, 1);
    MNodeLeave(node);
    return ok;
}

void MNode_RemoveRoute(MNODE* node, MNET_NODE dst) {
    MNODE_ROUTE* route = (dst != MNET_NODE_NONE) ? MNodeRouteEntry(node, dst) : NULL;
    if (route) {
        MNodeEnter(node);
        MNodeRouteClear(node, route);
        MNodeLeave(node);
    }
}

bool MNode_GetRoute(const MNODE* node, uint32_t index, MNODE_ROUTE_INFO* info) {
    if (index >= MNODE_MAX_ROUTES || node->Routes[index].Node == MNET_NODE_NONE || !info) {
        return false;
    }
    const MNODE_ROUTE* route = &node->Routes[index];
    info->Node = route->Node;
    info->Link = route->Link;
    info->Kind = (MNODE_ROUTE_KIND)route->Kind;
    info->Hops = route->Hops;
    return true;
}

MNODE_LINK MNode_FindRoute(const MNODE* node, MNET_NODE dst) {
    return MNodeRouteFind(node, dst);
}

void MNode_Sync(MNODE* node, MNODE_LINK link) {
    MNODE_LINK_SLOT* slot = MNodeSlot(node, link);
    if (!slot || !slot->Up) {
        return;
    }
    MNodeEnter(node);
    if (slot->Flags & MNODE_LINK_ANNOUNCE) {
        slot->Sync = (slot->Sync == MNODE_SYNC_UNSUPPORTED) ? MNODE_SYNC_WAITING : slot->Sync;
        MNodeAnnounce(node, link);
    } else {
        MNodeRegister(node, link);
    }
    MNodeLeave(node);
}

MNET_ENDPOINT MNode_Open(MNODE* node, MNET_ENDPOINT number, MNODE_HANDLER handler, void* ctx) {
    if (!handler) {
        return MNET_ENDPOINT_NONE;
    }

    MNODE_ENDPOINT* free = NULL;
    for (uint32_t i = 0; i < MNODE_MAX_ENDPOINTS && !free; i++) {
        if (!node->Endpoints[i].Handler) {
            free = &node->Endpoints[i];
        }
    }
    if (!free) {
        return MNET_ENDPOINT_NONE;
    }

    if (number == MNET_ENDPOINT_NONE) {
        for (uint32_t tries = 0; tries <= 0xFFFFu - MNET_ENDPOINT_DYNAMIC_FIRST && number == MNET_ENDPOINT_NONE; tries++) {
            MNET_ENDPOINT candidate = node->NextDynamic;
            node->NextDynamic = (candidate == 0xFFFF) ? (MNET_ENDPOINT)MNET_ENDPOINT_DYNAMIC_FIRST
                                                      : (MNET_ENDPOINT)(candidate + 1);
            if (!MNodeEndpoint(node, candidate)) {
                number = candidate;
            }
        }
        if (number == MNET_ENDPOINT_NONE) {
            return MNET_ENDPOINT_NONE;
        }
    } else if (MNodeEndpoint(node, number)) {
        return MNET_ENDPOINT_NONE;
    }

    free->Number  = number;
    free->Handler = handler;
    free->Ctx     = ctx;
    return number;
}

void MNode_Close(MNODE* node, MNET_ENDPOINT number) {
    MNODE_ENDPOINT* ep = MNodeEndpoint(node, number);
    if (ep && number != MNET_ENDPOINT_ROUTE) {
        memset(ep, 0, sizeof(*ep));
    }
}

MNODE_STATUS MNode_Send(MNODE* node, MNET_ENDPOINT from, MNET_NODE dst, MNET_ENDPOINT endpoint, MNET_PROTO proto,
                        const void* head, uint32_t headLength, const void* data, uint32_t length) {
    if (dst == MNET_NODE_NONE || dst == MNET_NODE_LINK || (headLength && !head) || (length && !data)) {
        return MNODE_BAD_PARAM;
    }
    MNodeEnter(node);
    MNODE_STATUS status = MNodeSendTo(node, MNODE_LINK_NONE, dst, from, endpoint, proto, head, headLength, data, length);
    MNodeLeave(node);
    return status;
}

MNODE_STATUS MNode_SendLink(MNODE* node, MNODE_LINK link, MNET_ENDPOINT from, MNET_ENDPOINT endpoint,
                            MNET_PROTO proto, const void* head, uint32_t headLength, const void* data, uint32_t length) {
    if (!MNodeSlot(node, link) || (headLength && !head) || (length && !data)) {
        return MNODE_BAD_PARAM;
    }
    MNodeEnter(node);
    MNODE_STATUS status = MNodeSendTo(node, link, MNET_NODE_LINK, from, endpoint, proto, head, headLength, data, length);
    MNodeLeave(node);
    return status;
}

MNODE_STATUS MNode_Reply(MNODE* node, MNET_ENDPOINT from, const MNET_HEADER* request, MNODE_LINK requestLink,
                         int32_t status, const void* data, uint32_t length) {
    if (!request || (length && !data)) {
        return MNODE_BAD_PARAM;
    }
    MNodeEnter(node);
    MNODE_STATUS result = MNodeAnswer(node, from, request, requestLink, status, data, length);
    MNodeLeave(node);
    return result;
}

void MNode_DeliverLocal(MNODE* node, const MNET_HEADER* packet) {
    if (!packet || packet->DstNode != node->Self) {
        return;
    }
    MNodeEnter(node);
    MNodeDeliver(node, packet, MNODE_LINK_NONE);
    MNodeLeave(node);
}
