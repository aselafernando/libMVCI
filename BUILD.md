# Building the MVCI J2534 driver

A single codebase produces the J2534 PassThru library on both platforms:

| Platform | Output (in `lib/`) | Serial backend      | DES backend     |
|----------|--------------------|---------------------|-----------------|
| Linux    | `lib/libMVCI.so`   | termios (`/dev/ttyUSBx`) | OpenSSL libcrypto |
| Windows  | `MVCI32.dll` (x86) / `MVCI64.dll` (x64) | FTDI D2XX (`ftd2xx.dll`) | Windows CNG (BCrypt) |

### Layout
```
include/mvci/   public headers      -> #include <mvci/j2534.h>, <mvci/serial.h>
src/            sources + private headers (io.h, compat.h, mvci.def)
test/           mvci_test.c (verification harness)
lib/            built library
```
Shared source: `src/passthru.c` (J2534 API), `src/serial.c` (protocol/session),
`src/io.c` (transport, `#ifdef`-selected), `src/des.c` (DES, `#ifdef`-selected).

---

## Linux (make)

Requires `gcc`, OpenSSL headers (`libssl-dev`), and pthreads.

```sh
sudo apt install build-essential libssl-dev
make                 # builds lib/libMVCI.so and mvci_test
make test            # runs the codec self-test (no hardware)
sudo ./mvci_test /dev/ttyUSB0   # live test on the adapter
sudo make install    # installs lib/libMVCI.so + headers under /usr/local
```

The adapter enumerates via the kernel `ftdi_sio` driver as `/dev/ttyUSBx`.
A J2534 client tells the library which node to use by either:
- passing the path as `pName` to `PassThruOpen`, or
- setting the `MVCI_PORT` environment variable (default `/dev/ttyUSB0`).

Ensure the user can access the port (e.g. `sudo usermod -aG dialout $USER`).

## Windows (Visual Studio)

Requires only Visual Studio 2022 (v143/v145 toolset; retarget if older).
**No FTDI SDK is needed to build** — `ftd2xx.dll` is loaded at runtime via
`LoadLibrary`, so there is no `ftd2xx.h`/`ftd2xx.lib` dependency and no
`FTD2XX_DIR` to set. (`Directory.Build.props` is therefore unused and can be
deleted.) `bcrypt.lib` is a standard Windows system library and links
automatically. The only runtime requirement is the installed **FTDI D2XX
driver** (which provides `ftd2xx.dll`):
<https://ftdichip.com/drivers/d2xx-drivers/>.

1. Open `MVCI.sln` (two projects: **MVCI** = the DLL, **mvci_test** = the
   verification EXE), pick a configuration:
   - **Release | Win32** → `lib\Release\MVCI32.dll`
   - **Release | x64**   → `lib\Release\MVCI64.dll`
   The test EXE builds to `bin\<platform>\Release\mvci_test.exe`.
3. Build Solution.

Verify with the same test program used on Linux (it's one cross-platform
source — the platform backends are selected at compile time):

```bat
bin\x64\Release\mvci_test.exe            :: codec self-test, no hardware
bin\x64\Release\mvci_test.exe M-VCI      :: live handshake + OBD on the adapter
```

DES uses the OS crypto (`bcrypt.lib`, linked automatically) — no OpenSSL needed
on Windows. The DLL opens the adapter through `ftd2xx.dll` by its USB
description `"M-VCI"`, so the FTDI D2XX runtime must be installed (it ships with
the FTDI driver). The exports are undecorated (see `mvci.def`) so any J2534
client resolves `PassThruOpen` etc. on both x86 and x64.

> The default Windows COM-port / VCP driver is **not** used — the DLL talks to
> the FTDI chip directly via D2XX, matching the original MVCI32.dll.

---

## Notes

- The library models one ISO14230 channel per opened device (the adapter's
  single K-line). `PassThruOpen` runs the handshake and starts a background
  keepalive thread; `PassThruClose` stops it.
- `PassThruReadMsgs` returns the device RxStatus byte unchanged (`0` = received
  data, `2` = TX echo), matching the original DLL's behaviour.
- Only the IOCTLs the adapter implements are wired up: `SET_CONFIG`,
  `CLEAR_PERIODIC_MSGS`, `FAST_INIT`, and the buffer/filter clears (no-ops).
  Others return `ERR_NOT_SUPPORTED`.
