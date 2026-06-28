/* mvci_serial.c — MVCI protocol + session (transport- and crypto-agnostic).
 * Transport via mvci_io.h, DES via mvci_des.c, threading via mvci_compat.h. */

#include <mvci/serial.h>
#include "io.h"
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 *  Pure codec
 * ====================================================================== */

uint8_t mvci_xorsum(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) c ^= p[i];
    return c;
}

int mvci_frame_plain(const uint8_t *payload, size_t plen, uint8_t *out, size_t out_cap)
{
    size_t total = plen + 3;
    if (total > 255 || total > out_cap) return -1;
    out[0] = (uint8_t)total;
    out[1] = 0x00;
    if (plen) memcpy(out + 2, payload, plen);
    out[2 + plen] = mvci_xorsum(out, 2 + plen);
    return (int)total;
}

int mvci_frame_enc(const uint8_t key[8], const uint8_t *inner, size_t inner_len,
                   uint8_t *out, size_t out_cap)
{
    uint8_t body[MVCI_MAX_INNER];
    size_t nb = (inner_len + 7) / 8;
    size_t clen = nb * 8;
    if (clen > sizeof body) return -1;
    memset(body, 0, clen);
    memcpy(body, inner, inner_len);
    mvci_des_encrypt(key, body, nb);
    return mvci_frame_plain(body, clen, out, out_cap);
}

int mvci_frame_payload(const uint8_t *frame, size_t frame_len, const uint8_t **payload_out)
{
    if (frame_len < 3) return -1;
    if (frame[0] != frame_len) return -1;
    if (frame[1] != 0x00) return -1;
    if (mvci_xorsum(frame, frame_len - 1) != frame[frame_len - 1]) return -1;
    if (payload_out) *payload_out = frame + 2;
    return (int)(frame_len - 3);
}

int mvci_frame_decrypt(const uint8_t key[8], const uint8_t *frame, size_t frame_len,
                       uint8_t *inner_out, size_t inner_cap)
{
    if (frame_len < 3 || frame[0] != frame_len || frame[1] != 0x00) return -1;
    int plen = (int)frame_len - 3;
    if (plen <= 0 || (plen % 8) != 0 || (size_t)plen > inner_cap) return -1;

    uint8_t wire_cs   = frame[frame_len - 1];
    uint8_t cs_cipher = mvci_xorsum(frame, frame_len - 1);     /* over ciphertext */

    memcpy(inner_out, frame + 2, plen);
    mvci_des_decrypt(key, inner_out, plen / 8);

    /* status replies checksum over ciphertext, message replies over plaintext */
    uint8_t cs_plain = frame[0] ^ frame[1] ^ mvci_xorsum(inner_out, plen);
    if (wire_cs != cs_cipher && wire_cs != cs_plain) return -1;
    return plen;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int mvci_inner_set_config(uint32_t param, uint32_t value, uint8_t *inner)
{
    static const uint8_t hdr[8] = { 0x0e, 0x00, 0x0e, 0x02, 0x04, 0x00, 0x00, 0x00 };
    memcpy(inner, hdr, 8);
    put_u32(inner + 8, param);
    put_u32(inner + 12, value);
    return 16;
}

int mvci_inner_connect(uint32_t proto, uint32_t flags, uint32_t baud, uint8_t *inner)
{
    inner[0] = 0x0d; inner[1] = 0x00; inner[2] = 0x07;
    put_u32(inner + 3, proto);
    put_u32(inner + 7, flags);
    put_u32(inner + 11, baud);
    return 15;
}

int mvci_inner_start_filter(uint32_t proto, uint32_t msgid, uint32_t type,
                            uint8_t mask, uint8_t pattern, uint8_t *inner)
{
    inner[0] = 0x10; inner[1] = 0x00; inner[2] = 0x0b;
    put_u32(inner + 3, proto);
    put_u32(inner + 7, msgid);
    put_u32(inner + 11, type);
    inner[15] = mask;
    inner[16] = pattern;
    return 17;
}

int mvci_inner_clear_periodic(uint8_t *inner)
{
    static const uint8_t b[8] = { 0x06, 0x00, 0x0e, 0x09, 0x04, 0x00, 0x00, 0x00 };
    memcpy(inner, b, 8);
    return 8;
}

int mvci_inner_fast_init(uint32_t proto, const uint8_t *init, size_t n, uint8_t *inner)
{
    inner[0] = (uint8_t)(5 + 1 + n);
    inner[1] = 0x00; inner[2] = 0x0e; inner[3] = 0x05;
    put_u32(inner + 4, proto);
    memcpy(inner + 8, init, n);
    return (int)(8 + n);
}

int mvci_inner_write_msg(uint32_t proto, const uint8_t *msg, size_t n, uint8_t *inner)
{
    inner[0] = (uint8_t)(1 + 8 + n);
    inner[1] = 0x00; inner[2] = 0x0a;
    put_u32(inner + 3, proto);
    put_u32(inner + 7, 0);
    memcpy(inner + 11, msg, n);
    return (int)(11 + n);
}

int mvci_inner_read_poll(uint8_t *inner)
{
    static const uint8_t b[8] = { 0x05, 0x00, 0x09, 0x04, 0x00, 0x00, 0x00, 0x00 };
    memcpy(inner, b, 8);
    return 8;
}

int mvci_parse_read_reply(const uint8_t *inner, int inner_len,
                          uint8_t *msg_out, size_t cap, uint8_t *rxstatus_out)
{
    if (inner_len < 4 || inner[2] != 0x09) return -1;
    if (rxstatus_out) *rxstatus_out = inner[3];
    int mlen = (int)inner[0] - 9;                  /* msg at offset 11 */
    if (mlen <= 0) return 0;                        /* status reply, no message */
    if (11 + mlen > inner_len || (size_t)mlen > cap) return -1;
    memcpy(msg_out, inner + 11, mlen);
    return mlen;
}

/* ======================================================================
 *  Session
 * ====================================================================== */

struct mvci_ctx {
    mvci_io_t       *io;
    int              have_key;
    uint8_t          key[8];
    mvci_mutex_t     lock;       /* serialises device I/O */
    mvci_thread_t    ka_thread;
    volatile int     ka_run;
};

/* set MVCI_DEBUG=1 to trace decrypted TX/RX inners on stderr */
static int dbg_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("MVCI_DEBUG"); v = (e && *e && *e != '0'); }
    return v;
}
static void dbg_hex(const char *tag, const uint8_t *b, int n)
{
    if (!dbg_on()) return;
    fprintf(stderr, "  %s", tag);
    for (int i = 0; i < n; i++) fprintf(stderr, " %02x", b[i]);
    fprintf(stderr, "\n");
}

mvci_ctx_t *mvci_open(const char *port)
{
    mvci_ctx_t *ctx = (mvci_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->io = mvci_io_open(port);
    if (!ctx->io) { free(ctx); return NULL; }
    mvci_mutex_init(&ctx->lock);
    return ctx;
}

void mvci_close(mvci_ctx_t *ctx)
{
    if (!ctx) return;
    mvci_stop_keepalive(ctx);
    mvci_io_close(ctx->io);
    mvci_mutex_destroy(&ctx->lock);
    free(ctx);
}

const uint8_t *mvci_key(const mvci_ctx_t *ctx)
{
    return ctx->have_key ? ctx->key : NULL;
}

/* read one framed message: LEN byte, then LEN-1 more bytes */
static int read_frame(mvci_ctx_t *ctx, uint8_t *buf, int cap, int timeout_ms)
{
    uint8_t len;
    if (mvci_io_read(ctx->io, &len, 1, timeout_ms) < 1) return -1;
    if (len < 3 || len > cap) return -1;
    buf[0] = len;
    if (mvci_io_read(ctx->io, buf + 1, len - 1, timeout_ms) < len - 1) return -1;
    return len;
}

/* reset + identify + read challenge -> install key. Caller must hold ctx->lock
 * when called after the keepalive thread is running. */
static int do_handshake(mvci_ctx_t *ctx)
{
    uint8_t frame[MVCI_MAX_FRAME];
    int n;

    n = mvci_frame_plain(NULL, 0, frame, sizeof frame);     /* reset */
    mvci_io_purge_rx(ctx->io);
    if (mvci_io_write(ctx->io, frame, n) != n) return -1;
    mvci_sleep_ms(110);
    mvci_io_purge_rx(ctx->io);

    static const uint8_t ident[] = { 0x07, 0x00, 0x01, 'M', 'V', 'C', 'I', '-', 'T' };
    n = mvci_frame_plain(ident, sizeof ident, frame, sizeof frame);
    if (mvci_io_write(ctx->io, frame, n) != n) return -1;

    n = read_frame(ctx, frame, sizeof frame, 2000);
    if (n < 0) return -1;

    const uint8_t *pl;
    int plen = mvci_frame_payload(frame, n, &pl);
    if (plen < 3 + 8 || pl[0] != 0x09) return -1;
    memcpy(ctx->key, pl + 3, 8);
    ctx->have_key = 1;
    return 0;
}

int mvci_handshake(mvci_ctx_t *ctx)
{
    /* Called from PassThruOpen before the keepalive thread starts — no lock. */
    return do_handshake(ctx);
}

/* unlocked core transaction (caller holds ctx->lock) */
static int transact_locked(mvci_ctx_t *ctx, const uint8_t *inner, size_t inner_len,
                           uint8_t *resp_inner, size_t resp_cap, int timeout_ms)
{
    uint8_t frame[MVCI_MAX_FRAME];
    int n = mvci_frame_enc(ctx->key, inner, inner_len, frame, sizeof frame);
    if (n < 0) return -1;

    dbg_hex("TX inner:", inner, (int)inner_len);
    mvci_io_purge_rx(ctx->io);
    if (mvci_io_write(ctx->io, frame, n) != n) return -1;

    n = read_frame(ctx, frame, sizeof frame, timeout_ms);
    if (n < 0) { if (dbg_on()) fprintf(stderr, "  RX: timeout\n"); return -1; }
    int r = mvci_frame_decrypt(ctx->key, frame, n, resp_inner, resp_cap);
    if (r < 0) { dbg_hex("RX raw(bad):", frame, n); return -1; }
    dbg_hex("RX inner:", resp_inner, r);
    return r;
}

int mvci_transact(mvci_ctx_t *ctx, const uint8_t *inner, size_t inner_len,
                  uint8_t *resp_inner, size_t resp_cap, int timeout_ms)
{
    if (!ctx->have_key) return -1;
    mvci_mutex_lock(&ctx->lock);
    int r = transact_locked(ctx, inner, inner_len, resp_inner, resp_cap, timeout_ms);
    mvci_mutex_unlock(&ctx->lock);
    return r;
}

/* ---- keepalive thread ----------------------------------------------- */

int mvci_keepalive(mvci_ctx_t *ctx)
{
    static const uint8_t ka[8] = { 0x05, 0x00, 0x09, 0x06, 0x00, 0x00, 0x00, 0x00 };
    uint8_t resp[32];
    int r = mvci_transact(ctx, ka, sizeof ka, resp, sizeof resp, 500);
    return r > 0 ? 0 : -1;
}

static MVCI_THREAD_RET keepalive_thread(void *arg)
{
    mvci_ctx_t *ctx = (mvci_ctx_t *)arg;
    while (ctx->ka_run) {
        mvci_sleep_ms(15);
        if (ctx->ka_run) mvci_keepalive(ctx);
    }
    return 0;
}

int mvci_start_keepalive(mvci_ctx_t *ctx)
{
    if (ctx->ka_run) return 0;
    ctx->ka_run = 1;
    if (mvci_thread_create(&ctx->ka_thread, keepalive_thread, ctx) != 0) {
        ctx->ka_run = 0;
        return -1;
    }
    return 0;
}

void mvci_stop_keepalive(mvci_ctx_t *ctx)
{
    if (!ctx->ka_run) return;
    ctx->ka_run = 0;
    mvci_thread_join(ctx->ka_thread);
}

/* ---- higher level J2534 operations ---------------------------------- */

#define MVCI_ISO14230 4

int mvci_connect(mvci_ctx_t *ctx, uint32_t proto, uint32_t flags, uint32_t baud)
{
    uint8_t in[32], resp[64];
    int n = mvci_inner_connect(proto, flags, baud, in);

    mvci_mutex_lock(&ctx->lock);
    int r = transact_locked(ctx, in, n, resp, sizeof resp, 2000);
    int ok = (r >= 3 && resp[0] == 0x02 && resp[2] == 0x07);

    /* After a prior Disconnect the device is de-initialised and ignores Connect
     * until it is reset; re-run the handshake (03/07/09) and retry once. */
    if (!ok && do_handshake(ctx) == 0) {
        r = transact_locked(ctx, in, n, resp, sizeof resp, 2000);
        ok = (r >= 3 && resp[0] == 0x02 && resp[2] == 0x07);
    }
    mvci_mutex_unlock(&ctx->lock);
    return ok ? 0 : -1;
}

int mvci_disconnect(mvci_ctx_t *ctx)
{
    static const uint8_t in[8] = { 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t resp[32];
    int r = mvci_transact(ctx, in, sizeof in, resp, sizeof resp, 1000);
    return r > 0 ? 0 : -1;
}

int mvci_start_filter(mvci_ctx_t *ctx, uint32_t msgid, uint8_t mask, uint8_t pattern)
{
    uint8_t in[32], resp[64];
    int n = mvci_inner_start_filter(MVCI_ISO14230, msgid, 1 /*PASS*/, mask, pattern, in);
    int r = mvci_transact(ctx, in, n, resp, sizeof resp, 2000);
    return (r >= 3 && resp[0] == 0x02 && resp[2] == 0x0b) ? 0 : -1;
}

int mvci_set_config(mvci_ctx_t *ctx, uint32_t param, uint32_t value)
{
    uint8_t in[16], resp[64];
    mvci_inner_set_config(param, value, in);
    int r = mvci_transact(ctx, in, sizeof in, resp, sizeof resp, 2000);
    return (r >= 3 && resp[0] == 0x02 && resp[2] == 0x0e) ? 0 : -1;
}

int mvci_clear_periodic(mvci_ctx_t *ctx)
{
    uint8_t in[8], resp[64];
    int n = mvci_inner_clear_periodic(in);
    int r = mvci_transact(ctx, in, n, resp, sizeof resp, 1000);
    return (r >= 3 && resp[0] == 0x02 && resp[2] == 0x0e) ? 0 : -1;
}

int mvci_fast_init(mvci_ctx_t *ctx, const uint8_t *init, size_t n,
                   uint8_t *resp_out, size_t cap)
{
    uint8_t in[32], resp[64];
    int ilen = mvci_inner_fast_init(MVCI_ISO14230, init, n, in);
    int r = mvci_transact(ctx, in, ilen, resp, sizeof resp, 3000);
    if (r < 3 || resp[2] != 0x0e) return -1;
    int mlen = (int)resp[0] - 1;                 /* 08 -> 7 ECU key bytes @ off 3 */
    if (mlen < 0 || 3 + mlen > r) mlen = (r > 3) ? r - 3 : 0;
    if ((size_t)mlen > cap) mlen = (int)cap;
    if (mlen > 0) memcpy(resp_out, resp + 3, mlen);
    return mlen;
}

int mvci_write_msg(mvci_ctx_t *ctx, const uint8_t *msg, size_t n)
{
    uint8_t in[32], resp[64];
    int ilen = mvci_inner_write_msg(MVCI_ISO14230, msg, n, in);
    int r = mvci_transact(ctx, in, ilen, resp, sizeof resp, 1000);
    return r > 0 ? 0 : -1;
}

int mvci_poll(mvci_ctx_t *ctx, uint8_t *msg_out, size_t cap,
              uint8_t *rxstatus_out, int timeout_ms)
{
    uint8_t in[8], resp[64];
    int n = mvci_inner_read_poll(in);
    int r = mvci_transact(ctx, in, n, resp, sizeof resp, timeout_ms);
    if (r < 0) return -1;
    return mvci_parse_read_reply(resp, r, msg_out, cap, rxstatus_out);
}

int mvci_read_msg(mvci_ctx_t *ctx, uint8_t *msg_out, size_t cap, int timeout_ms)
{
    uint8_t rx = 0;
    int m = mvci_poll(ctx, msg_out, cap, &rx, timeout_ms);
    if (m > 0 && rx == 0x00) return m;       /* data */
    return 0;                                 /* echo / none */
}

int mvci_obd_request(mvci_ctx_t *ctx, uint8_t addr, uint8_t pid,
                     uint8_t *out, size_t cap, int timeout_ms)
{
    mvci_clear_periodic(ctx);                 /* arm a fresh K-line transaction */

    uint8_t msg[5] = { 0x82, addr, 0xf0, 0x01, pid };
    if (mvci_write_msg(ctx, msg, sizeof msg) != 0) return -1;

    uint32_t deadline = mvci_now_ms() + (uint32_t)timeout_ms;
    while ((int32_t)(deadline - mvci_now_ms()) > 0) {
        int m = mvci_read_msg(ctx, out, cap, 500);
        if (m > 0) return m;
        mvci_sleep_ms(20);
    }
    return -1;
}
