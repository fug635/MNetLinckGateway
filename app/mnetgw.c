/*
    mnetgw — шлюз MNet: обычный узел MNet (core/MNode) с несколькими линками, который пересылает
    пакеты по DstNode. Своего протокола у шлюза нет — только служба route, как у любого узла.

    Линки:
        com — COM-порт к hv (или TCP-клиент к последовательному порту QEMU: --com tcp:host:port).
              Пока hv сам не зарегистрировался, шлюз предполагает, что за COM — hv (--com-neighbor);
              hv регистрируется REGISTER'ом и сообщает свои узлы (VM), предположение заменяется.
        tcp — сервер: каждое подключение — свой линк; программа регистрируется на нём как узел
              MNet (REGISTER со своим адресом). Старая программа без службы route узнаётся по
              адресу отправителя первого пакета. Отключение снимает её маршруты, и соседи узнают
              об этом сразу.

    Байты с COM вне пакетов (вывод загрузчика, EarlyIo, паника, текст Linux стенда с
    [TEST_STAND_READY]) печатаются в stdout как есть и уходят клиентам порта сырых байтов (--raw):
    только чтение, живой поток без истории — по нему контроллер стенда в Dbg видит Linux стенда.
    Сообщения шлюза — в stderr.
*/
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MNode.h"
#include "platform.h"

#define GW_DEFAULT_NODE   MNET_NODE_MAKE(MNET_NET_DEFAULT, 5)
#define GW_DEFAULT_LISTEN "127.0.0.1:8890"
#define GW_DEFAULT_RAW    "127.0.0.1:8892"
#define GW_RAW_CLIENTS    8
#define GW_NOTICE_MS      60000u  /* один и тот же отказ (вид, линк, узел) — в журнал не чаще раза в минуту */
#define GW_NOTICES        16
#define GW_TX_QUEUE       (256u * 1024u)
#define GW_READ_CHUNK     4096u
#define GW_REOPEN_MS      1000u
#define GW_WAIT_MS        20u

typedef struct _GW_CONN {
    PLAT_IO*    Io;
    MNODE_LINK  Link;
    bool        Com;
    bool        Failed;
    bool        RawNoted;
    char        Name[MNODE_LINK_NAME_MAX];
    MNET_PARSER Parser;
    uint8_t*    Tx;
    uint32_t    TxHead;
    uint32_t    TxTail;
} GW_CONN;

typedef struct _GW_OPTIONS {
    MNET_NODE   Node;
    const char* Com;
    uint32_t    Baud;
    const char* Listen;
    const char* Raw;
    MNET_NODE   ComNeighbor;
    bool        Verbose;
} GW_OPTIONS;

/* Отказ, который уже был в журнале: (вид, линк, узел) и когда. */
typedef struct _GW_SEEN {
    MNODE_EVENT_KIND Kind;
    MNODE_LINK       Link;
    MNET_NODE        Node;
    uint32_t         Ms;
} GW_SEEN;

static MNODE      g_Node;
static GW_OPTIONS g_Options;
static GW_CONN    g_Com;
static GW_CONN*   g_Conns[MNODE_MAX_LINKS];
static PLAT_IO*   g_Listener;
static PLAT_IO*   g_RawListener;
static GW_CONN*   g_Raw[GW_RAW_CLIENTS];   /* клиенты порта сырых байтов: линка узла у них нет */
static GW_SEEN    g_Seen[GW_NOTICES];

/* ---- Сообщения ------------------------------------------------------------------------------ */

static const char* GwNode(MNET_NODE node, char* text) {
    if (node == MNET_NODE_LINK) {
        strcpy(text, "link");
    } else {
        sprintf(text, "%u.%u", (unsigned)MNET_NODE_NET(node), (unsigned)MNET_NODE_LOCAL(node));
    }
    return text;
}

static const char* GwLinkName(MNODE_LINK link) {
    static char unknown[] = "?";
    return (link < MNODE_MAX_LINKS && g_Node.Links[link].Open) ? g_Node.Links[link].Name : unknown;
}

static void GwNotice(const char* format, ...) {
    va_list args;
    va_start(args, format);
    fputs("[mnetgw] ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(args);
}

static const char* GwKind(MNODE_ROUTE_KIND kind) {
    switch (kind) {
    case MNODE_ROUTE_BOOTSTRAP: return "bootstrap";
    case MNODE_ROUTE_NEIGHBOR:  return "neighbor";
    case MNODE_ROUTE_LEARNED:   return "learned";
    case MNODE_ROUTE_REMOTE:    return "remote";
    case MNODE_ROUTE_CONFIG:    break;
    }
    return "config";
}

static void GwPrintRoutes(void) {
    char a[16];
    GwNotice("routes of %s:", GwNode(g_Node.Self, a));
    for (uint32_t i = 0; i < MNODE_MAX_ROUTES; i++) {
        MNODE_ROUTE_INFO route;
        if (MNode_GetRoute(&g_Node, i, &route)) {
            GwNotice("  %-9s -> %s (%s, hops %u)", GwNode(route.Node, a), GwLinkName(route.Link), GwKind(route.Kind),
                     (unsigned)route.Hops);
        }
    }
}

/* Этот отказ (вид, линк, узел) уже был в журнале меньше GW_NOTICE_MS назад. */
static bool GwSeenRecently(const MNODE_EVENT* event) {
    uint32_t now    = Plat_NowMs() | 1u;   /* 0 — пустая запись */
    GW_SEEN* oldest = &g_Seen[0];
    for (uint32_t i = 0; i < GW_NOTICES; i++) {
        GW_SEEN* seen = &g_Seen[i];
        if (seen->Ms && seen->Kind == event->Kind && seen->Link == event->Link && seen->Node == event->Node) {
            if (now - seen->Ms < GW_NOTICE_MS) {
                return true;
            }
            seen->Ms = now;
            return false;
        }
        if (seen->Ms < oldest->Ms) {
            oldest = seen;
        }
    }
    oldest->Kind = event->Kind;
    oldest->Link = event->Link;
    oldest->Node = event->Node;
    oldest->Ms   = now;
    return false;
}

static void GwRawWrite(const uint8_t* data, uint32_t length);

static void GwEvent(void* ctx, const MNODE_EVENT* event) {
    (void)ctx;
    char node[16];
    switch (event->Kind) {
    case MNODE_EVENT_RAW:
        if (event->Link == g_Com.Link) {
            fwrite(event->Data, 1, event->Length, stdout);
            fflush(stdout);
            GwRawWrite(event->Data, event->Length);
        } else if (g_Conns[event->Link] && !g_Conns[event->Link]->RawNoted) {
            g_Conns[event->Link]->RawNoted = true;
            GwNotice("%s: bytes outside MNet packets are skipped", GwLinkName(event->Link));
        }
        break;
    case MNODE_EVENT_BAD_SOURCE:
        if (!GwSeenRecently(event)) {
            GwNotice("%s: packets from %s dropped: that node is not behind this link (repeats are not shown for "
                     "a minute)", GwLinkName(event->Link), GwNode(event->Node, node));
        }
        break;
    case MNODE_EVENT_REGISTERED:
        GwNotice("%s: node %s registered", GwLinkName(event->Link), GwNode(event->Node, node));
        break;
    case MNODE_EVENT_DENIED:
        // Тот, кто объявил узел, его пакеты сюда не доведёт: адрес уже у другого подключения.
        if (!GwSeenRecently(event)) {
            GwNotice("%s: node %s refused: address is taken behind another link; its packets through this link "
                     "are dropped", GwLinkName(event->Link), GwNode(event->Node, node));
        }
        break;
    case MNODE_EVENT_SYNC:
        GwNotice("%s: neighbor %s answered route exchange: %d", GwLinkName(event->Link), GwNode(event->Node, node),
                 (int)event->Status);
        break;
    case MNODE_EVENT_ROUTES:
        if (g_Options.Verbose) {
            GwPrintRoutes();
        }
        break;
    case MNODE_EVENT_DAMAGED:
        break;
    }
}

/* ---- Транспорт ------------------------------------------------------------------------------ */

static MNODE_TX GwSend(void* ctx, const void* packet, uint32_t size) {
    GW_CONN* conn = (GW_CONN*)ctx;
    if (!conn->Io || conn->Failed) {
        return MNODE_TX_DOWN;
    }
    if (GW_TX_QUEUE - conn->TxTail < size) {
        memmove(conn->Tx, conn->Tx + conn->TxHead, conn->TxTail - conn->TxHead);
        conn->TxTail -= conn->TxHead;
        conn->TxHead = 0;
        if (GW_TX_QUEUE - conn->TxTail < size) {
            return MNODE_TX_FULL;
        }
    }
    memcpy(conn->Tx + conn->TxTail, packet, size);
    conn->TxTail += size;
    return MNODE_TX_OK;
}

static const MNODE_LINK_OPS GwOps = {GwSend};

static void GwFlush(GW_CONN* conn) {
    while (conn->Io && !conn->Failed && conn->TxHead < conn->TxTail) {
        uint32_t left = conn->TxTail - conn->TxHead;
        int      sent = Plat_Write(conn->Io, conn->Tx + conn->TxHead, left < GW_READ_CHUNK ? left : GW_READ_CHUNK);
        if (sent < 0) {
            conn->Failed = true;
            return;
        }
        if (sent == 0) {
            return;
        }
        conn->TxHead += (uint32_t)sent;
    }
    if (conn->TxHead == conn->TxTail) {
        conn->TxHead = conn->TxTail = 0;
    }
}

static bool GwSplitHostPort(const char* text, char* host, size_t hostSize, uint16_t* port) {
    const char* colon = strrchr(text, ':');
    if (!colon || (size_t)(colon - text) >= hostSize) {
        return false;
    }
    memcpy(host, text, (size_t)(colon - text));
    host[colon - text] = 0;
    long value = strtol(colon + 1, NULL, 10);
    if (value <= 0 || value > 65535) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static void GwComOpen(void) {
    static char     lastError[256];
    char            error[256] = "";
    char            host[128];
    uint16_t        port;
    const char*     name = g_Options.Com;

    if (strncmp(name, "tcp:", 4) == 0) {
        g_Com.Io = GwSplitHostPort(name + 4, host, sizeof(host), &port) ? Plat_TcpConnect(host, port, error, sizeof(error))
                                                                         : NULL;
        if (!g_Com.Io && !error[0]) {
            snprintf(error, sizeof(error), "%s: expected tcp:host:port", name);
        }
    } else {
        g_Com.Io = Plat_SerialOpen(name, g_Options.Baud, error, sizeof(error));
    }

    if (!g_Com.Io) {
        if (strcmp(error, lastError) != 0) {
            GwNotice("com: %s, retrying every %u ms", error, GW_REOPEN_MS);
            strcpy(lastError, error);
        }
        return;
    }
    lastError[0]  = 0;
    g_Com.Failed  = false;
    g_Com.TxHead  = g_Com.TxTail = 0;
    GwNotice("com: %s open", name);
    MNode_SetLinkUp(&g_Node, g_Com.Link, true);
}

static void GwComClose(const char* why) {
    GwNotice("com: %s closed: %s", g_Options.Com, why);
    Plat_Close(g_Com.Io);
    g_Com.Io = NULL;
    MNode_SetLinkUp(&g_Node, g_Com.Link, false);
}

static void GwAccept(void) {
    char     peer[64];
    PLAT_IO* io;
    while (g_Listener && (io = Plat_TcpAccept(g_Listener, peer, sizeof(peer))) != NULL) {
        GW_CONN* conn = calloc(1, sizeof(*conn));
        conn->Io      = io;
        conn->Tx      = malloc(GW_TX_QUEUE);
        snprintf(conn->Name, sizeof(conn->Name), "tcp:%.27s", peer);

        // Узел на подключении сам говорит, кто он (REGISTER), или узнаётся по первому пакету (LEARN).
        MNODE_LINK_DESC desc = {conn->Name, &GwOps, conn, MNODE_LINK_STREAM | MNODE_LINK_TRUST | MNODE_LINK_LEARN,
                                MNET_NODE_NONE, false, true, &conn->Parser};
        conn->Link = MNode_AddLink(&g_Node, &desc);
        if (conn->Link == MNODE_LINK_NONE) {
            GwNotice("%s: refused, no free links", conn->Name);
            Plat_Close(io);
            free(conn->Tx);
            free(conn);
            continue;
        }
        g_Conns[conn->Link] = conn;
        GwNotice("%s: connected", conn->Name);
    }
}

/* ---- Порт сырых байтов ---------------------------------------------------------------------- */

static void GwRawWrite(const uint8_t* data, uint32_t length) {
    for (uint32_t i = 0; i < GW_RAW_CLIENTS; i++) {
        GW_CONN* client = g_Raw[i];
        // Клиент не успевает читать: байты для него теряются, о потере — один раз.
        if (client && GwSend(client, data, length) == MNODE_TX_FULL && !client->RawNoted) {
            client->RawNoted = true;
            GwNotice("%s: does not keep up, raw bytes are lost", client->Name);
        }
    }
}

static void GwRawAccept(void) {
    char     peer[64];
    PLAT_IO* io;
    while (g_RawListener && (io = Plat_TcpAccept(g_RawListener, peer, sizeof(peer))) != NULL) {
        GW_CONN** slot = NULL;
        for (uint32_t i = 0; i < GW_RAW_CLIENTS && !slot; i++) {
            if (!g_Raw[i]) {
                slot = &g_Raw[i];
            }
        }
        if (!slot) {
            GwNotice("raw:%s: refused, %u clients are connected already", peer, GW_RAW_CLIENTS);
            Plat_Close(io);
            continue;
        }
        GW_CONN* client = calloc(1, sizeof(*client));
        client->Io      = io;
        client->Link    = MNODE_LINK_NONE;
        client->Tx      = malloc(GW_TX_QUEUE);
        snprintf(client->Name, sizeof(client->Name), "raw:%.27s", peer);
        *slot = client;
        GwNotice("%s: connected", client->Name);
    }
}

/* Порт только для чтения: принятое от клиента выбрасывается, чтение — чтобы заметить отключение. */
static void GwRawService(void) {
    uint8_t buffer[256];
    for (uint32_t i = 0; i < GW_RAW_CLIENTS; i++) {
        GW_CONN* client = g_Raw[i];
        if (!client) {
            continue;
        }
        int got;
        while ((got = Plat_Read(client->Io, buffer, sizeof(buffer))) > 0) {
        }
        GwFlush(client);
        if (got < 0 || client->Failed) {
            GwNotice("%s: disconnected", client->Name);
            g_Raw[i] = NULL;
            Plat_Close(client->Io);
            free(client->Tx);
            free(client);
        }
    }
}

static void GwDrop(GW_CONN* conn, const char* why) {
    GwNotice("%s: %s", conn->Name, why);
    g_Conns[conn->Link] = NULL;
    MNode_RemoveLink(&g_Node, conn->Link);
    Plat_Close(conn->Io);
    free(conn->Tx);
    free(conn);
}

/* ---- Запуск --------------------------------------------------------------------------------- */

static bool GwParseNode(const char* text, MNET_NODE* node) {
    if (strcmp(text, "none") == 0) {
        *node = MNET_NODE_NONE;
        return true;
    }
    unsigned net, local;
    char     tail;
    if (sscanf(text, "%u.%u%c", &net, &local, &tail) != 2 || net > 0xFFFF || local > 0xFFFF) {
        return false;
    }
    *node = MNET_NODE_MAKE(net, local);
    return true;
}

static void GwUsage(void) {
    fprintf(stderr,
            "usage: mnetgw --com PORT [--node N.N] [--baud N] [--listen HOST:PORT|off] [--raw HOST:PORT|off]\n"
            "              [--com-neighbor N.N|none] [-v]\n"
            "  The gateway is an MNet node with a COM link to the hypervisor and a TCP server: the debugger and\n"
            "  other programs connect to it as MNet nodes; packets go by destination node, not to everyone.\n"
            "  --node          address of the gateway node (default 1.5)\n"
            "  --com           COM port to the hypervisor: COM3, /dev/ttyUSB0, or tcp:HOST:PORT (QEMU serial)\n"
            "  --baud          COM speed (default 115200)\n"
            "  --listen        TCP server for MNet nodes (default " GW_DEFAULT_LISTEN ")\n"
            "  --raw           TCP server for raw COM bytes: everything outside MNet packets (loader, panics, the\n"
            "                  stand's Linux with [TEST_STAND_READY]), read-only, live (default " GW_DEFAULT_RAW ")\n"
            "  --com-neighbor  node assumed behind COM until it registers (default 1.1, the hypervisor)\n"
            "  -v              print the route table on every change\n");
}

static bool GwParse(int argc, char** argv) {
    g_Options.Node        = GW_DEFAULT_NODE;
    g_Options.Baud        = 115200;
    g_Options.Listen      = GW_DEFAULT_LISTEN;
    g_Options.Raw         = GW_DEFAULT_RAW;
    g_Options.ComNeighbor = MNET_NODE_HV;

    // Без аргументов — подсказка: шлюз без COM ничего не принимает от hv.
    if (argc < 2) {
        GwUsage();
        return false;
    }
    for (int i = 1; i < argc; i++) {
        const char* arg   = argv[i];
        const char* value = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            g_Options.Verbose = true;
            continue;
        }
        if (!value) {
            GwUsage();
            return false;
        }
        i++;
        if (strcmp(arg, "--node") == 0) {
            if (!GwParseNode(value, &g_Options.Node) || g_Options.Node == MNET_NODE_NONE) {
                GwUsage();
                return false;
            }
        } else if (strcmp(arg, "--com") == 0) {
            g_Options.Com = value;
        } else if (strcmp(arg, "--baud") == 0) {
            g_Options.Baud = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(arg, "--listen") == 0) {
            g_Options.Listen = strcmp(value, "off") == 0 ? NULL : value;
        } else if (strcmp(arg, "--raw") == 0) {
            g_Options.Raw = strcmp(value, "off") == 0 ? NULL : value;
        } else if (strcmp(arg, "--com-neighbor") == 0) {
            if (!GwParseNode(value, &g_Options.ComNeighbor)) {
                GwUsage();
                return false;
            }
        } else {
            GwUsage();
            return false;
        }
    }
    if (!g_Options.Com && !g_Options.Listen) {
        GwUsage();
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (!GwParse(argc, argv) || !Plat_Init()) {
        return 2;
    }

    char node[16];
    MNode_Init(&g_Node, g_Options.Node, GwEvent, NULL);
    MNode_Poll(&g_Node, Plat_NowMs());
    GwNotice("node %s", GwNode(g_Options.Node, node));

    g_Com.Link = MNODE_LINK_NONE;
    if (g_Options.Com) {
        // Доверенный сосед: узлы hv (VM) он объявляет сам; до его регистрации — предположение о соседе.
        g_Com.Com = true;
        g_Com.Tx  = malloc(GW_TX_QUEUE);
        strcpy(g_Com.Name, "com");
        MNODE_LINK_DESC desc = {g_Com.Name, &GwOps, &g_Com, MNODE_LINK_STREAM | MNODE_LINK_TRUST | MNODE_LINK_ADOPT,
                                g_Options.ComNeighbor, true, false, &g_Com.Parser};
        g_Com.Link = MNode_AddLink(&g_Node, &desc);
        GwComOpen();
    } else {
        GwNotice("com: not set (--com COM3 or --com tcp:HOST:PORT): no link to the hypervisor, "
                 "the gateway only connects TCP nodes to each other");
    }

    if (g_Options.Listen) {
        char     host[128], error[256] = "";
        uint16_t port;
        if (!GwSplitHostPort(g_Options.Listen, host, sizeof(host), &port)) {
            GwUsage();
            return 2;
        }
        g_Listener = Plat_TcpListen(host, port, error, sizeof(error));
        if (!g_Listener) {
            GwNotice("tcp: cannot listen on %s: %s", g_Options.Listen, error);
            return 1;
        }
        GwNotice("tcp: server on %s: the debugger and other programs connect here and register as MNet "
                 "nodes; packets for a node go to its connection once it has registered",
                 g_Options.Listen);
    }

    if (g_Options.Raw && g_Options.Com) {
        char     host[128], error[256] = "";
        uint16_t port;
        if (!GwSplitHostPort(g_Options.Raw, host, sizeof(host), &port)) {
            GwUsage();
            return 2;
        }
        // Без порта сырых байтов шлюз работает: COM и узлы — главное, сырые байты остаются в stdout.
        g_RawListener = Plat_TcpListen(host, port, error, sizeof(error));
        if (g_RawListener) {
            GwNotice("raw: server on %s: COM bytes outside MNet packets (stand's Linux, loader, panics), read-only",
                     g_Options.Raw);
        } else {
            GwNotice("raw: cannot listen on %s: %s; raw COM bytes go to stdout only", g_Options.Raw, error);
        }
    }

    uint32_t nextReopen = Plat_NowMs();
    uint8_t  buffer[GW_READ_CHUNK];
    for (;;) {
        PLAT_IO* ios[MNODE_MAX_LINKS + GW_RAW_CLIENTS + 3];
        size_t   count = 0;
        ios[count++]   = g_Listener;
        ios[count++]   = g_RawListener;
        ios[count++]   = g_Com.Io;
        for (uint32_t i = 0; i < MNODE_MAX_LINKS; i++) {
            if (g_Conns[i]) {
                ios[count++] = g_Conns[i]->Io;
            }
        }
        for (uint32_t i = 0; i < GW_RAW_CLIENTS; i++) {
            if (g_Raw[i]) {
                ios[count++] = g_Raw[i]->Io;
            }
        }
        Plat_Wait(ios, count, GW_WAIT_MS);

        uint32_t now = Plat_NowMs();
        MNode_Poll(&g_Node, now);
        GwAccept();
        GwRawAccept();

        if (g_Options.Com && !g_Com.Io && (int32_t)(now - nextReopen) >= 0) {
            nextReopen = now + GW_REOPEN_MS;
            GwComOpen();
        }
        if (g_Com.Io) {
            int got;
            while ((got = Plat_Read(g_Com.Io, buffer, sizeof(buffer))) > 0) {
                MNode_InputBytes(&g_Node, g_Com.Link, buffer, (uint32_t)got);
            }
            if (got < 0 || g_Com.Failed) {
                GwComClose("read or write failed");
                nextReopen = now + GW_REOPEN_MS;
            }
        }

        for (uint32_t i = 0; i < MNODE_MAX_LINKS; i++) {
            GW_CONN* conn = g_Conns[i];
            if (!conn) {
                continue;
            }
            int got;
            while ((got = Plat_Read(conn->Io, buffer, sizeof(buffer))) > 0) {
                MNode_InputBytes(&g_Node, conn->Link, buffer, (uint32_t)got);
            }
            if (got < 0 || conn->Failed) {
                GwDrop(conn, "disconnected");
            }
        }

        if (g_Com.Io) {
            GwFlush(&g_Com);
        }
        for (uint32_t i = 0; i < MNODE_MAX_LINKS; i++) {
            if (g_Conns[i]) {
                GwFlush(g_Conns[i]);
            }
        }
        GwRawService();
    }
}
