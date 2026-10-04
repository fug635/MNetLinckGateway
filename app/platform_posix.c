#define _DEFAULT_SOURCE
#include "platform.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Всё — неблокирующие дескрипторы: COM-порт (termios, raw) и сокеты. */
struct _PLAT_IO {
    bool Socket;
    int  Fd;
};

static PLAT_IO* PlatWrap(int fd, bool socket) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    PLAT_IO* io = calloc(1, sizeof(*io));
    io->Fd      = fd;
    io->Socket  = socket;
    return io;
}

bool Plat_Init(void) {
    return true;
}

uint32_t Plat_NowMs(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u);
}

static speed_t PlatBaud(uint32_t baud) {
    switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default:     return B115200;
    }
}

PLAT_IO* Plat_SerialOpen(const char* name, uint32_t baud, char* error, size_t errorSize) {
    int fd = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(error, errorSize, "%s: %s", name, strerror(errno));
        return NULL;
    }

    // 8N1 без управления потоком — как драйвер UART ядра.
    struct termios tty;
    if (tcgetattr(fd, &tty) == 0) {
        cfmakeraw(&tty);
        cfsetispeed(&tty, PlatBaud(baud));
        cfsetospeed(&tty, PlatBaud(baud));
        tty.c_cflag |= CLOCAL | CREAD;
        tty.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
        tty.c_cc[VMIN]  = 0;
        tty.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tty);
        tcflush(fd, TCIOFLUSH);
    }
    return PlatWrap(fd, false);
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

PLAT_IO* Plat_TcpConnect(const char* host, uint16_t port, char* error, size_t errorSize) {
    struct sockaddr_in address;
    if (!PlatResolve(host, port, &address, error, errorSize)) {
        return NULL;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || connect(s, (struct sockaddr*)&address, sizeof(address)) != 0) {
        snprintf(error, errorSize, "%s:%u: %s", host, (unsigned)port, strerror(errno));
        if (s >= 0) {
            close(s);
        }
        return NULL;
    }
    int noDelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
    return PlatWrap(s, true);
}

PLAT_IO* Plat_TcpListen(const char* host, uint16_t port, char* error, size_t errorSize) {
    struct sockaddr_in address;
    if (!PlatResolve(host, port, &address, error, errorSize)) {
        return NULL;
    }
    int s     = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (s < 0 || bind(s, (struct sockaddr*)&address, sizeof(address)) != 0 || listen(s, 8) != 0) {
        snprintf(error, errorSize, "%s:%u: %s", host, (unsigned)port, strerror(errno));
        if (s >= 0) {
            close(s);
        }
        return NULL;
    }
    return PlatWrap(s, true);
}

PLAT_IO* Plat_TcpAccept(PLAT_IO* listener, char* peer, size_t peerSize) {
    struct sockaddr_in address;
    socklen_t          length = sizeof(address);
    int                s      = accept(listener->Fd, (struct sockaddr*)&address, &length);
    if (s < 0) {
        return NULL;
    }
    char host[64] = "?";
    inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host));
    snprintf(peer, peerSize, "%s:%u", host, (unsigned)ntohs(address.sin_port));
    int noDelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
    return PlatWrap(s, true);
}

int Plat_Read(PLAT_IO* io, void* buffer, size_t size) {
    ssize_t got = io->Socket ? recv(io->Fd, buffer, size, 0) : read(io->Fd, buffer, size);
    if (got > 0) {
        return (int)got;
    }
    if (got == 0) {
        return io->Socket ? -1 : 0;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}

int Plat_Write(PLAT_IO* io, const void* data, size_t size) {
    ssize_t sent = io->Socket ? send(io->Fd, data, size, MSG_NOSIGNAL) : write(io->Fd, data, size);
    if (sent >= 0) {
        return (int)sent;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}

void Plat_Close(PLAT_IO* io) {
    if (io) {
        close(io->Fd);
        free(io);
    }
}

void Plat_Wait(PLAT_IO* const* ios, size_t count, uint32_t timeoutMs) {
    struct pollfd fds[64];
    nfds_t        used = 0;
    for (size_t i = 0; i < count && used < 64; i++) {
        if (ios[i]) {
            fds[used].fd      = ios[i]->Fd;
            fds[used].events  = POLLIN;
            fds[used].revents = 0;
            used++;
        }
    }
    poll(fds, used, (int)timeoutMs);
}
