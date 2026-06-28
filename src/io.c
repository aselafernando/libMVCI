/* mvci_io.c — transport: termios (Linux) or FTDI D2XX (Windows). */

/* termios cfmakeraw + POSIX clock/nanosleep (the Makefile also sets these;
 * guarded so a standalone compile still works without warnings). */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "io.h"
#include "compat.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
#ifdef _WIN32
/* -------------------- Windows: FTDI D2XX (loaded at runtime) ----------
 * ftd2xx.dll is resolved with LoadLibrary/GetProcAddress, so the build needs
 * neither ftd2xx.h nor ftd2xx.lib — only the FTDI driver runtime (which ships
 * ftd2xx.dll) on the target machine. We declare the minimal D2XX surface here.
 */
typedef PVOID  FT_HANDLE;
typedef ULONG  FT_STATUS;
#define FT_OK                   0
#define FT_OPEN_BY_DESCRIPTION  2
#define FT_PURGE_RX             1
#define FT_BITS_8               8
#define FT_STOP_BITS_1          0
#define FT_PARITY_NONE          0
#define FT_FLOW_NONE            0

typedef FT_STATUS (WINAPI *PFN_OpenEx)(PVOID, DWORD, FT_HANDLE *);
typedef FT_STATUS (WINAPI *PFN_H)(FT_HANDLE);
typedef FT_STATUS (WINAPI *PFN_HU)(FT_HANDLE, ULONG);
typedef FT_STATUS (WINAPI *PFN_HUU)(FT_HANDLE, ULONG, ULONG);
typedef FT_STATUS (WINAPI *PFN_Chars)(FT_HANDLE, UCHAR, UCHAR, UCHAR);
typedef FT_STATUS (WINAPI *PFN_Flow)(FT_HANDLE, USHORT, UCHAR, UCHAR);
typedef FT_STATUS (WINAPI *PFN_Lat)(FT_HANDLE, UCHAR);
typedef FT_STATUS (WINAPI *PFN_RW)(FT_HANDLE, LPVOID, DWORD, LPDWORD);

static struct {
    HMODULE  dll;
    PFN_OpenEx OpenEx;
    PFN_H    Close, ResetDevice, ClrRts, SetDtr, ClrDtr;
    PFN_HU   SetBaudRate, Purge;
    PFN_HUU  SetTimeouts;
    PFN_Chars SetDataCharacteristics;
    PFN_Flow SetFlowControl;
    PFN_Lat  SetLatencyTimer;
    PFN_RW   Write, Read;
} FT;

static int ft_load(void)
{
    if (FT.dll) return 0;
    HMODULE m = LoadLibraryA("ftd2xx.dll");
    if (!m) return -1;

    FT.OpenEx                 = (PFN_OpenEx)GetProcAddress(m, "FT_OpenEx");
    FT.Close                  = (PFN_H)    GetProcAddress(m, "FT_Close");
    FT.ResetDevice            = (PFN_H)    GetProcAddress(m, "FT_ResetDevice");
    FT.ClrRts                 = (PFN_H)    GetProcAddress(m, "FT_ClrRts");
    FT.SetDtr                 = (PFN_H)    GetProcAddress(m, "FT_SetDtr");
    FT.ClrDtr                 = (PFN_H)    GetProcAddress(m, "FT_ClrDtr");
    FT.SetBaudRate            = (PFN_HU)   GetProcAddress(m, "FT_SetBaudRate");
    FT.Purge                  = (PFN_HU)   GetProcAddress(m, "FT_Purge");
    FT.SetTimeouts            = (PFN_HUU)  GetProcAddress(m, "FT_SetTimeouts");
    FT.SetDataCharacteristics = (PFN_Chars)GetProcAddress(m, "FT_SetDataCharacteristics");
    FT.SetFlowControl         = (PFN_Flow) GetProcAddress(m, "FT_SetFlowControl");
    FT.SetLatencyTimer        = (PFN_Lat)  GetProcAddress(m, "FT_SetLatencyTimer");
    FT.Write                  = (PFN_RW)   GetProcAddress(m, "FT_Write");
    FT.Read                   = (PFN_RW)   GetProcAddress(m, "FT_Read");

    if (!FT.OpenEx || !FT.Close || !FT.ResetDevice || !FT.ClrRts || !FT.SetDtr ||
        !FT.ClrDtr || !FT.SetBaudRate || !FT.Purge || !FT.SetTimeouts ||
        !FT.SetDataCharacteristics || !FT.SetFlowControl || !FT.SetLatencyTimer ||
        !FT.Write || !FT.Read) {
        FreeLibrary(m);
        return -1;
    }
    FT.dll = m;
    return 0;
}

struct mvci_io { FT_HANDLE h; };

mvci_io_t *mvci_io_open(const char *port)
{
    const char *desc = (port && *port) ? port : "M-VCI";
    if (ft_load() != 0) return NULL;

    mvci_io_t *io = (mvci_io_t *)calloc(1, sizeof(*io));
    if (!io) return NULL;

    if (FT.OpenEx((PVOID)desc, FT_OPEN_BY_DESCRIPTION, &io->h) != FT_OK) {
        free(io);
        return NULL;
    }

    FT.SetBaudRate(io->h, 115200);
    FT.SetDataCharacteristics(io->h, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_NONE);
    FT.SetFlowControl(io->h, FT_FLOW_NONE, 0, 0);
    FT.SetLatencyTimer(io->h, 2);
    FT.SetTimeouts(io->h, 100, 1000);          /* default rx/tx ms */

    FT.ResetDevice(io->h);
    FT.Purge(io->h, FT_PURGE_RX);
    FT.ResetDevice(io->h);

    /* DTR pulse = MCU reset (FT_ClrRts, FT_SetDtr, 15 ms, FT_ClrDtr) */
    FT.ClrRts(io->h);
    FT.SetDtr(io->h);
    mvci_sleep_ms(15);
    FT.ClrDtr(io->h);

    mvci_sleep_ms(1000);                        /* MCU boot */
    FT.Purge(io->h, FT_PURGE_RX);
    return io;
}

void mvci_io_close(mvci_io_t *io)
{
    if (!io) return;
    if (io->h) FT.Close(io->h);
    free(io);
}

int mvci_io_write(mvci_io_t *io, const uint8_t *buf, int n)
{
    DWORD written = 0;
    if (FT.Write(io->h, (LPVOID)buf, (DWORD)n, &written) != FT_OK) return -1;
    return (int)written;
}

int mvci_io_read(mvci_io_t *io, uint8_t *buf, int n, int timeout_ms)
{
    DWORD got = 0;
    FT.SetTimeouts(io->h, (ULONG)timeout_ms, 1000);
    if (FT.Read(io->h, buf, (DWORD)n, &got) != FT_OK) return -1;
    return (int)got;
}

void mvci_io_purge_rx(mvci_io_t *io)
{
    FT.Purge(io->h, FT_PURGE_RX);
}

/* ====================================================================== */
#else
/* ----------------------------- Linux: termios ------------------------ */
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

struct mvci_io { int fd; };

mvci_io_t *mvci_io_open(const char *port)
{
    mvci_io_t *io = (mvci_io_t *)calloc(1, sizeof(*io));
    if (!io) return NULL;

    io->fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (io->fd < 0) { free(io); return NULL; }
    fcntl(io->fd, F_SETFL, fcntl(io->fd, F_GETFL) & ~O_NONBLOCK);

    struct termios t;
    memset(&t, 0, sizeof t);
    cfmakeraw(&t);
    t.c_cflag = CS8 | CLOCAL | CREAD;
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(io->fd, TCSANOW, &t);
    tcflush(io->fd, TCIOFLUSH);

    /* DTR pulse = MCU reset (ClrRts, SetDtr, 15 ms, ClrDtr) */
    int s = 0;
    ioctl(io->fd, TIOCMGET, &s);
    s &= ~TIOCM_RTS; ioctl(io->fd, TIOCMSET, &s);
    s |=  TIOCM_DTR; ioctl(io->fd, TIOCMSET, &s);
    mvci_sleep_ms(15);
    s &= ~TIOCM_DTR; ioctl(io->fd, TIOCMSET, &s);

    mvci_sleep_ms(1000);                        /* MCU boot */
    tcflush(io->fd, TCIOFLUSH);
    return io;
}

void mvci_io_close(mvci_io_t *io)
{
    if (!io) return;
    if (io->fd >= 0) close(io->fd);
    free(io);
}

int mvci_io_write(mvci_io_t *io, const uint8_t *buf, int n)
{
    int off = 0;
    while (off < n) {
        int w = (int)write(io->fd, buf + off, (size_t)(n - off));
        if (w < 0) { if (errno == EAGAIN) continue; return -1; }
        off += w;
    }
    tcdrain(io->fd);
    return off;
}

int mvci_io_read(mvci_io_t *io, uint8_t *buf, int n, int timeout_ms)
{
    int got = 0;
    uint32_t deadline = mvci_now_ms() + (uint32_t)timeout_ms;
    while (got < n) {
        if ((int32_t)(deadline - mvci_now_ms()) <= 0) break;
        int r = (int)read(io->fd, buf + got, (size_t)(n - got));
        if (r < 0 && errno != EAGAIN) return -1;
        if (r > 0) got += r; else mvci_sleep_ms(1);
    }
    return got;
}

void mvci_io_purge_rx(mvci_io_t *io)
{
    tcflush(io->fd, TCIFLUSH);
}

#endif
