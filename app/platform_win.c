#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* COM-порт — синхронный HANDLE с таймаутами «вернуть, что есть»; сокеты — неблокирующие. */
struct _PLAT_IO {
    bool   Serial;
    HANDLE Port;
    SOCKET Socket;
};

static void PlatWinError(char* error, size_t size, const char* what, DWORD code) {
    char text[256] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, text, sizeof(text), NULL);
    size_t length = strlen(text);
    while (length && (text[length - 1] == '\n' || text[length - 1] == '\r' || text[length - 1] == '.')) {
        text[--length] = 0;
    }
    snprintf(error, size, "%s: %s (%lu)", what, text, (unsigned long)code);
}

bool Plat_Init(void) {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

uint32_t Plat_NowMs(void) {
    return (uint32_t)GetTickCount64();
}

PLAT_IO* Plat_SerialOpen(const char* name, uint32_t baud, char* error, size_t errorSize) {
    char path[64];
    snprintf(path, sizeof(path), strncmp(name, "\\\\.\\", 4) == 0 ? "%s" : "\\\\.\\%s", name);

    HANDLE port = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (port == INVALID_HANDLE_VALUE) {
        PlatWinError(error, errorSize, name, GetLastError());
        return NULL;
    }

    // 8N1 без управления потоком — как драйвер UART ядра.
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    GetCommState(port, &dcb);
    dcb.BaudRate     = baud;
    dcb.ByteSize     = 8;
    dcb.Parity       = NOPARITY;
    dcb.StopBits     = ONESTOPBIT;
    dcb.fBinary      = TRUE;
    dcb.fParity      = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl  = DTR_CONTROL_ENABLE;
    dcb.fRtsControl  = RTS_CONTROL_ENABLE;
    dcb.fOutX        = FALSE;
    dcb.fInX         = FALSE;
    dcb.fAbortOnError = FALSE;

    COMMTIMEOUTS timeouts;
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout        = MAXDWORD;   // ReadFile сразу возвращает то, что пришло
    timeouts.WriteTotalTimeoutConstant  = 20;

    if (!SetCommState(port, &dcb) || !SetCommTimeouts(port, &timeouts) || !SetupComm(port, 64 * 1024, 64 * 1024)) {
        PlatWinError(error, errorSize, name, GetLastError());
        CloseHandle(port);
        return NULL;
    }
    PurgeComm(port, PURGE_RXCLEAR | PURGE_TXCLEAR);

    PLAT_IO* io = calloc(1, sizeof(*io));
    io->Serial = true;
    io->Port   = port;
    io->Socket = INVALID_SOCKET;
    return io;
}

static bool PlatResolve(const char* host, uint16_t port, struct sockaddr_in* address, char* error, size_t errorSize) {
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &address->sin_addr) == 1) {
        return true;
    }

    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &result) != 0 || !result) {
        snprintf(error, errorSize, "%s: address not found", host);
        return false;
    }
    address->sin_addr = ((struct sockaddr_in*)result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return true;
}

static PLAT_IO* PlatSocket(SOCKET s) {
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    PLAT_IO* io = calloc(1, sizeof(*io));
    io->Port   = INVALID_HANDLE_VALUE;
    io->Socket = s;
    return io;
}

PLAT_IO* Plat_TcpConnect(const char* host, uint16_t port, char* error, size_t errorSize) {
    struct sockaddr_in address;
    if (!PlatResolve(host, port, &address, error, errorSize)) {
        return NULL;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET || connect(s, (struct sockaddr*)&address, sizeof(address)) != 0) {
        PlatWinError(error, errorSize, host, (DWORD)WSAGetLastError());
        if (s != INVALID_SOCKET) {
            closesocket(s);
        }
        return NULL;
    }
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
    return PlatSocket(s);
}

PLAT_IO* Plat_TcpListen(const char* host, uint16_t port, char* error, size_t errorSize) {
    struct sockaddr_in address;
    if (!PlatResolve(host, port, &address, error, errorSize)) {
        return NULL;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr*)&address, sizeof(address)) != 0 || listen(s, 8) != 0) {
        PlatWinError(error, errorSize, host, (DWORD)WSAGetLastError());
        if (s != INVALID_SOCKET) {
            closesocket(s);
        }
        return NULL;
    }
    return PlatSocket(s);
}

PLAT_IO* Plat_TcpAccept(PLAT_IO* listener, char* peer, size_t peerSize) {
    struct sockaddr_in address;
    int                length = sizeof(address);
    SOCKET             s      = accept(listener->Socket, (struct sockaddr*)&address, &length);
    if (s == INVALID_SOCKET) {
        return NULL;
    }
    char host[64] = "?";
    inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host));
    snprintf(peer, peerSize, "%s:%u", host, (unsigned)ntohs(address.sin_port));
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
    return PlatSocket(s);
}

int Plat_Read(PLAT_IO* io, void* buffer, size_t size) {
    if (io->Serial) {
        DWORD   got    = 0;
        COMSTAT status;
        DWORD   errors = 0;
        ClearCommError(io->Port, &errors, &status);
        if (!ReadFile(io->Port, buffer, (DWORD)size, &got, NULL)) {
            return -1;
        }
        return (int)got;
    }

    int got = recv(io->Socket, buffer, (int)size, 0);
    if (got > 0) {
        return got;
    }
    if (got == 0) {
        return -1;
    }
    return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
}

int Plat_Write(PLAT_IO* io, const void* data, size_t size) {
    if (io->Serial) {
        DWORD written = 0;
        if (!WriteFile(io->Port, data, (DWORD)size, &written, NULL) && GetLastError() != ERROR_TIMEOUT) {
            return -1;
        }
        return (int)written;
    }

    int sent = send(io->Socket, data, (int)size, 0);
    if (sent >= 0) {
        return sent;
    }
    return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
}

void Plat_Close(PLAT_IO* io) {
    if (!io) {
        return;
    }
    if (io->Serial) {
        CloseHandle(io->Port);
    } else {
        closesocket(io->Socket);
    }
    free(io);
}

void Plat_Wait(PLAT_IO* const* ios, size_t count, uint32_t timeoutMs) {
    // select() на Windows — только сокеты; COM-порт опрашивается: тогда ждём не дольше 5 мс.
    fd_set read;
    FD_ZERO(&read);
    u_int sockets = 0;
    for (size_t i = 0; i < count; i++) {
        if (!ios[i]) {
            continue;
        }
        if (ios[i]->Serial) {
            if (timeoutMs > 5) {
                timeoutMs = 5;
            }
        } else if (sockets < FD_SETSIZE) {
            FD_SET(ios[i]->Socket, &read);
            sockets++;
        }
    }
    if (!sockets) {
        Sleep(timeoutMs);
        return;
    }
    struct timeval timeout = {(long)(timeoutMs / 1000), (long)(timeoutMs % 1000) * 1000};
    select(0, &read, NULL, NULL, &timeout);
}
