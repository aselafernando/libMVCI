/* passthru.c — SAE J2534 PassThru API on top of the MVCI session layer.
 *
 * Exposed as libMVCI.so (Linux) / MVCI32.dll, MVCI64.dll (Windows).
 *
 * ISO14230 K-line interface, so we model one channel
 * per opened device. Device/channel/filter IDs are small integers handed back
 * to the caller and mapped to a slot here.
 * 
 * Other Protocols are supported however unable to test without a suitable vehicle
 * 
 */

/* Mark this TU as the DLL build so j2534.h emits dllexport (Windows). The VS
 * project also defines this; guard to avoid a redefinition warning. */
#ifndef MVCI_BUILDING_DLL
#define MVCI_BUILDING_DLL
#endif
#include <mvci/j2534.h>
#include <mvci/serial.h>
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SLOTS   4

typedef struct {
    int          open;
    int          connected;
    mvci_ctx_t  *ctx;
    uint32_t     proto;
    uint32_t     next_id;        /* filter/msg id generator */
} slot_t;

static slot_t       g_slot[MAX_SLOTS];
static mvci_mutex_t g_lock;
static int          g_init = 0;
static char         g_last_error[128] = "No error";

static void ensure_init(void)
{
    if (!g_init) { mvci_mutex_init(&g_lock); g_init = 1; }
}

static void set_err(const char *s) { strncpy(g_last_error, s, sizeof g_last_error - 1); }

/* device/channel/filter id encoding: dev = slot+1, channel = slot+0x100 */
#define DEV_ID(s)         ((unsigned long)((s) + 1))
#define CH_ID(s)          ((unsigned long)((s) + 0x100))
#define SLOT_FROM_DEV(id) ((int)(id) - 1)
#define SLOT_FROM_CH(id)  ((int)(id) - 0x100)

static slot_t *dev_slot(unsigned long id)
{
    int i = SLOT_FROM_DEV(id);
    if (i < 0 || i >= MAX_SLOTS || !g_slot[i].open) return NULL;
    return &g_slot[i];
}
static slot_t *ch_slot(unsigned long id)
{
    int i = SLOT_FROM_CH(id);
    if (i < 0 || i >= MAX_SLOTS || !g_slot[i].open) return NULL;
    return &g_slot[i];
}

/* ---- platform default port ------------------------------------------ */
static const char *default_port(void *pName)
{
#ifdef _WIN32
    (void)pName;
    return "M-VCI";                                  /* FTDI description */
#else
    if (pName && *(const char *)pName) return (const char *)pName;
    const char *env = getenv("MVCI_PORT");
    if (env) return env;
  #if defined(MVCI_TRANSPORT_LIBUSB)
    return "M-VCI";        /* libusb: scan FTDI, prefer product "M-VCI" (use
                              "fd:<n>" on Android — see BUILD.md) */
  #elif defined(__APPLE__)
    return "/dev/cu.usbserial";                      /* FTDI VCP node (macOS) */
  #else
    return "/dev/ttyUSB0";                           /* ftdi_sio node (Linux) */
  #endif
#endif
}

/* ====================================================================== */

long J2534_API PassThruOpen(void *pName, unsigned long *pDeviceID)
{
    ensure_init();
    if (!pDeviceID) { set_err("NULL pDeviceID"); return ERR_NULL_PARAMETER; }

    mvci_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < MAX_SLOTS; i++) if (!g_slot[i].open) { slot = i; break; }
    if (slot < 0) { mvci_mutex_unlock(&g_lock); set_err("no free device slot"); return ERR_FAILED; }

    mvci_ctx_t *ctx = mvci_open(default_port(pName));
    if (!ctx) { mvci_mutex_unlock(&g_lock); set_err("device open failed"); return ERR_DEVICE_NOT_CONNECTED; }
    if (mvci_handshake(ctx) != 0) {
        mvci_close(ctx);
        mvci_mutex_unlock(&g_lock);
        set_err("handshake failed");
        return ERR_DEVICE_NOT_CONNECTED;
    }
    mvci_start_keepalive(ctx);

    g_slot[slot].open = 1;
    g_slot[slot].connected = 0;
    g_slot[slot].ctx = ctx;
    g_slot[slot].next_id = 1;
    *pDeviceID = DEV_ID(slot);
    mvci_mutex_unlock(&g_lock);
    return STATUS_NOERROR;
}

long J2534_API PassThruClose(unsigned long DeviceID)
{
    ensure_init();
    mvci_mutex_lock(&g_lock);
    slot_t *s = dev_slot(DeviceID);
    if (!s) { mvci_mutex_unlock(&g_lock); set_err("invalid device id"); return ERR_INVALID_DEVICE_ID; }
    if (s->connected) mvci_disconnect(s->ctx);       /* session teardown (01 00 02) */
    mvci_close(s->ctx);                              /* stops keepalive + closes */
    memset(s, 0, sizeof *s);
    mvci_mutex_unlock(&g_lock);
    return STATUS_NOERROR;
}

long J2534_API PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID,
                               unsigned long Flags, unsigned long BaudRate,
                               unsigned long *pChannelID)
{
    ensure_init();
    slot_t *s = dev_slot(DeviceID);
    if (!s) { set_err("invalid device id"); return ERR_INVALID_DEVICE_ID; }
    if (!pChannelID) { set_err("NULL pChannelID"); return ERR_NULL_PARAMETER; }

    if (mvci_connect(s->ctx, ProtocolID, Flags, BaudRate) != 0) {
        set_err("connect failed");
        return ERR_FAILED;
    }
    s->connected = 1;
    s->proto = ProtocolID;
    *pChannelID = CH_ID(SLOT_FROM_DEV(DeviceID));
    return STATUS_NOERROR;
}

long J2534_API PassThruDisconnect(unsigned long ChannelID)
{
    ensure_init();
    slot_t *s = ch_slot(ChannelID);
    if (!s || !s->connected) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }
    /* Drop the channel only; keep the device session alive (keepalive continues)
     * so the next PassThruConnect succeeds without re-initialising the adapter.
     * The wire teardown (01 00 02) happens at PassThruClose. */
    s->connected = 0;
    return STATUS_NOERROR;
}

long J2534_API PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType,
                                      PASSTHRU_MSG *pMaskMsg, PASSTHRU_MSG *pPatternMsg,
                                      PASSTHRU_MSG *pFlowControlMsg, unsigned long *pFilterID)
{
    ensure_init();
    (void)FilterType; (void)pFlowControlMsg;
    slot_t *s = ch_slot(ChannelID);
    if (!s || !s->connected) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }
    if (!pMaskMsg || !pPatternMsg || !pFilterID) { set_err("NULL filter msg"); return ERR_NULL_PARAMETER; }

    uint8_t mask    = pMaskMsg->DataSize    ? pMaskMsg->Data[0]    : 0;
    uint8_t pattern = pPatternMsg->DataSize ? pPatternMsg->Data[0] : 0;
    uint32_t msgid  = 0x000e7a00u + (s->next_id & 0xff);    /* device-style handle */

    if (mvci_start_filter(s->ctx, msgid, mask, pattern) != 0) {
        set_err("start filter failed");
        return ERR_FAILED;
    }
    *pFilterID = s->next_id++;
    return STATUS_NOERROR;
}

long J2534_API PassThruStopMsgFilter(unsigned long ChannelID, unsigned long FilterID)
{
    ensure_init();
    (void)FilterID;
    slot_t *s = ch_slot(ChannelID);
    if (!s || !s->connected) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }
    return STATUS_NOERROR;                            /* device clears on disconnect */
}

long J2534_API PassThruWriteMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                                 unsigned long *pNumMsgs, unsigned long TimeInterval)
{
    ensure_init();
    (void)TimeInterval;
    slot_t *s = ch_slot(ChannelID);
    if (!s || !s->connected) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }
    if (!pMsg || !pNumMsgs || *pNumMsgs == 0) { set_err("NULL/empty msg"); return ERR_NULL_PARAMETER; }
    if (pMsg->DataSize == 0 || pMsg->DataSize > 64) { set_err("bad DataSize"); return ERR_INVALID_MSG; }

    if (mvci_write_msg(s->ctx, pMsg->Data, pMsg->DataSize) != 0) {
        set_err("write failed");
        return ERR_FAILED;
    }
    *pNumMsgs = 1;
    return STATUS_NOERROR;
}

long J2534_API PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                                unsigned long *pNumMsgs, unsigned long Timeout)
{
    ensure_init();
    slot_t *s = ch_slot(ChannelID);
    if (!s || !s->connected) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }
    if (!pMsg || !pNumMsgs || *pNumMsgs == 0) { set_err("NULL/empty msg"); return ERR_NULL_PARAMETER; }

    unsigned long want = *pNumMsgs, got = 0;
    uint32_t deadline = mvci_now_ms() + (uint32_t)(Timeout ? Timeout : 1);

    while (got < want) {
        uint8_t buf[64], rx = 0;
        int m = mvci_poll(s->ctx, buf, sizeof buf, &rx, 200);
        if (m > 0) {
            PASSTHRU_MSG *o = &pMsg[got];
            memset(o, 0, sizeof *o);
            o->ProtocolID = s->proto;
            o->RxStatus   = rx;                       /* 0 = data, 2 = tx echo */
            o->DataSize   = (uint32_t)m;
            memcpy(o->Data, buf, (size_t)m);
            got++;
            continue;
        }
        if ((int32_t)(deadline - mvci_now_ms()) <= 0) break;
        mvci_sleep_ms(5);
    }

    *pNumMsgs = got;
    if (got == 0) { set_err("no message"); return ERR_BUFFER_EMPTY; }
    return STATUS_NOERROR;
}

long J2534_API PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID,
                             void *pInput, void *pOutput)
{
    ensure_init();
    slot_t *s = ch_slot(ChannelID);
    if (!s) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }

    switch (IoctlID) {
    case SET_CONFIG: {
        SCONFIG_LIST *list = (SCONFIG_LIST *)pInput;
        if (!list || (list->NumOfParams && !list->ConfigPtr)) return ERR_NULL_PARAMETER;
        for (uint32_t i = 0; i < list->NumOfParams; i++)
            if (mvci_set_config(s->ctx, list->ConfigPtr[i].Parameter,
                                list->ConfigPtr[i].Value) != 0) {
                set_err("set_config failed");
                return ERR_FAILED;
            }
        return STATUS_NOERROR;
    }
    case CLEAR_PERIODIC_MSGS:
        return mvci_clear_periodic(s->ctx) == 0 ? STATUS_NOERROR : ERR_FAILED;

    case CLEAR_TX_BUFFER:
    case CLEAR_RX_BUFFER:
    case CLEAR_MSG_FILTERS:
        return STATUS_NOERROR;

    case FAST_INIT: {
        PASSTHRU_MSG *in = (PASSTHRU_MSG *)pInput;
        PASSTHRU_MSG *out = (PASSTHRU_MSG *)pOutput;
        if (!in || in->DataSize == 0) return ERR_NULL_PARAMETER;
        uint8_t reply[32];
        int k = mvci_fast_init(s->ctx, in->Data, in->DataSize, reply, sizeof reply);
        if (k < 0) { set_err("fast init failed"); return ERR_FAILED; }
        if (out) {
            memset(out, 0, sizeof *out);
            out->ProtocolID = s->proto;
            out->DataSize = (uint32_t)k;
            memcpy(out->Data, reply, (size_t)k);
        }
        return STATUS_NOERROR;
    }

    default:
        set_err("ioctl not supported");
        return ERR_NOT_SUPPORTED;
    }
}

/* ---- stubs / informational ------------------------------------------ */

long J2534_API PassThruStartPeriodicMsg(unsigned long ChannelID, PASSTHRU_MSG *pMsg,
                                        unsigned long *pMsgID, unsigned long TimeInterval)
{ (void)ChannelID;(void)pMsg;(void)pMsgID;(void)TimeInterval; return ERR_NOT_SUPPORTED; }

long J2534_API PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID)
{ (void)ChannelID;(void)MsgID; return ERR_NOT_SUPPORTED; }

long J2534_API PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long PinNumber,
                                             unsigned long Voltage)
{ (void)DeviceID;(void)PinNumber;(void)Voltage; return ERR_NOT_SUPPORTED; }

long J2534_API PassThruReadVersion(unsigned long DeviceID, char *pFirmwareVersion,
                                   char *pDllVersion, char *pApiVersion)
{
    ensure_init();
    if (!dev_slot(DeviceID)) return ERR_INVALID_DEVICE_ID;
    /* Report the same identity as the original Mini-VCI DLL so the host app
     * (e.g. Toyota Techstream) recognises the adapter and selects ISO14230. */
    if (pFirmwareVersion) strcpy(pFirmwareVersion, "J2534 MINIV1.03");
    if (pDllVersion)      strcpy(pDllVersion, "MVCI J2534 DLL v1.4.6");
    if (pApiVersion)      strcpy(pApiVersion, "04.04");
    return STATUS_NOERROR;
}

long J2534_API PassThruGetLastError(char *pErrorDescription)
{
    if (!pErrorDescription) return ERR_NULL_PARAMETER;
    strcpy(pErrorDescription, g_last_error);
    return STATUS_NOERROR;
}
