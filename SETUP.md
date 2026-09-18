# AP2700 on Apple Silicon: reproducible setup

This project runs the original 32-bit AP2700 Windows application under free
Wine on Apple Silicon macOS. A native arm64 helper uses libusb to access the
USB–APIB adapter; an x86 Windows compatibility DLL (`apbridge.dll`) replaces
the Cypress driver calls made by AP's existing `apio.dll`.

This is a communication prototype. It was exercised with a powered Audio
Precision 2722A and the user confirmed that AP2700 appeared connected, but
measurement accuracy, every AP2700 feature, and long-term stability have not
been validated.

## Inputs and layout

Work from the repository root. The original AP2700 installer is supplied by
the user; its original license applies. The tested extracted application tree
was placed at:

```
research/installer/AP2700Setup/
```

It must contain at least `Ap2700.exe` and the original `apio.dll`. The
original x64 Cypress driver under the installer is retained for reference;
macOS does not load it.

The important project files are:

* `native/bridge.c` — arm64 macOS libusb server.
* `include/apusb_protocol.h` — authenticated loopback RPC protocol.
* `shim/apbridge.c` — x86 Windows DLL implementing the AP/Cypress calls used
  by AP2700.
* `tools/prepare_shim.py` — copies the AP tree and patches only the copied
  `apio.dll` imports.
* `tools/with_bridge.py` — starts a per-run helper with a random local port
  and token, then runs Wine.

The generated directories (`build/`, the extracted installer, and the Wine
archive) are local artifacts and are ignored by Git.

## Dependencies

Use Apple Silicon macOS 15 or later. Install Apple command-line tools, Rosetta 2, Homebrew `libusb`, and an x86
MinGW-w64 compiler. The downloaded Wine runtime and the AP2700 program are
x86, so Apple Silicon macOS needs Rosetta for this setup. On this machine the
relevant commands are:

```sh
softwareupdate --install-rosetta
brew install libusb mingw-w64
python3 -m venv .venv
.venv/bin/pip install pefile capstone
```

`capstone` is needed for the reverse-engineering reports; `pefile` is
required by `prepare_shim.py`. The runtime itself is the official Gcenx Wine
macOS build, version 11.0, downloaded from:

<https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.0_1/wine-stable-11.0_1-osx64.tar.xz>

The tested SHA-256 is
`b50dc50ec7f41d58b115a6b685d4d1315ba3c797bd3aa0f49213f2703cb82388`.
Release metadata: https://api.github.com/repos/Gcenx/macOS_Wine_builds/releases/tags/11.0_1 .

## Build the native helper and diagnostic tools

The following commands assume Homebrew's Apple Silicon prefix:

```sh
mkdir -p build
clang -std=c11 -Wall -Wextra -Werror -mmacosx-version-min=15.0 -Iinclude -I/opt/homebrew/opt/libusb/include/libusb-1.0 native/bridge.c -L/opt/homebrew/opt/libusb/lib -lusb-1.0 -o build/apusb-bridge
clang -std=c11 -Wall -Wextra -Werror -I/opt/homebrew/opt/libusb/include/libusb-1.0 tools/probe_usb.c -L/opt/homebrew/opt/libusb/lib -lusb-1.0 -o build/probe_usb
```

`bridge.c` targets USB vendor `13e4`, product `0003`, claims interface
0, alternate setting 0, and uses bulk OUT `0x02` and bulk IN `0x86`. The
helper listens only on `127.0.0.1`; `tools/with_bridge.py` supplies its
random port and 32-byte hexadecimal authentication token.

## Install Wine and AP2700 into an isolated prefix

Download and verify the archive, then extract it:

```sh
mkdir -p research/vendor build/wine-runtime
curl -L -o research/vendor/wine-stable-11.0_1-osx64.tar.xz https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.0_1/wine-stable-11.0_1-osx64.tar.xz
shasum -a 256 research/vendor/wine-stable-11.0_1-osx64.tar.xz
tar -xJf research/vendor/wine-stable-11.0_1-osx64.tar.xz -C build/wine-runtime
```

Set the runtime and prefix variables for subsequent commands:

```sh
export WINEPREFIX="$PWD/build/wine-prefix"
export WINE="$PWD/build/wine-runtime/Wine Stable.app/Contents/Resources/wine/bin/wine"
export WINEDEBUG=-all
export MVK_CONFIG_LOG_LEVEL=0
```

Initialize the clean prefix and install the user's installer. The tested
installer accepted a silent install into `C:\AP2700`:

```sh
env WINEPREFIX="$WINEPREFIX" WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=' "$WINE" wineboot -u
env WINEPREFIX="$WINEPREFIX" WINEDEBUG=-all MVK_CONFIG_LOG_LEVEL=0 "$WINE" /path/to/AP2700Setup.exe /S '/D=C:\AP2700'
```

Use the supplied installer under its applicable original license. If it uses a
different destination, adjust the destination used below.

## Prepare the Windows shim

Build the x86 shim from the original application tree. This copies the tree to
`build/ap2700`, creates a generated marker, patches the copied `apio.dll`
so its `KERNEL32.dll` and `SETUPAPI.dll` imports resolve through
`apbridge.dll`, and builds the forwarding export table. The source DLL is
checked by SHA-256 before and after patching and is never modified.

```sh
.venv/bin/python tools/prepare_shim.py --source research/installer/AP2700Setup --output build/ap2700
```

If the installer was not extracted into `research/installer/AP2700Setup`,
the installed application directory is also a valid source tree:

```sh
.venv/bin/python tools/prepare_shim.py --source "$WINEPREFIX/drive_c/AP2700" --output build/ap2700
```

For the analyzed supplied application, the original source DLL has SHA-256
`512197f0205c16c09f804db58243e278843436de1e6ad5d0226be581b590bf90`. Check
the source before preparing it if identifying the expected AP2700 build:

```sh
shasum -a 256 research/installer/AP2700Setup/apio.dll
```

The default compiler is `i686-w64-mingw32-gcc`; pass `--compiler` if the
MinGW executable has another name. If the application was installed directly
into the Wine prefix, copy the generated DLLs into the installed directory,
preserving a backup of the original:

```sh
if [[ ! -e "$WINEPREFIX/drive_c/AP2700/apio.dll.original" ]]; then
  cp "$WINEPREFIX/drive_c/AP2700/apio.dll" "$WINEPREFIX/drive_c/AP2700/apio.dll.original"
fi
cp build/ap2700/apio.dll build/ap2700/apbridge.dll "$WINEPREFIX/drive_c/AP2700/"
```

The current shim deliberately covers the observed AP sequence only: device
interface enumeration, open/close, Cypress control and bulk IOCTLs, completion
events, and `GetOverlappedResult`. It does not provide a Windows kernel
driver, device reset/port cycling, isochronous transfers, cancellation, or
general concurrent overlapped-I/O semantics.

## Verify before launching AP2700

Build and run the native protocol test only while AP2700 is closed:

```sh
python3 tools/test_bridge.py --hardware
```

This checks authentication and framing, opens the real adapter, reads the
device/configuration descriptors, performs a standard EP0 descriptor request,
and closes the device. It sends no AP measurement commands. The descriptor
test should identify `13e4:0003`.

Build the x86 smoke executable and place it beside the generated shim:

```sh
i686-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror tools/shim_smoke.c -o build/shim_smoke.exe -lsetupapi
cp build/shim_smoke.exe "$WINEPREFIX/drive_c/AP2700/"
```

Run the smoke test through the helper and Wine:

```sh
env WINEPREFIX="$WINEPREFIX" WINEDEBUG=-all MVK_CONFIG_LOG_LEVEL=0 python3 tools/with_bridge.py "$WINE" 'C:\AP2700\shim_smoke.exe'
```

It should report a real descriptor, packed Cypress header, completion event,
and `GetOverlappedResult` success. The wrapper writes private diagnostic logs under `research/reports/`; those logs are not published.

## Launch and evidence

Run AP2700 through the same wrapper, with its working directory set to the
installed application directory:

```sh
cd "$WINEPREFIX/drive_c/AP2700"
env WINEPREFIX="$WINEPREFIX" WINEDEBUG=-all MVK_CONFIG_LOG_LEVEL=0 python3 /path/to/tools/with_bridge.py "$WINE" 'C:\AP2700\Ap2700.exe'
```

`Launch AP2700.command` is the user-facing launcher assembled from these
steps. Keep the USB–APIB adapter connected directly to the Mac and close any
existing AP2700 session before starting another; the running process owns the
adapter. Logs are written under `research/reports/`. Set `APUSB_TRACE=1`
for per-transfer logging; leave it unset for normal operation.

The first recorded CrossOver run completed 112,292 IOCTLs and 111,807 bulk
transfers with no bridge errors. The free Wine smoke test also passed, and the
subsequent free Wine AP2700 session completed 93,178 IOCTLs and 92,693 bulk
transfers with no reported errors. These counts demonstrate communication
with the connected analyzer only; they are not a measurement validation.

## Make an Applications-folder app

The automated equivalent of this guide is `tools/setup_macos.sh`. After a manual
build, close AP2700 and stop the private prefix services before packaging:

```sh
"${WINE%/wine}/wineserver" -k || true
"${WINE%/wine}/wineserver" -w
python3 tools/package_app.py
```

The output is `build/package/AP2700.app`. The app includes the installed AP2700
prefix, so it is for your own deployment, not a public release. The build script
refuses to replace an existing output app. See README for install, diagnostics,
and runtime data locations. Never delete a used prefix without saving its data.
