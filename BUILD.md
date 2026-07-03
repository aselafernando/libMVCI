# Building the MVCI J2534 driver

A single codebase produces the J2534 PassThru library on all three platforms:

| Platform | Output              | Serial backend      | DES backend     |
|----------|---------------------|---------------------|-----------------|
| Linux    | `libMVCI.so`        | termios (`/dev/ttyUSBx`) | OpenSSL libcrypto |
| macOS    | `libMVCI.dylib`     | termios (`/dev/cu.usbserial*`) | CommonCrypto (system) |
| Windows  | `MVCI32.dll` (x86) / `MVCI64.dll` (x64) | FTDI D2XX (`ftd2xx.dll`) | Windows CNG (BCrypt) |

CMake is the single build system for every platform (Linux, macOS, Windows).

### Layout
```
include/mvci/   public headers      -> #include <mvci/j2534.h>, <mvci/serial.h>
src/            sources + private headers (io.h, compat.h, mvci.def)
test/           mvci_test.c (verification harness)
cmake/          MVCIConfig.cmake.in (package config template)
CMakeLists.txt  cross-platform build
```
Shared source: `src/passthru.c` (J2534 API), `src/serial.c` (protocol/session),
`src/io.c` (transport, `#ifdef`-selected), `src/des.c` (DES, `#ifdef`-selected).

---

## CMake (all platforms)

Requires CMake ≥ 3.15 and a C11 compiler. On Linux you also need OpenSSL headers
(`libssl-dev`); macOS and Windows use the OS-provided crypto, so there is no
external crypto dependency there.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build            # runs the hardware-free codec self-test
```

Outputs land in `build/` (multi-config generators like Visual Studio put them in
`build/Release/`): the driver library and `mvci_test`. Run a live test by passing
a port to the test binary, e.g. `./build/mvci_test /dev/cu.usbserial-XXXX` (macOS)
or `build\Release\mvci_test.exe M-VCI` (Windows).

Options:
- `-DMVCI_BUILD_TESTS=OFF` — skip the self-test target.
- `-DMVCI_INSTALL=OFF` — skip install/export rules.

Both default **ON** for a standalone build and **OFF** when MVCI is pulled in via
`add_subdirectory()`.

### Presets (Windows 32- and 64-bit)

The architecture is fixed per build tree (one compiler per tree), so 32- and
64-bit are separate configurations rather than one build. `CMakePresets.json`
provides them as named targets (CMake ≥ 3.21). They use the **NMake** generator,
which ships with MSVC and needs no extra tooling, and carry an `architecture`
hint (`strategy: external`) so **IDEs that read CMakePresets — Visual Studio,
VS Code CMake Tools — activate the right x86/x64 environment automatically** when
you pick the preset. From a terminal, run each in the **matching Native Tools
Command Prompt** (any Visual Studio edition):

```bat
:: In the "x64 Native Tools Command Prompt"  -> build\win64\MVCI64.dll
cmake --preset win64
cmake --build --preset win64
ctest --preset win64

:: In the "x86 Native Tools Command Prompt"  -> build\win32\MVCI32.dll
cmake --preset win32
cmake --build --preset win32
ctest --preset win32
```

(Equivalently, run `vcvarsall.bat x64` / `x86` in a plain prompt first.) The
output name (`MVCI32`/`MVCI64`) is chosen automatically from the target pointer
size, and the preset passes `MVCI_EXPECT_ARCH` so running `win32` from an x64
prompt (or vice-versa) fails immediately with a clear message instead of
silently building the wrong DLL. Both DLLs export the same undecorated
`PassThru*` symbols (via `src/mvci.def`), so a 32-bit J2534 host resolves them
just like a 64-bit one.

Prefer the Visual Studio generator? Configure two build trees explicitly:
`cmake -S . -B build\win32 -G "Visual Studio 17 2022" -A Win32` (and `-A x64`) —
substitute the generator string for your VS edition.

The `default` preset uses the platform's default generator for Linux/macOS.

### Including MVCI in another CMake project

**Vendored (subdirectory):**
```cmake
add_subdirectory(third_party/libMVCI)
target_link_libraries(my_app PRIVATE MVCI::MVCI)
```

**Installed (find_package):**
```sh
cmake --install build --prefix /your/prefix
```
```cmake
find_package(MVCI REQUIRED)          # add /your/prefix to CMAKE_PREFIX_PATH
target_link_libraries(my_app PRIVATE MVCI::MVCI)
```

Either way the consumer links the imported target `MVCI::MVCI`, which carries the
public include path and the platform crypto/thread dependencies automatically.
The consumable ABI is the J2534 `PassThru*` API (`#include <mvci/j2534.h>`); the
`serial.h` codec helpers are internal and are not exported from the shared
library.

---

## Per-platform notes

All three platforms build with the CMake steps above; what differs is the
runtime dependency and how the adapter enumerates.

### Linux

Needs OpenSSL headers (`sudo apt install libssl-dev`) plus a C11 compiler; DES
uses `libcrypto`. The adapter enumerates via the kernel `ftdi_sio` driver as
`/dev/ttyUSBx`. Tell the library which node to use with the `pName` argument to
`PassThruOpen` or the `MVCI_PORT` env var (default `/dev/ttyUSB0`), and ensure
port access (e.g. `sudo usermod -aG dialout $USER`). Live test:
`sudo ./build/mvci_test /dev/ttyUSB0`.

### macOS

No external dependency — DES runs through the system **CommonCrypto** framework
(part of `libSystem`). The adapter enumerates through Apple's built-in FTDI VCP
driver as `/dev/cu.usbserial-*` (or `/dev/tty.usbserial-*`); because the suffix
is device-specific, pass it via `pName` or `MVCI_PORT` (default
`/dev/cu.usbserial`). List candidates with `ls /dev/cu.usbserial-*`. Live test:
`./build/mvci_test /dev/cu.usbserial-XXXX`.

### Windows

No external dependency and **no FTDI SDK to build** — `bcrypt.lib` (DES) links
automatically, and `ftd2xx.dll` is loaded at runtime via `LoadLibrary` (no
`ftd2xx.h`/`ftd2xx.lib`). The only runtime requirement is the installed **FTDI
D2XX driver** (ships `ftd2xx.dll`): <https://ftdichip.com/drivers/d2xx-drivers/>.
CMake produces `MVCI32.dll` (x86) or `MVCI64.dll` (x64) with undecorated exports
(see `src/mvci.def`) so any J2534 client resolves `PassThruOpen` etc. on both
architectures — build each with the `win32` / `win64` presets above. The DLL
opens the adapter by its USB description `"M-VCI"`. Live test:
`build\win64\mvci_test.exe M-VCI`.

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
