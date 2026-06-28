# MVCI Wire Protocol Specification

This document describes the serial protocol spoken by the FTDI‑based "M‑VCI"
(Mini‑VCI) diagnostic cable, as implemented by `libMVCI`. It is provided for
interoperability and documentation purposes.

All multi‑byte integers are **little‑endian** unless noted. Byte values are
hexadecimal. "Inner" refers to a decrypted command/response body; "frame" refers
to the bytes actually on the wire.

---

## 1. Link layer

- USB: FTDI FT232R, VID `0403` / PID `6001`, USB product description `M-VCI`.
- Serial line: **115200 baud, 8 data bits, no parity, 1 stop bit**, no flow control.
- **MCU reset on open:** clear RTS, set DTR, wait 15 ms, clear DTR; then wait
  ~1 s for the adapter to boot before the first frame.

## 2. Frame format

Every frame, in both directions, has the form:

```
+-------+-------+---------------------------+--------+
| LEN   | 0x00  | PAYLOAD (LEN-3 bytes)     | XORSUM |
+-------+-------+---------------------------+--------+
  u8      u8      LEN-3 bytes                 u8
```

- **LEN** — total frame length in bytes, counting all four parts
  (`LEN = 3 + len(PAYLOAD)`). Maximum observed frame is well under 255 bytes.
- **0x00** — reserved, always zero.
- **PAYLOAD** — see §4 (plaintext for the handshake, otherwise DES‑encrypted).
- **XORSUM** — exclusive‑OR of every byte that precedes it (i.e. of `LEN`,
  `0x00`, and all `PAYLOAD` bytes).

### 2.1 Checksum rule

`XORSUM` is computed over the bytes as they appear on the wire — that is, over
the **payload as transmitted**:

- Frames the host sends, and device **status** replies (§6.1), checksum over the
  (encrypted) payload.
- Device **message** replies (§6.2) checksum over the **decrypted** inner body.

A robust receiver accepts a frame if `XORSUM` matches *either* the encrypted
payload or the decrypted inner. (`libMVCI` decrypts, then accepts if either rule
holds.)

## 3. Session establishment (handshake)

The handshake frames are **plaintext** (no encryption). Immediately after the
link‑layer reset (§1):

| Step | Direction | Frame | Notes |
|------|-----------|-------|-------|
| 1 | host → dev | `03 00 03` | Reset. No payload. Wait ~110 ms afterwards. |
| 2 | host → dev | `0C 00 07 00 01 4D 56 43 49 2D 54 62` | Identify. Payload = `07 00 01` + ASCII `"MVCI-T"`. |
| 3 | dev → host | `0E 00 09 00 01 K0 K1 K2 K3 K4 K5 K6 K7 cs` | Challenge. Payload = `09 00 01` + 8 key bytes. |

The **DES key for the rest of the session is `K0..K7`** — the 8 bytes following
`09 00 01` in the challenge reply (the trailing byte `cs` is the frame `XORSUM`).

The challenge is a per‑connection nonce: it advances on each handshake and resets
to a fixed starting value when the adapter is power‑cycled. The key is always
exactly the challenge bytes the device just sent, so no derivation is required.

## 4. Encryption

After the handshake, the `PAYLOAD` of every frame is:

```
PAYLOAD = DES-ECB( key, zero-pad(INNER, 8) )
```

- Cipher: **single DES in ECB mode** (FIPS 46‑3), standard S‑boxes, 8‑byte
  blocks, key parity bits ignored. Any standard DES implementation interoperates.
- `key` is the 8 bytes from the handshake (§3).
- `INNER` (the logical command/response, §5) is zero‑padded up to a multiple of
  8 bytes before encryption; the resulting ciphertext length is the payload
  length.

Decryption of received frames is the same operation in reverse
(`DES-ECB-decrypt`).

## 5. Inner command format

A decrypted inner body has the envelope:

```
+-------+-------+-------+----------------------+
| ILEN  | 0x00  | CMD   | ARGS ...             |
+-------+-------+-------+----------------------+
  u8      u8      u8      (ILEN-1) bytes
```

- **ILEN** — length of `CMD` + `ARGS` (i.e. `ILEN = 1 + len(ARGS)`).
- **CMD** — command opcode (§5.1).
- The full inner body is `ILEN + 2` bytes, then zero‑padded to 8 for encryption.

Some opcodes carry a **sub‑function** in the first argument byte (notably
`CMD 0x0E` and `CMD 0x09`). The `04 00 00 00` seen in several commands is the
J2534 **protocol id** as a u32 (ISO14230 = 4).

### 5.1 Command reference (host → device)

Byte templates below are the complete inner bodies (before padding). `[..]`
denotes a little‑endian field.

| Function | Opcode | Inner template | Reply (§6) |
|----------|--------|----------------|------------|
| Connect | `07` | `0D 00 07` `[proto u32]` `[flags u32]` `[baud u32]` | status `02 00 07` |
| Start message filter | `0B` | `10 00 0B` `[proto u32]` `[msgID u32]` `[type u32]` `mask(1)` `pattern(1)` | status `02 00 0B` |
| Set config (one param) | `0E`/`02` | `0E 00 0E 02` `[proto u32]` `[param u32]` `[value u32]` | status `02 00 0E` |
| Clear periodic msgs | `0E`/`09` | `06 00 0E 09` `[proto u32]` | status `02 00 0E` |
| Fast init | `0E`/`05` | `0A 00 0E 05` `[proto u32]` `init bytes…` | message reply (ECU key bytes) |
| Write message | `0A` | `0E 00 0A` `[proto u32]` `[flags u32]` `msg bytes…` | status `02 00 0A` |
| Read poll | `09`/`04` | `05 00 09 04 00 00 00 00` | message reply, or empty status |
| Keepalive | `09`/`06` | `05 00 09 06 00 00 00 00` | status `02 00 09` |
| Disconnect | `02` | `01 00 02 00 00 00 00 00` | status `02 00 02` |

Notes:
- **Filter `type`** values follow J2534: `1` = PASS, `2` = BLOCK, `3` = FLOW_CONTROL.
- **Set config** sends one parameter per command; J2534 parameter ids and values
  pass through unchanged (`param`, `value`).
- **Write message** `msg bytes` is the raw protocol message, e.g. an ISO14230
  request `82 <addr> F0 01 <pid>`.
- The host should issue **Clear periodic msgs** immediately before each Write
  message to arm a fresh K‑line transaction.

## 6. Responses (device → host)

After decryption, replies take one of two forms.

### 6.1 Status reply

```
02 00 <CMD> 00 00 00 00 <status>          (8 bytes, 1 block)
```

`<CMD>` echoes the command being acknowledged; `<status>` is a result byte.
Checksummed over the encrypted payload (§2.1).

### 6.2 Message reply (read poll / fast init)

```
+------+------+------+----------+----------------+-----------------+
| ILEN | 0x00 | 09   | RxStatus | 00 × 7         | MSG (ILEN-9)    |
+------+------+------+----------+----------------+-----------------+
   0      1      2       3         4..10            11..
```

- **RxStatus** (offset 3): `00` = received data message, `02` = transmit echo
  (loopback of the message the host just sent).
- **MSG** begins at offset 11; its length is `ILEN - 9`.
- If no message is available, the device returns a short status‑style reply
  (`ILEN` < 9, e.g. `02 00 09 10 …`) and no MSG.

Checksummed over the decrypted inner (§2.1).

A typical request/response exchange (read poll loop) returns, in order: zero or
more "no data" replies, one echo (`RxStatus = 02`), then the ECU data
(`RxStatus = 00`).

## 7. Keepalive

While a device is open the host must keep the adapter alive by transmitting
periodically (≈ every 15 ms); the adapter resets if traffic stops. The dedicated
keepalive command (§5.1) is used when idle. During an active read‑poll loop the
poll frames themselves satisfy this requirement.

## 8. Teardown

To close a session cleanly: send **Disconnect** (`01 00 02 …`), then close the
serial port / `FT_Close`. The next open performs the link‑layer reset (§1),
which re‑initialises the adapter MCU.

## 9. Worked example

`SET_CONFIG(param = 7, value = 0)` with session key `B0 CB 49 68 07 45 C8 7F`:

```
inner  = 0E 00 0E 02 04 00 00 00 07 00 00 00 00 00 00 00   (16 bytes, 2 blocks)
cipher = DES-ECB(key, inner)
       = 1A 7C EF A7 56 16 8C BC  3F 7D 9A 06 8E 58 87 E6
LEN    = 3 + 16 = 0x13
XORSUM = XOR(13, 00, cipher…) = 24
frame  = 13 00 1A 7C EF A7 56 16 8C BC 3F 7D 9A 06 8E 58 87 E6 24
```

The device acknowledges with a status reply that decrypts to
`02 00 0E 00 00 00 00 28`.

---

*This specification was determined by observation of the adapter's behaviour and
is documented here for interoperability. It describes an independent
implementation and contains no third‑party code.*
