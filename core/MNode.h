#pragma once

/*
    MNode — переносимый узел MNet: линки, таблица маршрутов «узел -> линк», пересылка пакетов,
    endpoint'ы и служба route (регистрация соседей и обмен таблицами, MNET_ENDPOINT_ROUTE в MNet.h).

    Шлюз — обычный узел MNode с несколькими линками: пакет не себе он пересылает в линк, который
    выбирает по DstNode в своей таблице. Линк в пакет не пишется. Ни COM, ни TCP узел не знает:
    транспорт хозяин подключает как линк — ops.Send отдаёт пакет целиком, а принятые байты или
    пакеты хозяин передаёт в MNode_InputBytes / MNode_InputPacket. Поэтому один и тот же код
    работает в шлюзе на Windows и Linux, в Dbg (Qt) и на ESP32 / MCU.

    Без ОС, потоков и malloc: вся память — структура MNODE, которую держит хозяин. Размеры таблиц
    задаются макросами MNODE_MAX_* до включения заголовка. Время — миллисекунды от хозяина
    (MNode_Poll), любое начало отсчёта.

    Не потокобезопасно: все вызовы — из одного потока. ops.Send, обработчики endpoint'ов и событий
    не должны синхронно звать MNode_Input* и MNode_Poll; отправлять пакеты из них можно.

    Правила (те же, что у маршрутизатора hv, Prj Doc/mnet.txt):
        - источник проверяется по линку: принимаются пакеты от узлов, маршрут к которым идёт через
          этот же линк; неизвестный источник — по политике линка (LEARN, ADOPT, DEFAULT);
        - пакет на MNET_NODE_LINK — всегда себе, от соседа по линку; ответ на него — тем же линком;
        - чужой пакет: HopLimit - 1, новый HeaderCrc, в линк по маршруту, но не обратно;
        - на rpc-запрос, который некуда доставить, узел отвечает MNET_E_NO_ENDPOINT / MNET_E_NO_ROUTE.

    Служба route:
        - сосед регистрируется REGISTER'ом: сам он и перечисленные узлы (если линк TRUST) — через
          этот линк. Адрес, занятый за другим линком, не принимается (MNET_E_DENIED);
        - в ответ на REGISTER и GET узел отдаёт свою таблицу без узлов за линком спрашивающего;
        - соседям, которые просили NOTIFY, после каждого изменения таблицы уходит REGISTER FULL;
        - линк ANNOUNCE: узел сам регистрируется у соседа при подъёме линка и раз в
          MNODE_REFRESH_MS, повторяя через 1, 2, 4, 8 с, пока сосед молчит;
        - линк упал — маршруты через него, полученные от соседа, снимаются; настроенные хозяином
          (CONFIG, BOOTSTRAP) остаются.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "MNet.h"
#include "MNetCodec.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MNODE_MAX_LINKS
#define MNODE_MAX_LINKS 32
#endif
#ifndef MNODE_MAX_ROUTES
#define MNODE_MAX_ROUTES 128
#endif
#ifndef MNODE_MAX_ENDPOINTS
#define MNODE_MAX_ENDPOINTS 64
#endif
/* Пакеты самому себе ждут здесь конца текущего вызова MNode_*: обработчик не вызывается изнутри Send. */
#ifndef MNODE_LOCAL_QUEUE
#define MNODE_LOCAL_QUEUE (16u * 1024u)
#endif

#define MNODE_LINK_NAME_MAX 32

#define MNODE_RETRY_MIN_MS   1000u   /* ANNOUNCE: сосед молчит — REGISTER через 1, 2, 4, 8 с */
#define MNODE_RETRY_MAX_MS   8000u
#define MNODE_REFRESH_MS     15000u  /* ANNOUNCE: сосед ответил — обновлять регистрацию так часто */
#define MNODE_UNSUPPORTED_MS 30000u  /* у соседа нет службы route — спрашивать реже */
#define MNODE_LEARN_MIN_MS   2000u   /* узнан неизвестный узел за линком — сверить таблицу не чаще */
#define MNODE_STREAM_IDLE_MS 100u    /* поток молчит — недоразобранные байты отдать как RAW */
#define MNODE_PACKET_IDLE_MS 1000u   /* ...но начало с целым заголовком (CRC сошлась) — точно пакет, ждать дольше */

typedef uint8_t MNODE_LINK;

#define MNODE_LINK_NONE ((MNODE_LINK)0xFF)

/* Ответ транспорта на пакет: принят, очередь полна (пакет отброшен, как в IP), линк не работает. */
typedef enum _MNODE_TX {
    MNODE_TX_OK = 0,
    MNODE_TX_FULL,
    MNODE_TX_DOWN,
} MNODE_TX;

typedef struct _MNODE_LINK_OPS {
    MNODE_TX (*Send)(void* ctx, const void* packet, uint32_t size);
} MNODE_LINK_OPS;

/* Политика линка. */
#define MNODE_LINK_STREAM   0x0001u  /* вход — поток байтов (MNode_InputBytes), разборщик — Parser */
#define MNODE_LINK_DEFAULT  0x0002u  /* маршрут по умолчанию; источники с него не проверяются */
#define MNODE_LINK_TRUST    0x0004u  /* сосед может объявлять узлы за собой (REGISTER, таблица) */
#define MNODE_LINK_LEARN    0x0008u  /* соседа ещё нет — им становится первый неизвестный источник (клиент без route) */
#define MNODE_LINK_ANNOUNCE 0x0010u  /* узел сам регистрируется у соседа (REGISTER FULL | NOTIFY) */
#define MNODE_LINK_ADOPT    0x0020u  /* неизвестный источник с линка — узел за соседом (линк теряет объявления: COM) */

typedef struct _MNODE_LINK_DESC {
    const char*           Name;
    const MNODE_LINK_OPS* Ops;
    void*                 Ctx;
    uint32_t              Flags;
    MNET_NODE             Neighbor;     /* сосед, если известен заранее, иначе MNET_NODE_NONE */
    bool                  Bootstrap;    /* Neighbor — только предположение: заменяется регистрацией */
    bool                  Up;           /* линк уже работает */
    MNET_PARSER*          Parser;       /* STREAM: память разборщика от хозяина */
} MNODE_LINK_DESC;

typedef enum _MNODE_ROUTE_KIND {
    MNODE_ROUTE_CONFIG = 0,     /* поставил хозяин; обмен маршрутами его не трогает */
    MNODE_ROUTE_BOOTSTRAP,      /* предположение хозяина до регистрации соседа */
    MNODE_ROUTE_NEIGHBOR,       /* сосед: зарегистрировался или выучен LEARN */
    MNODE_ROUTE_LEARNED,        /* пришёл пакет с доверенного линка от неизвестного узла */
    MNODE_ROUTE_REMOTE,         /* узел за соседом: из его REGISTER или таблицы */
} MNODE_ROUTE_KIND;

typedef enum _MNODE_SYNC {
    MNODE_SYNC_NONE = 0,        /* узел у соседа не регистрируется */
    MNODE_SYNC_WAITING,         /* сосед ещё не ответил */
    MNODE_SYNC_DONE,            /* зарегистрирован, таблица соседа получена */
    MNODE_SYNC_UNSUPPORTED,     /* сосед ответил ошибкой: службы route у него нет */
} MNODE_SYNC;

typedef struct _MNODE_ROUTE_INFO {
    MNET_NODE        Node;
    MNODE_LINK       Link;
    MNODE_ROUTE_KIND Kind;
    uint8_t          Hops;
} MNODE_ROUTE_INFO;

typedef struct _MNODE_LINK_INFO {
    char       Name[MNODE_LINK_NAME_MAX];
    uint32_t   Flags;
    bool       Up;
    MNET_NODE  Neighbor;
    MNODE_SYNC Sync;
    bool       Subscribed;      /* сосед просил NOTIFY */
    uint32_t   RxPackets;
    uint32_t   RxBad;
    uint32_t   RxRawBytes;
    uint32_t   TxPackets;
    uint32_t   TxDropped;
} MNODE_LINK_INFO;

typedef struct _MNODE_STATS {
    uint32_t Delivered;
    uint32_t Forwarded;
    uint32_t NoRoute;
    uint32_t NoEndpoint;
    uint32_t BadSource;
    uint32_t HopLimit;
    uint32_t LocalDropped;
    uint32_t RouteUpdates;      /* принятые REGISTER и таблицы соседей */
    uint32_t RouteRejected;     /* узлы из них, которые не приняты */
    uint32_t RouteLearned;      /* узлы, узнанные по пакетам с доверенных линков */
} MNODE_STATS;

typedef enum _MNODE_EVENT_KIND {
    MNODE_EVENT_BAD_SOURCE,     /* Link, Node — пакет от узла, которому через этот линк нельзя */
    MNODE_EVENT_RAW,            /* Link, Data, Length — байты вне пакетов на потоковом линке */
    MNODE_EVENT_DAMAGED,        /* Link — испорченный пакет на потоковом линке */
    MNODE_EVENT_ROUTES,         /* таблица маршрутов изменилась */
    MNODE_EVENT_REGISTERED,     /* Link, Node — сосед зарегистрировался */
    MNODE_EVENT_DENIED,         /* Link, Node — сосед или узел из его объявления не принят: адрес занят за другим линком */
    MNODE_EVENT_SYNC,           /* Link, Node, Status — сосед ответил на нашу регистрацию */
} MNODE_EVENT_KIND;

typedef struct _MNODE_EVENT {
    MNODE_EVENT_KIND Kind;
    MNODE_LINK       Link;
    MNET_NODE        Node;
    int32_t          Status;
    const uint8_t*   Data;
    uint32_t         Length;
} MNODE_EVENT;

typedef enum _MNODE_STATUS {
    MNODE_OK = 0,
    MNODE_NO_ROUTE,             /* маршрута нет или линк не работает */
    MNODE_NO_BUFFERS,           /* очередь линка полна */
    MNODE_TOO_LARGE,
    MNODE_BAD_PARAM,
} MNODE_STATUS;

typedef void (*MNODE_EVENT_HANDLER)(void* ctx, const MNODE_EVENT* event);

/* packet действителен только во время вызова; from — линк, из которого он пришёл (NONE — от себя). */
typedef void (*MNODE_HANDLER)(void* ctx, const MNET_HEADER* packet, MNODE_LINK from);

/* Внутренности — открыты только ради статического размещения. */
typedef struct _MNODE_LINK_SLOT {
    bool                  Open;
    bool                  Up;
    bool                  Subscribed;
    bool                  Speaks;           /* сосед говорит на route: присылал запросы или отвечал */
    bool                  RefreshPending;
    char                  Name[MNODE_LINK_NAME_MAX];
    const MNODE_LINK_OPS* Ops;
    void*                 Ctx;
    uint32_t              Flags;
    MNET_NODE             Neighbor;
    MNET_NODE             ConfigNeighbor;
    bool                  ConfigBootstrap;
    MNET_PARSER*          Parser;
    uint32_t              LastRxMs;
    MNODE_SYNC            Sync;
    uint32_t              Request;
    uint32_t              DueMs;
    uint32_t              BackoffMs;
    uint32_t              Misses;
    uint32_t              RefreshMs;
    uint32_t              SentMs;
    uint32_t              ViewCrc;
    uint32_t              Epoch;
    uint32_t              TableGen;
    uint32_t              RxPackets;
    uint32_t              RxBad;
    uint32_t              RxRawBytes;
    uint32_t              TxPackets;
    uint32_t              TxDropped;
} MNODE_LINK_SLOT;

typedef struct _MNODE_ROUTE {
    MNET_NODE  Node;
    MNODE_LINK Link;
    uint8_t    Kind;
    uint8_t    Hops;
    uint32_t   Epoch;
} MNODE_ROUTE;

typedef struct _MNODE_ENDPOINT {
    MNET_ENDPOINT Number;
    MNODE_HANDLER Handler;
    void*         Ctx;
} MNODE_ENDPOINT;

typedef struct _MNODE {
    MNET_NODE           Self;
    uint32_t            NowMs;
    MNODE_EVENT_HANDLER Event;
    void*               EventCtx;
    MNODE_LINK          DefaultLink;
    MNODE_STATS         Stats;
    uint32_t            Generation;
    uint32_t            Epoch;
    uint32_t            NextRequestId;
    MNET_ENDPOINT       NextDynamic;
    bool                Dirty;
    uint32_t            Depth;
    uint32_t            LocalHead;
    uint32_t            LocalTail;
    MNODE_LINK_SLOT     Links[MNODE_MAX_LINKS];
    MNODE_ROUTE         Routes[MNODE_MAX_ROUTES];
    MNODE_ENDPOINT      Endpoints[MNODE_MAX_ENDPOINTS];
    uint8_t             Local[MNODE_LOCAL_QUEUE];
    uint8_t             Scratch[MNET_MAX_PACKET];
} MNODE;

void          MNode_Init(MNODE* node, MNET_NODE self, MNODE_EVENT_HANDLER event, void* eventCtx);
MNET_NODE     MNode_Self(const MNODE* node);
const MNODE_STATS* MNode_Stats(const MNODE* node);

/* Линки. MNODE_LINK_NONE — нет места или неверное описание. */
MNODE_LINK    MNode_AddLink(MNODE* node, const MNODE_LINK_DESC* desc);
void          MNode_RemoveLink(MNODE* node, MNODE_LINK link);
void          MNode_SetLinkUp(MNODE* node, MNODE_LINK link, bool up);
void          MNode_SetDefaultLink(MNODE* node, MNODE_LINK link);
bool          MNode_GetLink(const MNODE* node, MNODE_LINK link, MNODE_LINK_INFO* info);

/* Вход от транспорта и время. */
void          MNode_InputBytes(MNODE* node, MNODE_LINK link, const void* data, uint32_t length);
void          MNode_InputPacket(MNODE* node, MNODE_LINK link, const void* packet, uint32_t length);
void          MNode_Poll(MNODE* node, uint32_t nowMs);

/* Маршруты. bootstrap — предположение до регистрации соседа, иначе CONFIG. */
bool          MNode_AddRoute(MNODE* node, MNET_NODE dst, MNODE_LINK link, bool bootstrap);
void          MNode_RemoveRoute(MNODE* node, MNET_NODE dst);
bool          MNode_GetRoute(const MNODE* node, uint32_t index, MNODE_ROUTE_INFO* info);
MNODE_LINK    MNode_FindRoute(const MNODE* node, MNET_NODE dst);
/* Зарегистрироваться у соседа и получить его таблицу сейчас, не дожидаясь срока. */
void          MNode_Sync(MNODE* node, MNODE_LINK link);

/* Endpoint'ы. number = MNET_ENDPOINT_NONE — выдать динамический номер; NONE в ответ — занято. */
MNET_ENDPOINT MNode_Open(MNODE* node, MNET_ENDPOINT number, MNODE_HANDLER handler, void* ctx);
void          MNode_Close(MNODE* node, MNET_ENDPOINT number);

/* Отправка: head и data идут в данных пакета подряд (например, заголовок rpc и тело). */
MNODE_STATUS  MNode_Send(MNODE* node, MNET_ENDPOINT from, MNET_NODE dst, MNET_ENDPOINT endpoint, MNET_PROTO proto,
                         const void* head, uint32_t headLength, const void* data, uint32_t length);
/* Соседу по линку (DstNode = MNET_NODE_LINK). */
MNODE_STATUS  MNode_SendLink(MNODE* node, MNODE_LINK link, MNET_ENDPOINT from, MNET_ENDPOINT endpoint,
                             MNET_PROTO proto, const void* head, uint32_t headLength, const void* data, uint32_t length);
/* Ответ на rpc-запрос; requestLink — откуда пришёл запрос (нужен для запросов соседу по линку). */
MNODE_STATUS  MNode_Reply(MNODE* node, MNET_ENDPOINT from, const MNET_HEADER* request, MNODE_LINK requestLink,
                          int32_t status, const void* data, uint32_t length);
/* Доставить своему endpoint'у пакет, адресованный этому узлу (пакет себе, отложенный хозяином). */
void          MNode_DeliverLocal(MNODE* node, const MNET_HEADER* packet);

#ifdef __cplusplus
}
#endif
