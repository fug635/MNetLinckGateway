#pragma once

/*
    Платформа шлюза mnetgw: COM-порт, TCP и время. Узел MNet (core/MNode) о ней не знает —
    платформа нужна только приложению, которое подключает транспорты к узлу как линки.
    Реализации: platform_win.c (Win32, Winsock), platform_posix.c (Linux: termios, сокеты BSD).
    На ESP32 / MCU её место займут драйвер UART и lwIP.

    Все операции не ждут: Read и Write возвращают сколько смогли (0 — сейчас нечего), отрицательное
    значение — соединение закрыто или ошибка. Ждать событий — Plat_Wait.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct _PLAT_IO PLAT_IO;

bool     Plat_Init(void);
uint32_t Plat_NowMs(void);

/* name: COM3, \\.\COM12, /dev/ttyUSB0. */
PLAT_IO* Plat_SerialOpen(const char* name, uint32_t baud, char* error, size_t errorSize);
PLAT_IO* Plat_TcpConnect(const char* host, uint16_t port, char* error, size_t errorSize);
PLAT_IO* Plat_TcpListen(const char* host, uint16_t port, char* error, size_t errorSize);
/* Новое подключение или NULL; peer — "адрес:порт". */
PLAT_IO* Plat_TcpAccept(PLAT_IO* listener, char* peer, size_t peerSize);

int      Plat_Read(PLAT_IO* io, void* buffer, size_t size);
int      Plat_Write(PLAT_IO* io, const void* data, size_t size);
void     Plat_Close(PLAT_IO* io);

/* Ждать данных на любом из ios не дольше timeoutMs. */
void     Plat_Wait(PLAT_IO* const* ios, size_t count, uint32_t timeoutMs);
