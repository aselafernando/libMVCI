# libMVCI — an open J2534 driver for the Mini-VCI USB adapter

Cross-platform implementation of the **SAE J2534 PassThru API** for the
common FTDI FT232R–based *Mini‑VCI / "M‑VCI"* diagnostic cable, so the hardware
you own can be used with standard J2534 diagnostic software — including on
**Linux** and **macOS**, where no vendor driver exists.

| Platform | Output | Serial backend | Crypto backend |
|---|---|---|---|
| Linux   | `libMVCI.so` | termios (`/dev/ttyUSBx`) | OpenSSL libcrypto |
| macOS   | `libMVCI.dylib` | termios (`/dev/cu.usbserial-*`) | CommonCrypto (system) |
| Windows | `MVCI32.dll` (x86), `MVCI64.dll` (x64) | FTDI D2XX (`ftd2xx.dll`, loaded at runtime) | Windows CNG (BCrypt) |

Built with CMake; the library lands in the build tree (e.g. `build/`). See
**[BUILD.md](BUILD.md)**.

## Why this exists — interoperability

This project was created **for interoperability**. The "M‑VCI" cable ships with a
closed, Windows‑only J2534 DLL, which means owners of the hardware cannot use it
with J2534 tools on Linux, nor with their own software, and have no documentation
of how the device communicates.

`libMVCI` is an independent, re‑implementation of the publicly‑observable
J2534 interface and the cable's serial wire protocol, written so that:

- the same physical adapter works across operating systems (notably Linux);
- it can be used as a drop‑in J2534 provider for existing diagnostic
  applications; and
- the device's behaviour is documented and not locked to one vendor's binary.

It contains **no vendor code** — only an original implementation that speaks the
same wire protocol.

## Status

- ✅ **ISO14230 / KWP2000 (K‑line)** — connect, message filters, `SET_CONFIG`,
  fast‑init, and OBD request/response are implemented and tested on a vehicle
  (live PID reads; works as a J2534 provider for Toyota Techstream over K‑line).
- ⬜ **ISO15765 / CAN** — not yet implemented. CAN ECUs are not yet reachable.
- The wire framing (DES‑based) and session handling are fully implemented and
  identical across all platforms; only the protocol/transport/crypto *backends*
  differ per OS, selected at compile time.

## Hardware

FTDI FT232R, USB VID `0x0403` / PID `0x6001`, USB description **`M-VCI`**
(115200 8N1). On Linux it enumerates through the kernel `ftdi_sio` driver as
`/dev/ttyUSBx`; on macOS through Apple's FTDI VCP driver as `/dev/cu.usbserial-*`;
on Windows it is opened directly through `ftd2xx.dll`. No `libusb` dependency exists.

## Building

CMake is the cross-platform build (Linux, macOS, Windows). See
**[BUILD.md](BUILD.md)** for per-platform notes and how to consume MVCI from
another CMake project.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build            # codec self-test, no hardware needed
```

On Windows, build a specific architecture with the bundled presets (from the
matching Native Tools Command Prompt, or by selecting the preset in your IDE):

```bat
cmake --preset win64   &&  cmake --build --preset win64   :: MVCI64.dll
cmake --preset win32   &&  cmake --build --preset win32   :: MVCI32.dll
```

Reuse it from another CMake project — either vendored:

```cmake
add_subdirectory(external/libmvci)
target_link_libraries(your_target PRIVATE libmvci)
```

or installed:

```cmake
find_package(libmvci CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE libMVCI::libmvci)
```

## Using it

The library exports the standard J2534 `PassThru*` functions, so any J2534 host
application can load it.

- **Windows:** register/point your tool at `MVCI32.dll` (32‑bit hosts) or
  `MVCI64.dll` (64‑bit). It opens the adapter by its USB description `M-VCI`.
- **Linux:** load `libMVCI.so`. Tell it which serial node to use via the
  `pName` argument to `PassThruOpen`, or the `MVCI_PORT` environment variable
  (default `/dev/ttyUSB0`). Add yourself to the `dialout` group for port access.
- **macOS:** load `libMVCI.dylib`. Same as Linux, but the FTDI VCP node is
  `/dev/cu.usbserial-*` (`MVCI_PORT` default `/dev/cu.usbserial`).

The MVCI session layer can also be used directly via `#include <mvci/serial.h>`
(see `test/mvci_test.c` for an example: handshake → connect → fast‑init → read
PIDs).

## Layout

```
include/mvci/    public headers      ->  #include <mvci/j2534.h>, <mvci/serial.h>
src/             sources + private headers (io.h, compat.h, mvci.def)
test/            mvci_test.c — cross-platform self-test + live harness
docs/            protocol specification (PROTOCOL.md)
cmake/           package-config template (libmvciConfig.cmake.in)
CMakeLists.txt   cross-platform build
CMakePresets.json  win32/win64 + default build presets
build/           build output (generated)
```

## Documentation

The wire protocol and frame format are specified in
**[docs/PROTOCOL.md](docs/PROTOCOL.md)**.

## License

[LICENSE.md](LICENSE.md)

## Disclaimer

This is an independent, unofficial project for **interoperability** with hardware
the user already owns. It is not affiliated with, authorised by, or endorsed by
Toyota, XHorse, FTDI, or any J2534 software vendor. All product and company names
and trademarks are the property of their respective owners. Provided as‑is, with
no warranty; use at your own risk.
