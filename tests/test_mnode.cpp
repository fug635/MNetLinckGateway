/*
    Тесты переносимого узла MNode без ОС и Qt: несколько узлов в памяти, соединённых «проводами».
    Провод передаёт пакеты (или байты, если линк потоковый) соседу через общую очередь, которую
    тест прокачивает pump(): так ответы не приходят изнутри Send, как и у настоящего транспорта.
*/
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MNode.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        g_checks++;                                                                       \
        if (!(cond)) {                                                                    \
            g_failures++;                                                                 \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                                 \
    } while (0)

using Bytes = std::vector<uint8_t>;

MNET_NODE addr(uint16_t local)
{
    return MNET_NODE_MAKE(MNET_NET_DEFAULT, local);
}

struct Net;

struct Port
{
    Net *net = nullptr;
    MNODE *peer = nullptr;
    MNODE_LINK peerLink = MNODE_LINK_NONE;
    bool peerStream = false;
    bool up = true;
    bool full = false;
    std::vector<Bytes> sent;
};

struct Message
{
    MNODE *to;
    MNODE_LINK link;
    bool stream;
    Bytes bytes;
};

struct Net
{
    std::deque<Message> queue;

    void pump()
    {
        for (int i = 0; i < 10000 && !queue.empty(); i++) {
            Message m = std::move(queue.front());
            queue.pop_front();
            if (m.stream)
                MNode_InputBytes(m.to, m.link, m.bytes.data(), uint32_t(m.bytes.size()));
            else
                MNode_InputPacket(m.to, m.link, m.bytes.data(), uint32_t(m.bytes.size()));
        }
    }
};

MNODE_TX portSend(void *ctx, const void *packet, uint32_t size)
{
    auto *port = static_cast<Port *>(ctx);
    if (!port->up)
        return MNODE_TX_DOWN;
    if (port->full)
        return MNODE_TX_FULL;
    Bytes bytes(static_cast<const uint8_t *>(packet), static_cast<const uint8_t *>(packet) + size);
    port->sent.push_back(bytes);
    if (port->peer)
        port->net->queue.push_back({port->peer, port->peerLink, port->peerStream, bytes});
    return MNODE_TX_OK;
}

const MNODE_LINK_OPS PortOps = {portSend};

struct Event
{
    MNODE_EVENT_KIND kind;
    MNODE_LINK link;
    MNET_NODE node;
    int32_t status;
    std::string data;
};

// Узел с журналом событий; память узла — в куче, как у хозяина на ПК.
struct Node
{
    std::unique_ptr<MNODE> mnode{new MNODE};
    std::vector<Event> events;
    std::vector<std::unique_ptr<Port>> ports;
    std::vector<std::unique_ptr<MNET_PARSER>> parsers;

    explicit Node(MNET_NODE self)
    {
        MNode_Init(mnode.get(), self, [](void *ctx, const MNODE_EVENT *e) {
            static_cast<Node *>(ctx)->events.push_back(
                {e->Kind, e->Link, e->Node, e->Status,
                 e->Data ? std::string(reinterpret_cast<const char *>(e->Data), e->Length) : std::string()});
        }, this);
    }

    MNODE *operator->() { return mnode.get(); }
    MNODE *get() { return mnode.get(); }

    MNODE_LINK add(const char *name, uint32_t flags, MNET_NODE neighbor = MNET_NODE_NONE, bool bootstrap = false)
    {
        ports.push_back(std::make_unique<Port>());
        MNET_PARSER *parser = nullptr;
        if (flags & MNODE_LINK_STREAM) {
            parsers.push_back(std::make_unique<MNET_PARSER>());
            parser = parsers.back().get();
        }
        MNODE_LINK_DESC desc = {name, &PortOps, ports.back().get(), flags, neighbor, bootstrap, false, parser};
        return MNode_AddLink(mnode.get(), &desc);
    }

    Port *port(MNODE_LINK link) { return static_cast<Port *>(mnode->Links[link].Ctx); }

    int count(MNODE_EVENT_KIND kind) const
    {
        int n = 0;
        for (const Event &e : events)
            n += e.kind == kind;
        return n;
    }
};

// Соединить два линка проводом и поднять их.
void wire(Net &net, Node &a, MNODE_LINK la, Node &b, MNODE_LINK lb)
{
    Port *pa = a.port(la);
    Port *pb = b.port(lb);
    pa->net = pb->net = &net;
    pa->peer = b.get();
    pa->peerLink = lb;
    pa->peerStream = (b->Links[lb].Flags & MNODE_LINK_STREAM) != 0;
    pb->peer = a.get();
    pb->peerLink = la;
    pb->peerStream = (a->Links[la].Flags & MNODE_LINK_STREAM) != 0;
    MNode_SetLinkUp(a.get(), la, true);
    MNode_SetLinkUp(b.get(), lb, true);
    net.pump();
}

void cut(Net &net, Node &a, MNODE_LINK la, Node &b, MNODE_LINK lb)
{
    a.port(la)->up = false;
    b.port(lb)->up = false;
    MNode_SetLinkUp(a.get(), la, false);
    MNode_SetLinkUp(b.get(), lb, false);
    net.pump();
}

bool hasRoute(Node &n, MNET_NODE dst, MNODE_LINK link, MNODE_ROUTE_KIND kind)
{
    for (uint32_t i = 0; i < MNODE_MAX_ROUTES; i++) {
        MNODE_ROUTE_INFO info;
        if (MNode_GetRoute(n.get(), i, &info) && info.Node == dst)
            return info.Link == link && info.Kind == kind;
    }
    return false;
}

bool known(Node &n, MNET_NODE dst)
{
    for (uint32_t i = 0; i < MNODE_MAX_ROUTES; i++) {
        MNODE_ROUTE_INFO info;
        if (MNode_GetRoute(n.get(), i, &info) && info.Node == dst)
            return true;
    }
    return false;
}

// Endpoint, который копит принятые пакеты.
struct Inbox
{
    std::vector<Bytes> packets;
    std::vector<MNODE_LINK> links;

    static void handler(void *ctx, const MNET_HEADER *packet, MNODE_LINK from)
    {
        auto *inbox = static_cast<Inbox *>(ctx);
        const auto *bytes = reinterpret_cast<const uint8_t *>(packet);
        inbox->packets.emplace_back(bytes, bytes + MNet_PacketSize(packet));
        inbox->links.push_back(from);
    }

    const MNET_HEADER &header(size_t i) const { return *reinterpret_cast<const MNET_HEADER *>(packets[i].data()); }
    std::string text(size_t i) const
    {
        const MNET_HEADER &h = header(i);
        return std::string(reinterpret_cast<const char *>(packets[i].data() + sizeof(MNET_HEADER)), h.Length);
    }
    const MNET_RPC_HEADER &rpc(size_t i) const
    {
        return *reinterpret_cast<const MNET_RPC_HEADER *>(packets[i].data() + sizeof(MNET_HEADER));
    }
};

// rpc-служба эха: отвечает тем же телом.
struct Echo
{
    MNODE *node;
    MNET_ENDPOINT number;
    int calls = 0;

    static void handler(void *ctx, const MNET_HEADER *packet, MNODE_LINK from)
    {
        auto *echo = static_cast<Echo *>(ctx);
        echo->calls++;
        const uint8_t *body = MNet_Payload(packet) + sizeof(MNET_RPC_HEADER);
        MNode_Reply(echo->node, echo->number, packet, from, MNET_OK, body, packet->Length - sizeof(MNET_RPC_HEADER));
    }
};

MNODE_STATUS rpcCall(Node &from, MNET_ENDPOINT src, MNET_NODE dst, MNET_ENDPOINT ep, uint32_t id, const std::string &body)
{
    MNET_RPC_HEADER rpc = {id, 1, 0, 0};
    return MNode_Send(from.get(), src, dst, ep, MNET_PROTO_RPC, &rpc, sizeof(rpc), body.data(), uint32_t(body.size()));
}

constexpr uint32_t Leaf = MNODE_LINK_DEFAULT | MNODE_LINK_TRUST | MNODE_LINK_ANNOUNCE;
constexpr uint32_t GwPort = MNODE_LINK_TRUST | MNODE_LINK_LEARN;
constexpr uint32_t HvExternal = MNODE_LINK_TRUST | MNODE_LINK_ANNOUNCE | MNODE_LINK_ADOPT;
constexpr uint32_t GwCom = MNODE_LINK_STREAM | MNODE_LINK_TRUST | MNODE_LINK_ADOPT;
const MNET_ENDPOINT EchoEp = 1500;

/*
    Стенд из одного шлюза:
        hv (1.1) --COM-- gw (1.5) --tcp--> dbg (1.3)
                                  --tcp--> tool (1.50)
    hv ведёт себя как ядро: сам регистрируется на «внешнем» линке, его службы — hv:echo.
*/
struct Bench
{
    Net net;
    Node hv{MNET_NODE_HV};
    Node gw{addr(5)};
    Node dbg{MNET_NODE_DEV};
    Node tool{addr(50)};
    MNODE_LINK hvCom, gwCom, gwDbg, dbgGw, gwTool, toolGw;
    Echo hvEcho{hv.get(), EchoEp};
    Echo toolEcho{tool.get(), EchoEp};

    Bench()
    {
        MNode_Open(hv.get(), EchoEp, Echo::handler, &hvEcho);
        MNode_Open(tool.get(), EchoEp, Echo::handler, &toolEcho);
        hvCom = hv.add("COM1", MNODE_LINK_STREAM | HvExternal);
        gwCom = gw.add("com", GwCom, MNET_NODE_HV, true);
        gwDbg = gw.add("tcp#1", GwPort);
        dbgGw = dbg.add("gw", Leaf);
        gwTool = gw.add("tcp#2", GwPort);
        toolGw = tool.add("gw", Leaf);
    }

    void up()
    {
        wire(net, hv, hvCom, gw, gwCom);
        wire(net, dbg, dbgGw, gw, gwDbg);
        wire(net, tool, toolGw, gw, gwTool);
    }
};

void testRegistrationAndDiscovery()
{
    Bench b;
    // До регистрации hv за COM — только предположение шлюза.
    CHECK(hasRoute(b.gw, MNET_NODE_HV, b.gwCom, MNODE_ROUTE_BOOTSTRAP));
    b.up();

    // Шлюз узнал hv и обоих клиентов по их REGISTER, а не по типу линка.
    CHECK(hasRoute(b.gw, MNET_NODE_HV, b.gwCom, MNODE_ROUTE_NEIGHBOR));
    CHECK(hasRoute(b.gw, MNET_NODE_DEV, b.gwDbg, MNODE_ROUTE_NEIGHBOR));
    CHECK(hasRoute(b.gw, addr(50), b.gwTool, MNODE_ROUTE_NEIGHBOR));
    CHECK(b.gw->Links[b.gwCom].Subscribed);

    // hv получил таблицу шлюза: сам шлюз, отладчик и программа — через COM.
    CHECK(hasRoute(b.hv, addr(5), b.hvCom, MNODE_ROUTE_NEIGHBOR));
    CHECK(hasRoute(b.hv, MNET_NODE_DEV, b.hvCom, MNODE_ROUTE_REMOTE));
    CHECK(hasRoute(b.hv, addr(50), b.hvCom, MNODE_ROUTE_REMOTE));
    CHECK(b.hv->Links[b.hvCom].Sync == MNODE_SYNC_DONE);

    // Отладчик видит через шлюз hv и программу; себя в таблице шлюза для себя не видит.
    CHECK(hasRoute(b.dbg, MNET_NODE_HV, b.dbgGw, MNODE_ROUTE_REMOTE));
    CHECK(hasRoute(b.dbg, addr(50), b.dbgGw, MNODE_ROUTE_REMOTE));
    CHECK(hasRoute(b.dbg, addr(5), b.dbgGw, MNODE_ROUTE_NEIGHBOR));
    CHECK(!known(b.dbg, MNET_NODE_DEV));
    for (uint32_t i = 0; i < MNODE_MAX_ROUTES; i++) {
        MNODE_ROUTE_INFO info;
        if (MNode_GetRoute(b.dbg.get(), i, &info) && info.Node == MNET_NODE_HV)
            CHECK(info.Hops == 2);
    }
}

void testForwardingBothWays()
{
    Bench b;
    b.up();
    Inbox inbox;
    MNET_ENDPOINT ep = MNode_Open(b.dbg.get(), MNET_ENDPOINT_NONE, Inbox::handler, &inbox);
    CHECK(ep == MNET_ENDPOINT_DYNAMIC_FIRST);

    // Debugger -> TCP -> Gateway -> COM -> HV и ответ обратно тем же механизмом.
    CHECK(rpcCall(b.dbg, ep, MNET_NODE_HV, EchoEp, 7, "ping hv") == MNODE_OK);
    b.net.pump();
    CHECK(b.hvEcho.calls == 1);
    CHECK(inbox.packets.size() == 1);
    if (inbox.packets.size() == 1) {
        CHECK(inbox.header(0).SrcNode == MNET_NODE_HV);
        CHECK(inbox.rpc(0).RequestId == 7);
        CHECK(inbox.rpc(0).Flags & MNET_RPC_RESPONSE);
        CHECK(inbox.text(0).substr(sizeof(MNET_RPC_HEADER)) == "ping hv");
        CHECK(inbox.header(0).HopLimit == MNET_DEFAULT_HOP_LIMIT - 1);
    }
    CHECK(b.gw->Stats.Forwarded == 2);

    // HV -> Gateway -> TCP node и обратно.
    Inbox hvInbox;
    MNET_ENDPOINT hvEp = MNode_Open(b.hv.get(), MNET_ENDPOINT_NONE, Inbox::handler, &hvInbox);
    CHECK(rpcCall(b.hv, hvEp, addr(50), EchoEp, 8, "ping tool") == MNODE_OK);
    b.net.pump();
    CHECK(b.toolEcho.calls == 1);
    CHECK(hvInbox.packets.size() == 1);
    if (!hvInbox.packets.empty())
        CHECK(hvInbox.header(0).SrcNode == addr(50));

    // В пакете только адреса: на COM и на TCP он тот же, шлюз меняет лишь HopLimit и HeaderCrc.
    const Bytes &onCom = b.hv.port(b.hvCom)->sent.back();
    const Bytes &onTcp = b.gw.port(b.gwTool)->sent.back();
    CHECK(onCom.size() == onTcp.size());
    if (onCom.size() == onTcp.size() && onCom.size() > sizeof(MNET_HEADER)) {
        CHECK(std::memcmp(onCom.data() + sizeof(MNET_HEADER), onTcp.data() + sizeof(MNET_HEADER),
                          onCom.size() - sizeof(MNET_HEADER)) == 0);
        CHECK(onTcp[offsetof(MNET_HEADER, HopLimit)] == onCom[offsetof(MNET_HEADER, HopLimit)] - 1);
    }
}

void testRefusals()
{
    Bench b;
    b.up();
    Inbox inbox;
    MNET_ENDPOINT ep = MNode_Open(b.dbg.get(), MNET_ENDPOINT_NONE, Inbox::handler, &inbox);

    // Узла нет нигде: шлюз отвечает NO_ROUTE сам, не дожидаясь таймаута.
    CHECK(rpcCall(b.dbg, ep, addr(77), EchoEp, 1, "x") == MNODE_OK);
    b.net.pump();
    CHECK(inbox.packets.size() == 1);
    if (!inbox.packets.empty()) {
        CHECK(inbox.rpc(0).Status == MNET_E_NO_ROUTE);
        CHECK(inbox.header(0).SrcNode == addr(5));
    }

    // Узел есть, endpoint'а нет: отвечает сам узел.
    CHECK(rpcCall(b.dbg, ep, MNET_NODE_HV, 999, 2, "x") == MNODE_OK);
    b.net.pump();
    CHECK(inbox.packets.size() == 2);
    if (inbox.packets.size() == 2)
        CHECK(inbox.rpc(1).Status == MNET_E_NO_ENDPOINT);

    // На datagram — молча.
    MNode_Send(b.dbg.get(), ep, addr(77), EchoEp, MNET_PROTO_DATAGRAM, nullptr, 0, "d", 1);
    b.net.pump();
    CHECK(inbox.packets.size() == 2);

    // COM закрыт: rpc к hv получает NO_ROUTE от шлюза; маршрут-предположение к hv остался.
    b.hv.port(b.hvCom)->up = false;
    b.gw.port(b.gwCom)->up = false;
    MNode_SetLinkUp(b.gw.get(), b.gwCom, false);
    b.net.pump();
    CHECK(hasRoute(b.gw, MNET_NODE_HV, b.gwCom, MNODE_ROUTE_BOOTSTRAP));
    CHECK(rpcCall(b.dbg, ep, MNET_NODE_HV, EchoEp, 3, "x") == MNODE_OK);
    b.net.pump();
    CHECK(inbox.packets.size() == 3);
    if (inbox.packets.size() == 3)
        CHECK(inbox.rpc(2).Status == MNET_E_NO_ROUTE);
}

void testSourceCheck()
{
    Bench b;
    b.up();
    Inbox inbox;
    MNode_Open(b.hv.get(), 3000, Inbox::handler, &inbox);

    // Программа на tcp#2 выдаёт себя за отладчик: пакет отброшен, отладчик — за tcp#1.
    Bytes spoof(sizeof(MNET_HEADER) + 1);
    auto *h = reinterpret_cast<MNET_HEADER *>(spoof.data());
    MNet_HeaderInit(h, MNET_PROTO_DATAGRAM, MNET_NODE_HV, 3000, MNET_NODE_DEV, 1, 1);
    spoof[sizeof(MNET_HEADER)] = 'x';
    MNet_Seal(h);
    MNode_InputPacket(b.gw.get(), b.gwTool, spoof.data(), uint32_t(spoof.size()));
    b.net.pump();
    CHECK(inbox.packets.empty());
    CHECK(b.gw->Stats.BadSource == 1);
    CHECK(b.gw.count(MNODE_EVENT_BAD_SOURCE) == 1);

    // Неизвестный адрес с TCP тоже не принимается: «узлы за соседом» без объявления — только на COM (ADOPT).
    MNet_HeaderInit(h, MNET_PROTO_DATAGRAM, MNET_NODE_HV, 3000, addr(9), 1, 1);
    MNet_Seal(h);
    MNode_InputPacket(b.gw.get(), b.gwTool, spoof.data(), uint32_t(spoof.size()));
    b.net.pump();
    CHECK(inbox.packets.empty());
    CHECK(!known(b.gw, addr(9)));

    // С COM — принимается как узел за hv, и шлюз сверяет таблицу с hv.
    MNet_HeaderInit(h, MNET_PROTO_DATAGRAM, addr(5), 3000, addr(0x109), 1, 1);
    MNet_Seal(h);
    Inbox gwInbox;
    MNode_Open(b.gw.get(), 3000, Inbox::handler, &gwInbox);
    MNode_InputBytes(b.gw.get(), b.gwCom, spoof.data(), uint32_t(spoof.size()));
    CHECK(gwInbox.packets.size() == 1);
    CHECK(hasRoute(b.gw, addr(0x109), b.gwCom, MNODE_ROUTE_LEARNED));
    MNode_Poll(b.gw.get(), MNODE_LEARN_MIN_MS);
    b.net.pump();
    CHECK(!known(b.gw, addr(0x109)));   // hv такого узла не знает — таблица hv его сняла

    // Регистрация чужого адреса через другой линк отклоняется.
    Node impostor{MNET_NODE_DEV};
    MNODE_LINK impostorGw = impostor.add("gw", Leaf);
    MNODE_LINK gwImpostor = b.gw.add("tcp#3", GwPort);
    wire(b.net, impostor, impostorGw, b.gw, gwImpostor);
    CHECK(b.gw.count(MNODE_EVENT_DENIED) == 1);
    CHECK(hasRoute(b.gw, MNET_NODE_DEV, b.gwDbg, MNODE_ROUTE_NEIGHBOR));
    CHECK(impostor->Links[impostorGw].Sync == MNODE_SYNC_UNSUPPORTED);
}

void testTakenAddressReported()
{
    // Узел из объявления соседа с адресом, занятым за другим линком, не принимается — и об этом
    // сообщается: иначе он виден в таблицах, а его пакеты молча отбрасываются (BAD_SOURCE).
    Bench b;
    b.up();
    Node hub{addr(7)};
    MNODE_LINK hubGw = hub.add("gw", Leaf);
    MNODE_LINK hubDev = hub.add("dev", 0, MNET_NODE_DEV);   // за шлюзом Dbg — свой dev
    MNODE_LINK gwHub = b.gw.add("tcp#3", GwPort);
    MNode_SetLinkUp(hub.get(), hubDev, true);
    wire(b.net, hub, hubGw, b.gw, gwHub);

    // Шлюз принял сам шлюз Dbg, но не его dev: 1.3 уже за tcp#1.
    CHECK(hasRoute(b.gw, addr(7), gwHub, MNODE_ROUTE_NEIGHBOR));
    CHECK(hasRoute(b.gw, MNET_NODE_DEV, b.gwDbg, MNODE_ROUTE_NEIGHBOR));
    int gwDenied = 0;
    for (const Event &e : b.gw.events)
        gwDenied += e.kind == MNODE_EVENT_DENIED && e.node == MNET_NODE_DEV && e.link == gwHub;
    CHECK(gwDenied >= 1);

    // Шлюз Dbg тоже знает о конфликте: в таблице шлюза 1.3 — за другим его соседом.
    int hubDenied = 0;
    for (const Event &e : hub.events)
        hubDenied += e.kind == MNODE_EVENT_DENIED && e.node == MNET_NODE_DEV && e.link == hubGw;
    CHECK(hubDenied >= 1);
}

void testDisconnectCleansRoutes()
{
    Bench b;
    b.up();
    CHECK(known(b.hv, addr(50)));
    CHECK(known(b.dbg, addr(50)));

    // Отключение программы: шлюз снимает её маршрут и сразу сообщает подписчикам.
    MNode_RemoveLink(b.gw.get(), b.gwTool);
    b.net.pump();
    CHECK(!known(b.gw, addr(50)));
    CHECK(!known(b.hv, addr(50)));
    CHECK(!known(b.dbg, addr(50)));
    CHECK(known(b.hv, MNET_NODE_DEV));

    // Повторное подключение под тем же адресом — снова регистрация.
    Node again{addr(50)};
    MNODE_LINK againGw = again.add("gw", Leaf);
    MNODE_LINK gwAgain = b.gw.add("tcp#4", GwPort);
    wire(b.net, again, againGw, b.gw, gwAgain);
    CHECK(hasRoute(b.gw, addr(50), gwAgain, MNODE_ROUTE_NEIGHBOR));
    CHECK(known(b.hv, addr(50)));

    // COM упал: узлы hv снимаются, предположение «за COM — hv» остаётся до нового подъёма.
    cut(b.net, b.hv, b.hvCom, b.gw, b.gwCom);
    CHECK(hasRoute(b.gw, MNET_NODE_HV, b.gwCom, MNODE_ROUTE_BOOTSTRAP));
    CHECK(!known(b.hv, MNET_NODE_DEV));
    CHECK(!known(b.dbg, MNET_NODE_HV));
}

void testHvNodesAnnounced()
{
    // VM за hv (внутренний линк) объявляются шлюзу; узел, вставший позже, — сразу, без опроса.
    Bench b;
    Node vm1{addr(0x101)};
    MNODE_LINK hvVm1 = b.hv.add("ring1", 0, addr(0x101));
    MNODE_LINK vm1Hv = vm1.add("ring", MNODE_LINK_DEFAULT);
    b.up();
    wire(b.net, b.hv, hvVm1, vm1, vm1Hv);
    CHECK(hasRoute(b.gw, addr(0x101), b.gwCom, MNODE_ROUTE_REMOTE));
    CHECK(hasRoute(b.dbg, addr(0x101), b.dbgGw, MNODE_ROUTE_REMOTE));

    Node vm2{addr(0x102)};
    MNODE_LINK hvVm2 = b.hv.add("ring2", 0, addr(0x102));
    MNODE_LINK vm2Hv = vm2.add("ring", MNODE_LINK_DEFAULT);
    wire(b.net, b.hv, hvVm2, vm2, vm2Hv);
    CHECK(hasRoute(b.dbg, addr(0x102), b.dbgGw, MNODE_ROUTE_REMOTE));

    // Отладчик достаёт VM через шлюз и hv.
    Echo vmEcho{vm2.get(), EchoEp};
    MNode_Open(vm2.get(), EchoEp, Echo::handler, &vmEcho);
    Inbox inbox;
    MNET_ENDPOINT ep = MNode_Open(b.dbg.get(), MNET_ENDPOINT_NONE, Inbox::handler, &inbox);
    rpcCall(b.dbg, ep, addr(0x102), EchoEp, 9, "vm");
    b.net.pump();
    CHECK(vmEcho.calls == 1);
    CHECK(inbox.packets.size() == 1);
    if (!inbox.packets.empty())
        CHECK(inbox.header(0).HopLimit == MNET_DEFAULT_HOP_LIMIT - 2);
}

void testGatewayChain()
{
    // HV -> Gateway A -> Gateway B -> Server: тот же механизм, без особой роли шлюза.
    Net net;
    Node hv{MNET_NODE_HV}, a{addr(5)}, bgw{addr(7)}, server{addr(200)};
    MNODE_LINK hvA = hv.add("COM1", MNODE_LINK_STREAM | HvExternal);
    MNODE_LINK aHv = a.add("com", GwCom, MNET_NODE_HV, true);
    MNODE_LINK aB = a.add("tcp#1", GwPort);
    MNODE_LINK bA = bgw.add("uplink", MNODE_LINK_TRUST | MNODE_LINK_ANNOUNCE);
    MNODE_LINK bS = bgw.add("tcp#1", GwPort);
    MNODE_LINK sB = server.add("gw", Leaf);
    Echo echo{server.get(), EchoEp};
    MNode_Open(server.get(), EchoEp, Echo::handler, &echo);

    wire(net, hv, hvA, a, aHv);
    wire(net, bgw, bA, a, aB);
    wire(net, server, sB, bgw, bS);

    CHECK(hasRoute(a, addr(200), aB, MNODE_ROUTE_REMOTE));
    CHECK(hasRoute(hv, addr(200), hvA, MNODE_ROUTE_REMOTE));
    CHECK(hasRoute(server, MNET_NODE_HV, sB, MNODE_ROUTE_REMOTE));

    Inbox inbox;
    MNET_ENDPOINT ep = MNode_Open(hv.get(), MNET_ENDPOINT_NONE, Inbox::handler, &inbox);
    rpcCall(hv, ep, addr(200), EchoEp, 11, "far");
    net.pump();
    CHECK(echo.calls == 1);
    CHECK(inbox.packets.size() == 1);
    if (!inbox.packets.empty())
        CHECK(inbox.header(0).HopLimit == MNET_DEFAULT_HOP_LIMIT - 2);
}

void testHubThroughExternalGateway()
{
    // Шлюз Dbg (1.7) в режиме «через шлюз»: его COM закрыт (за ним лишь предположение «hv»), наверх —
    // TCP к внешнему шлюзу (1.5), у которого COM к hv. Отладчик dev — за шлюзом Dbg.
    Net net;
    Node hv{MNET_NODE_HV}, ext{addr(5)}, hub{addr(7)}, dev{MNET_NODE_DEV};
    MNODE_LINK hvCom = hv.add("COM1", MNODE_LINK_STREAM | HvExternal);
    MNODE_LINK extCom = ext.add("com", GwCom, MNET_NODE_HV, true);
    MNODE_LINK extTcp = ext.add("tcp#1", GwPort);
    MNODE_LINK hubCom = hub.add("com", GwCom, MNET_NODE_HV, true);
    MNODE_LINK hubUp = hub.add("gw", Leaf);
    MNODE_LINK hubDev = hub.add("dev", GwPort);
    MNODE_LINK devUp = dev.add("gw", Leaf);
    Echo echo{hv.get(), EchoEp};
    MNode_Open(hv.get(), EchoEp, Echo::handler, &echo);

    wire(net, hv, hvCom, ext, extCom);
    wire(net, dev, devUp, hub, hubDev);
    // Пока внешний шлюз не подключён, к hv дороги нет: COM шлюза Dbg закрыт.
    Inbox inbox;
    MNET_ENDPOINT ep = MNode_Open(dev.get(), MNET_ENDPOINT_NONE, Inbox::handler, &inbox);
    rpcCall(dev, ep, MNET_NODE_HV, EchoEp, 1, "x");
    net.pump();
    CHECK(inbox.packets.size() == 1);
    if (!inbox.packets.empty())
        CHECK(inbox.rpc(0).Status == MNET_E_NO_ROUTE);

    // Наверх — внешний шлюз: шлюз Dbg регистрирует себя и dev, получает hv из таблицы внешнего.
    wire(net, hub, hubUp, ext, extTcp);
    CHECK(hasRoute(hub, MNET_NODE_HV, hubUp, MNODE_ROUTE_REMOTE));
    CHECK(hasRoute(ext, MNET_NODE_DEV, extTcp, MNODE_ROUTE_REMOTE));
    CHECK(hasRoute(hv, MNET_NODE_DEV, hvCom, MNODE_ROUTE_REMOTE));
    CHECK(hasRoute(dev, MNET_NODE_HV, devUp, MNODE_ROUTE_REMOTE));

    rpcCall(dev, ep, MNET_NODE_HV, EchoEp, 2, "via gateway");
    net.pump();
    CHECK(echo.calls == 1);
    CHECK(inbox.packets.size() == 2);
    if (inbox.packets.size() == 2) {
        CHECK(inbox.rpc(1).Status == MNET_OK);
        CHECK(inbox.header(1).HopLimit == MNET_DEFAULT_HOP_LIMIT - 2);
    }

    // Обратно в прямой режим: внешний шлюз отключён, COM поднят — снова предположение «за COM — hv».
    cut(net, hub, hubUp, ext, extTcp);
    CHECK(!known(ext, MNET_NODE_DEV));
    MNode_SetLinkUp(hub.get(), hubCom, true);
    CHECK(hasRoute(hub, MNET_NODE_HV, hubCom, MNODE_ROUTE_BOOTSTRAP));
}

void testLegacyClientLearned()
{
    // Программа без службy route (старый переходник wsl): соседом линка её делает первый пакет.
    Bench b;
    b.up();
    Inbox inbox;
    MNode_Open(b.hv.get(), 3001, Inbox::handler, &inbox);
    MNODE_LINK gwLegacy = b.gw.add("tcp#9", GwPort);
    MNode_SetLinkUp(b.gw.get(), gwLegacy, true);

    Bytes packet(sizeof(MNET_HEADER) + 2);
    auto *h = reinterpret_cast<MNET_HEADER *>(packet.data());
    MNet_HeaderInit(h, MNET_PROTO_DATAGRAM, MNET_NODE_HV, 3001, MNET_NODE_WSL, 1500, 2);
    std::memcpy(packet.data() + sizeof(MNET_HEADER), "hi", 2);
    MNet_Seal(h);
    MNode_InputPacket(b.gw.get(), gwLegacy, packet.data(), uint32_t(packet.size()));
    b.net.pump();
    CHECK(hasRoute(b.gw, MNET_NODE_WSL, gwLegacy, MNODE_ROUTE_NEIGHBOR));
    CHECK(inbox.packets.size() == 1);
    // hv узнал wsl из рассылки шлюза раньше, чем пришёл пакет, — а не по самому пакету.
    CHECK(hasRoute(b.hv, MNET_NODE_WSL, b.hvCom, MNODE_ROUTE_REMOTE));
    CHECK(b.hv->Stats.RouteLearned == 0);
}

void testStreamAndRaw()
{
    // Потоковый линк: мусор и текст вне пакетов уходят событием RAW, пакеты разбираются по байту.
    Node gw{addr(5)};
    MNODE_LINK com = gw.add("com", GwCom, MNET_NODE_HV, true);
    MNode_SetLinkUp(gw.get(), com, true);
    Inbox inbox;
    MNode_Open(gw.get(), 3002, Inbox::handler, &inbox);

    Bytes packet(sizeof(MNET_HEADER) + 3);
    auto *h = reinterpret_cast<MNET_HEADER *>(packet.data());
    MNet_HeaderInit(h, MNET_PROTO_DATAGRAM, addr(5), 3002, MNET_NODE_HV, 3, 3);
    std::memcpy(packet.data() + sizeof(MNET_HEADER), "log", 3);
    MNet_Seal(h);

    const std::string text = "UEFI boot\n";
    MNode_InputBytes(gw.get(), com, text.data(), uint32_t(text.size()));
    for (uint8_t byte : packet)
        MNode_InputBytes(gw.get(), com, &byte, 1);
    CHECK(inbox.packets.size() == 1);
    std::string raw;
    for (const Event &e : gw.events)
        if (e.kind == MNODE_EVENT_RAW)
            raw += e.data;
    CHECK(raw == text);

    // Пакет с паузой после заголовка (UART под QEMU занят) не разрывается: заголовок целый — это пакет.
    MNode_Poll(gw.get(), 100);
    MNode_InputBytes(gw.get(), com, packet.data(), uint32_t(sizeof(MNET_HEADER) + 1));
    MNode_Poll(gw.get(), 100 + MNODE_STREAM_IDLE_MS * 5);
    MNode_InputBytes(gw.get(), com, packet.data() + sizeof(MNET_HEADER) + 1,
                     uint32_t(packet.size() - sizeof(MNET_HEADER) - 1));
    CHECK(inbox.packets.size() == 2);

    // Недописанная строка выходит как RAW, когда поток замолчал.
    MNode_InputBytes(gw.get(), com, "P", 1);
    MNode_Poll(gw.get(), 2000);
    raw.clear();
    for (const Event &e : gw.events)
        if (e.kind == MNODE_EVENT_RAW)
            raw += e.data;
    CHECK(raw == text + "P");
}

void testAnnounceRetries()
{
    // Сосед молчит — REGISTER повторяется через 1, 2, 4, 8, 8 с; ответил — раз в 15 с.
    Node hv{MNET_NODE_HV};
    MNODE_LINK com = hv.add("COM1", HvExternal);
    MNode_Poll(hv.get(), 0);
    MNode_SetLinkUp(hv.get(), com, true);
    Port *port = hv.port(com);
    CHECK(port->sent.size() == 1);
    const uint32_t times[] = {999, 1000, 2999, 3000, 6999, 7000, 14999, 15000, 23000};
    const size_t expected[] = {1, 2, 2, 3, 3, 4, 4, 5, 6};
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        MNode_Poll(hv.get(), times[i]);
        CHECK(port->sent.size() == expected[i]);
    }
    const auto *h = reinterpret_cast<const MNET_HEADER *>(port->sent.back().data());
    CHECK(h->DstNode == MNET_NODE_LINK);
    CHECK(h->DstEndpoint == MNET_ENDPOINT_ROUTE);
}

void testPagedTable()
{
    // Большая таблица отдаётся страницами GET; отладчик собирает все записи.
    Bench b;
    b.up();
    MNODE_LINK fake = b.gw.add("many", 0);
    MNode_SetLinkUp(b.gw.get(), fake, true);
    for (uint16_t i = 0; i < 100; i++)
        MNode_AddRoute(b.gw.get(), addr(uint16_t(1000 + i)), fake, false);
    b.net.pump();
    int seen = 0;
    for (uint16_t i = 0; i < 100; i++)
        seen += known(b.dbg, addr(uint16_t(1000 + i)));
    CHECK(seen == 100);

    // Таблица страницей: GET со Start, ответ с Next.
    struct Pager
    {
        std::vector<Bytes> answers;
        static void handler(void *ctx, const MNET_HEADER *packet, MNODE_LINK)
        {
            const auto *bytes = reinterpret_cast<const uint8_t *>(packet);
            static_cast<Pager *>(ctx)->answers.emplace_back(bytes, bytes + MNet_PacketSize(packet));
        }
    } pager;
    MNET_ENDPOINT ep = MNode_Open(b.dbg.get(), MNET_ENDPOINT_NONE, Pager::handler, &pager);
    MNET_RPC_HEADER rpc = {77, MNET_ROUTE_GET, 0, 0};
    MNET_ROUTE_GET_REQUEST get = {100};
    MNode_SendLink(b.dbg.get(), b.dbgGw, ep, MNET_ENDPOINT_ROUTE, MNET_PROTO_RPC, &rpc, sizeof(rpc), &get, sizeof(get));
    b.net.pump();
    CHECK(pager.answers.size() == 1);
    if (!pager.answers.empty()) {
        const auto *h = reinterpret_cast<const MNET_HEADER *>(pager.answers[0].data());
        CHECK(h->DstNode == MNET_NODE_LINK);
        MNET_ROUTE_TABLE table;
        std::memcpy(&table, pager.answers[0].data() + sizeof(MNET_HEADER) + sizeof(MNET_RPC_HEADER), sizeof(table));
        CHECK(table.Next == 0);
        CHECK(table.EntrySize == sizeof(MNET_ROUTE_ENTRY));
        CHECK(table.Count == 103 - 100); // gw, hv, tool и 100 узлов «many» — 103 записи, со 100-й — 3
    }
}

} // namespace

int main()
{
    struct Case
    {
        const char *name;
        void (*run)();
    } cases[] = {
        {"registrationAndDiscovery", testRegistrationAndDiscovery},
        {"forwardingBothWays", testForwardingBothWays},
        {"refusals", testRefusals},
        {"sourceCheck", testSourceCheck},
        {"takenAddressReported", testTakenAddressReported},
        {"disconnectCleansRoutes", testDisconnectCleansRoutes},
        {"hvNodesAnnounced", testHvNodesAnnounced},
        {"gatewayChain", testGatewayChain},
        {"hubThroughExternalGateway", testHubThroughExternalGateway},
        {"legacyClientLearned", testLegacyClientLearned},
        {"streamAndRaw", testStreamAndRaw},
        {"announceRetries", testAnnounceRetries},
        {"pagedTable", testPagedTable},
    };
    for (const Case &c : cases) {
        const int before = g_failures;
        c.run();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", c.name);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
