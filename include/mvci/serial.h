/* MVCI wire protocol — DES-ECB based, fully reverse engineered.
 *
 * Transport is abstracted (see mvci_io.h): Linux/macOS use termios on a serial
 * node, Windows uses FTDI D2XX by description "M-VCI".  DES is abstracted too
 * (mvci_des.c): OpenSSL on Linux, CommonCrypto on macOS, CNG/BCrypt on Windows.
 *
 * -----------------------------------------------------------------------
 * Frame:   [LEN][0x00][PAYLOAD : LEN-3 bytes][XORSUM]
 *   LEN    = total frame length (incl. all 4+ parts)
 *   XORSUM = XOR of every byte before it
 *   PAYLOAD is plaintext for the pre-key handshake frames; otherwise it is
 *           DES-ECB(key, inner) with `inner` zero-padded to a multiple of 8.
 *
 * Cipher:  single DES-ECB. key = first 8 bytes of the connect challenge,
 *          which the device sends in the clear: 0e 00 09 00 01 <K0..K7> <sum>.
 *
 * Handshake:  TX 03 00 03 ; TX 0c 00 07 00 01 "MVCI-T" <sum> ;
 *             RX 0e 00 09 00 01 <8 key> <sum>
 *
 * Inner command bodies (decrypted):
 *   connect       0d 00 07 <proto u32><flags u32><baud u32>
 *   start filter  10 00 0b <proto u32><msgid u32><type u32><mask><pattern>
 *   set config    0e 00 0e 02 04 00 00 00 <param u32><value u32>
 *   clear periodic 06 00 0e 09 04 00 00 00
 *   fast init     0a 00 0e 05 04 00 00 00 <init bytes>
 *   write msg     0e 00 0a 04 00 00 00 00 00 00 00 <msg bytes>
 *   read poll     05 00 09 04 00 00 00 00
 *   keepalive     05 00 09 06 00 00 00 00
 *   disconnect    01 00 02 00 00 00 00 00
 *   reply         [len] 00 09 <rxstatus> ... <msg @ off 11>, msglen = len-9
 *                 status reply 02 00 <cmd> 00 00 00 00 <status>
 */
#ifndef MVCI_SERIAL_H
#define MVCI_SERIAL_H

#include <stdint.h>
#include <stddef.h>

#define MVCI_BAUD        115200
#define MVCI_MAX_INNER   256
#define MVCI_MAX_FRAME   (3 + MVCI_MAX_INNER)

/* ======================================================================
 *  Pure codec (no I/O — unit testable)
 * ====================================================================== */
uint8_t mvci_xorsum(const uint8_t *p, size_t n);

void mvci_des_encrypt(const uint8_t key[8], uint8_t *buf, size_t nblocks);
void mvci_des_decrypt(const uint8_t key[8], uint8_t *buf, size_t nblocks);

int mvci_frame_plain(const uint8_t *payload, size_t plen, uint8_t *out, size_t out_cap);
int mvci_frame_enc(const uint8_t key[8], const uint8_t *inner, size_t inner_len,
                   uint8_t *out, size_t out_cap);
int mvci_frame_payload(const uint8_t *frame, size_t frame_len, const uint8_t **payload_out);
int mvci_frame_decrypt(const uint8_t key[8], const uint8_t *frame, size_t frame_len,
                       uint8_t *inner_out, size_t inner_cap);

int mvci_inner_set_config(uint32_t param, uint32_t value, uint8_t *inner);
int mvci_inner_connect(uint32_t proto, uint32_t flags, uint32_t baud, uint8_t *inner);
int mvci_inner_start_filter(uint32_t proto, uint32_t msgid, uint32_t type,
                            uint8_t mask, uint8_t pattern, uint8_t *inner);
int mvci_inner_clear_periodic(uint8_t *inner);
int mvci_inner_fast_init(uint32_t proto, const uint8_t *init, size_t n, uint8_t *inner);
int mvci_inner_write_msg(uint32_t proto, const uint8_t *msg, size_t n, uint8_t *inner);
int mvci_inner_read_poll(uint8_t *inner);

/* Parse a decrypted reply. Returns message length (>0) with the message copied
 * to msg_out for echo/data replies, 0 for a status reply with no message, or -1.
 * *rxstatus_out receives the device RxStatus byte (0=data, 2=tx echo). */
int mvci_parse_read_reply(const uint8_t *inner, int inner_len,
                          uint8_t *msg_out, size_t cap, uint8_t *rxstatus_out);

/* ======================================================================
 *  Session (transport + protocol)
 * ====================================================================== */
typedef struct mvci_ctx mvci_ctx_t;

mvci_ctx_t *mvci_open(const char *port);
void        mvci_close(mvci_ctx_t *ctx);

int            mvci_handshake(mvci_ctx_t *ctx);       /* reset+identify -> key */
const uint8_t *mvci_key(const mvci_ctx_t *ctx);

/* send an encrypted inner command, read+decrypt one reply (thread-safe) */
int mvci_transact(mvci_ctx_t *ctx, const uint8_t *inner, size_t inner_len,
                  uint8_t *resp_inner, size_t resp_cap, int timeout_ms);

/* background keepalive thread (05 00 09 06 ...) — start after handshake */
int  mvci_start_keepalive(mvci_ctx_t *ctx);
void mvci_stop_keepalive(mvci_ctx_t *ctx);

/* J2534 operations (ISO14230) */
int mvci_connect(mvci_ctx_t *ctx, uint32_t proto, uint32_t flags, uint32_t baud);
int mvci_disconnect(mvci_ctx_t *ctx);
int mvci_start_filter(mvci_ctx_t *ctx, uint32_t msgid, uint8_t mask, uint8_t pattern);
int mvci_set_config(mvci_ctx_t *ctx, uint32_t param, uint32_t value);
int mvci_clear_periodic(mvci_ctx_t *ctx);
int mvci_fast_init(mvci_ctx_t *ctx, const uint8_t *init, size_t n,
                   uint8_t *resp_out, size_t cap);
int mvci_keepalive(mvci_ctx_t *ctx);

int mvci_write_msg(mvci_ctx_t *ctx, const uint8_t *msg, size_t n);

/* One read poll. Returns msg length (>0) with *rxstatus set, 0 if none, -1 err. */
int mvci_poll(mvci_ctx_t *ctx, uint8_t *msg_out, size_t cap,
              uint8_t *rxstatus_out, int timeout_ms);

/* Poll until a data (RxStatus==0) message arrives or timeout. Returns len/-1. */
int mvci_read_msg(mvci_ctx_t *ctx, uint8_t *msg_out, size_t cap, int timeout_ms);

/* Convenience: 82 <addr> F0 01 <pid> request -> ECU data reply. */
int mvci_obd_request(mvci_ctx_t *ctx, uint8_t addr, uint8_t pid,
                     uint8_t *out, size_t cap, int timeout_ms);

#endif /* MVCI_SERIAL_H */
