/* mvci_io.c — transport backend, selected at build time:
 *   Windows                     : FTDI D2XX (ftd2xx.dll)
 *   MVCI_TRANSPORT_LIBUSB        : FTDI over libusb (Android / driverless Linux)
 *   otherwise (Linux/macOS)      : termios on a /dev/ttyUSB* node (ftdi_sio) */

/* termios cfmakeraw + POSIX clock/nanosleep — defined here so the transport
 * compiles standalone (no reliance on build-system -D flags), no warnings. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* On macOS, cfmakeraw()/cfsetspeed() and the TIOCM* ioctls are only exposed
 * under the Darwin extensions (otherwise hidden by _POSIX_C_SOURCE above). */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
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
#elif defined(MVCI_TRANSPORT_LIBUSB)
/* ------------------- Linux / Android: FTDI over libusb ----------------
 * Talks to the FTDI chip directly via USB control/bulk transfers, so it needs
 * neither the ftdi_sio kernel VCP driver nor a /dev/ttyUSB* node. This is the
 * path for Android, whose stock kernels ship libusb-capable USB host support
 * but not ftdi_sio.
 *
 * The FTDI-specific work that ftdi_sio / D2XX normally hide is reimplemented
 * here: baud-rate divisor, line settings, modem-control (DTR reset pulse), the
 * latency timer, RX purge, and — on the read path — stripping the 2 modem/line
 * status bytes the chip prepends to every USB packet.
 */
#include <libusb.h>
#include <stdio.h>

#define FTDI_VID                0x0403

/* FTDI vendor requests (bmRequestType 0x40 = vendor, host->device). */
#define SIO_RESET               0x00
#define SIO_SET_MODEM_CTRL      0x01
#define SIO_SET_FLOW_CTRL       0x02
#define SIO_SET_BAUDRATE        0x03
#define SIO_SET_DATA            0x04
#define SIO_SET_LATENCY_TIMER   0x09

#define SIO_RESET_SIO           0
#define SIO_RESET_PURGE_RX      1

/* modem-control wValue: high byte = line mask, low byte = level (DTR=b0 RTS=b1) */
#define SIO_SET_DTR_HIGH        0x0101
#define SIO_SET_DTR_LOW         0x0100
#define SIO_SET_RTS_LOW         0x0200

#define FTDI_DATA_8N1           0x0008   /* 8 data bits, no parity, 1 stop bit */

struct mvci_io {
    libusb_context       *usb;
    libusb_device_handle *h;
    int                   own_ctx;      /* we created usb -> we exit it */
    int                   iface;
    unsigned char         in_ep, out_ep;
    int                   pkt_size;     /* USB max packet size on the IN endpoint */
    uint8_t               rxbuf[4096];  /* decoded (status bytes stripped) RX data */
    int                   rxlen, rxpos;
};

static int ftdi_ctrl(mvci_io_t *io, uint8_t req, uint16_t val, uint16_t idx)
{
    int r = libusb_control_transfer(io->h, 0x40, req, val, idx, NULL, 0, 1000);
    return r < 0 ? -1 : 0;
}

/* FTDI baud-rate divisor encoding for the 3 MHz base clock (FT232 family). */
static uint32_t ftdi_baud_divisor(int baud)
{
    static const unsigned char frac_code[8] = { 0, 3, 2, 4, 1, 5, 6, 7 };
    const int clk = 3000000, clk_div = 16;
    unsigned long enc;

    if (baud >= clk / clk_div) {
        enc = 0;
    } else if (baud >= clk / (clk_div + clk_div / 2)) {
        enc = 1;
    } else if (baud >= clk / (2 * clk_div)) {
        enc = 2;
    } else {
        int divisor = clk * 16 / clk_div / baud;
        int best = (divisor & 1) ? divisor / 2 + 1 : divisor / 2;
        if (best > 0x1ffff) best = 0x1ffff;
        enc = ((unsigned long)best >> 3) |
              ((unsigned long)frac_code[best & 7] << 14);
    }
    return (uint32_t)enc;
}

/* Fill in in_ep/out_ep/pkt_size from the active config; keep FT232 defaults on
 * any failure so a nonstandard descriptor still has a fighting chance. */
static void ftdi_probe_endpoints(mvci_io_t *io)
{
    io->in_ep = 0x81; io->out_ep = 0x02; io->pkt_size = 64; io->iface = 0;

    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(libusb_get_device(io->h), &cfg) != 0)
        return;
    if (cfg->bNumInterfaces > 0 && cfg->interface[0].num_altsetting > 0) {
        const struct libusb_interface_descriptor *id =
            &cfg->interface[0].altsetting[0];
        io->iface = id->bInterfaceNumber;
        for (int i = 0; i < id->bNumEndpoints; i++) {
            const struct libusb_endpoint_descriptor *ep = &id->endpoint[i];
            if ((ep->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep->bEndpointAddress & 0x80) {
                io->in_ep = ep->bEndpointAddress;
                if (ep->wMaxPacketSize) io->pkt_size = ep->wMaxPacketSize;
            } else {
                io->out_ep = ep->bEndpointAddress;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
}

/* Locate the adapter and open a handle into io->h. Accepts, via `port`:
 *   "fd:<n>"        — wrap a file descriptor from the Android USB Host API
 *   "<vid>:<pid>"   — explicit hex VID:PID (e.g. "0403:6001")
 *   NULL/""/"M-VCI" — scan FTDI (VID 0x0403) devices, prefer product "M-VCI"
 */
static int ftdi_open_handle(mvci_io_t *io, const char *port)
{
    unsigned vid = 0, pid = 0;
    int fd = -1;

    if (port && strncmp(port, "fd:", 3) == 0)
        fd = (int)strtol(port + 3, NULL, 0);
    else if (port && sscanf(port, "%x:%x", &vid, &pid) == 2)
        ; /* explicit VID:PID */
    else
        vid = 0; /* product-name scan below */

    if (fd >= 0) {
        /* Android: the Java USB Host API already opened the device and handed
         * us its fd; the process cannot enumerate USB. Device discovery was
         * already disabled before libusb_init() (see mvci_io_open). */
#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000107)
        return libusb_wrap_sys_device(io->usb, (intptr_t)fd, &io->h) == 0 ? 0 : -1;
#else
        return -1;   /* libusb too old for fd wrapping */
#endif
    }

    if (vid && pid) {
        io->h = libusb_open_device_with_vid_pid(io->usb, (uint16_t)vid, (uint16_t)pid);
        return io->h ? 0 : -1;
    }

    /* Scan for an FTDI device, preferring one whose product string matches the
     * requested name ("M-VCI" by default); fall back to the first FTDI found. */
    const char *want = (port && *port) ? port : "M-VCI";
    libusb_device **list;
    ssize_t cnt = libusb_get_device_list(io->usb, &list);
    if (cnt < 0) return -1;

    libusb_device_handle *fallback = NULL;
    for (ssize_t i = 0; i < cnt && !io->h; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        if (dd.idVendor != FTDI_VID) continue;

        libusb_device_handle *h = NULL;
        if (libusb_open(list[i], &h) != 0 || !h) continue;

        char prod[128] = { 0 };
        if (dd.iProduct)
            libusb_get_string_descriptor_ascii(h, dd.iProduct,
                                                (unsigned char *)prod, sizeof prod);
        if (strcmp(prod, want) == 0) {
            io->h = h;                       /* exact match wins */
        } else if (!fallback) {
            fallback = h;                    /* remember the first FTDI device */
        } else {
            libusb_close(h);
        }
    }
    if (!io->h && fallback) io->h = fallback;
    else if (io->h && fallback) libusb_close(fallback);
    libusb_free_device_list(list, 1);
    return io->h ? 0 : -1;
}

mvci_io_t *mvci_io_open(const char *port)
{
    mvci_io_t *io = (mvci_io_t *)calloc(1, sizeof(*io));
    if (!io) return NULL;

    /* NO_DEVICE_DISCOVERY is a global flag that must be set before libusb_init;
     * required on Android, where we open from a caller-supplied fd ("fd:<n>")
     * and the process is not allowed to enumerate the USB bus. */
    if (port && strncmp(port, "fd:", 3) == 0) {
#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000108)
        libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
#endif
    }

    if (libusb_init(&io->usb) != 0) { free(io); return NULL; }
    io->own_ctx = 1;

    if (ftdi_open_handle(io, port) != 0) {
        libusb_exit(io->usb);
        free(io);
        return NULL;
    }

    ftdi_probe_endpoints(io);
    libusb_set_auto_detach_kernel_driver(io->h, 1);  /* unbind ftdi_sio if present */
    if (libusb_claim_interface(io->h, io->iface) != 0) {
        libusb_close(io->h);
        libusb_exit(io->usb);
        free(io);
        return NULL;
    }

    uint32_t div = ftdi_baud_divisor(115200);
    ftdi_ctrl(io, SIO_RESET, SIO_RESET_SIO, 0);
    ftdi_ctrl(io, SIO_SET_BAUDRATE,
              (uint16_t)(div & 0xFFFF), (uint16_t)((div >> 16) & 0xFF));
    ftdi_ctrl(io, SIO_SET_DATA, FTDI_DATA_8N1, 0);
    ftdi_ctrl(io, SIO_SET_FLOW_CTRL, 0, 0);            /* no flow control */
    ftdi_ctrl(io, SIO_SET_LATENCY_TIMER, 2, 0);
    ftdi_ctrl(io, SIO_RESET, SIO_RESET_PURGE_RX, 0);

    /* DTR pulse = MCU reset (ClrRts, SetDtr, 15 ms, ClrDtr) */
    ftdi_ctrl(io, SIO_SET_MODEM_CTRL, SIO_SET_RTS_LOW, 0);
    ftdi_ctrl(io, SIO_SET_MODEM_CTRL, SIO_SET_DTR_HIGH, 0);
    mvci_sleep_ms(15);
    ftdi_ctrl(io, SIO_SET_MODEM_CTRL, SIO_SET_DTR_LOW, 0);

    mvci_sleep_ms(1000);                               /* MCU boot */
    ftdi_ctrl(io, SIO_RESET, SIO_RESET_PURGE_RX, 0);
    io->rxlen = io->rxpos = 0;
    return io;
}

void mvci_io_close(mvci_io_t *io)
{
    if (!io) return;
    if (io->h) {
        libusb_release_interface(io->h, io->iface);
        libusb_close(io->h);
    }
    if (io->own_ctx && io->usb) libusb_exit(io->usb);
    free(io);
}

int mvci_io_write(mvci_io_t *io, const uint8_t *buf, int n)
{
    int off = 0;
    while (off < n) {
        int xfer = 0;
        int r = libusb_bulk_transfer(io->h, io->out_ep,
                                     (unsigned char *)buf + off, n - off, &xfer, 1000);
        if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) return -1;
        if (xfer == 0) return -1;                      /* no progress -> give up */
        off += xfer;
    }
    return off;
}

int mvci_io_read(mvci_io_t *io, uint8_t *buf, int n, int timeout_ms)
{
    int got = 0;
    uint32_t deadline = mvci_now_ms() + (uint32_t)timeout_ms;

    while (got < n) {
        if (io->rxpos < io->rxlen) {                   /* drain decoded bytes first */
            int avail = io->rxlen - io->rxpos;
            int cp = (n - got < avail) ? n - got : avail;
            memcpy(buf + got, io->rxbuf + io->rxpos, (size_t)cp);
            io->rxpos += cp;
            got += cp;
            continue;
        }
        int32_t rem = (int32_t)(deadline - mvci_now_ms());
        if (rem <= 0) break;

        unsigned char raw[512];
        int want = io->pkt_size < (int)sizeof raw ? io->pkt_size : (int)sizeof raw;
        int xfer = 0;
        int r = libusb_bulk_transfer(io->h, io->in_ep, raw, want, &xfer,
                                     rem > 200 ? 200 : (unsigned)rem);
        if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) return -1;

        /* Each USB packet is prefixed with 2 modem/line status bytes; strip
         * them. An idle poll returns just those 2 bytes (no data). */
        io->rxlen = io->rxpos = 0;
        for (int i = 0; i < xfer; i += io->pkt_size) {
            int chunk = xfer - i;
            if (chunk > io->pkt_size) chunk = io->pkt_size;
            if (chunk <= 2) continue;
            memcpy(io->rxbuf + io->rxlen, raw + i + 2, (size_t)(chunk - 2));
            io->rxlen += chunk - 2;
        }
    }
    return got;
}

void mvci_io_purge_rx(mvci_io_t *io)
{
    ftdi_ctrl(io, SIO_RESET, SIO_RESET_PURGE_RX, 0);
    io->rxlen = io->rxpos = 0;
}

/* ====================================================================== */
#else
/* ------------------------- Linux / macOS: termios -------------------- */
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
