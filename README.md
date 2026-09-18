# AP2700 on Apple Silicon macOS

Run the original Audio Precision AP2700 Windows application on a Mac using
**free Wine and a native USB–APIB bridge**. No CrossOver subscription or Windows
VM is needed. This is an independent, experimental compatibility project.

Tested with AP2700 3.30 SP2, an Audio Precision 2722A, USB–APIB adapter
`13e4:0003`, and macOS 15.7.9 on an M1 Max. Communication works; measurement
accuracy and exhaustive feature coverage still need comparison with Windows.

## Build your app

You need an **Apple Silicon Mac**, macOS 15 or later, Rosetta 2, and your own
`AP2700Setup.exe` from the AP2700 3.30 SP2 installer. Intel Macs and other
installer versions are not covered by this setup script.

1. Install [Homebrew](https://brew.sh/), Apple's command-line tools
   (`xcode-select --install`), and Rosetta (`softwareupdate --install-rosetta`)
   if missing.
2. Download this repository or clone it, then run:

   ```sh
   git clone https://github.com/nblivener/ap2700-macos-bridge.git
   cd ap2700-macos-bridge
   ./tools/setup_macos.sh /path/to/AP2700Setup.exe
   ```

3. Drag `build/package/AP2700.app` into **Applications**. Connect the USB–APIB
   adapter to this Mac, power on the analyzer, and open **AP2700**.

The script installs build dependencies through Homebrew, downloads the pinned
Wine runtime and verifies its SHA-256, installs your AP2700 into a new isolated
Windows environment, builds the bridge, and packages the app. It stops if it
finds an existing build prefix or an unrecognized `apio.dll`. It does not modify
your installer. Allow a few GB of free disk space for the build.

The completed app contains Wine and libusb. It needs neither Homebrew nor Python
to run, and can be copied to another compatible Mac for your own installation.
**Do not publish the generated app or prefix:** it contains your proprietary
AP2700 installation and may contain local settings. This repository distributes
only the original bridge/launcher source and documentation, not AP2700 or a
preinstalled Windows environment. Third-party software retains its own license.

## Using it

Open **AP2700** from Applications. First launch creates a writable environment
under `~/Library/Application Support/AP2700/prefix` and may take a minute.
Logs go to `~/Library/Logs/AP2700/`. Close AP2700 to release the USB adapter and
end the helper session. Run one AP2700 instance per adapter.

The private Windows Documents folder is inside that prefix; Wine's `Z:` drive
also lets you access Mac files. Copying the app does not copy settings changed
after its first launch. Keep the USB adapter assigned to macOS if a VM is open.

If text is too small, run `./tools/set_scaling.sh 125` from this repository to
select 125% Wine scaling, then save your work and reopen AP2700 when convenient.
The script does not restart the app or change your Mac's display resolution.
Supported values are 100, 125, 150, 175, and 200; 100 restores the default.
This uses the same [LogPixels setting as Wine's configuration tool](https://github.com/wine-mirror/wine/blob/wine-11.0/programs/winecfg/x11drvdlg.c).

With the app closed and adapter attached, this diagnostic checks a real USB
descriptor, Cypress buffer layout, completion event, and returned byte count:

```sh
/Applications/AP2700.app/Contents/MacOS/AP2700 --hardware-smoke-test
```

It sends no AP measurement commands. Logs should contain `PASS: hardware
descriptor 13e4:0003`. For detailed transfer diagnostics, run the app executable
with `APUSB_TRACE=1`; ordinary launches log errors and lifecycle events.

If startup fails, inspect the log, check Rosetta and the USB connection, and
close any other program holding the adapter. Preserve the Application Support
folder before resetting it: that folder contains your Windows settings/files.

## How it works

```text
AP2700.exe (x86, Wine)
  -> original AP protocol in a copied apio.dll
  -> apbridge.dll (Win32 / Cypress IOCTL compatibility)
  -> authenticated per-session connection on 127.0.0.1
  -> apusb-bridge (native arm64, libusb)
  -> USB–APIB adapter -> analyzer
```

This bypasses the Windows kernel driver; it is not an ARM64 Windows driver port.
The tool patches the import names in a copy of `apio.dll`, redirecting selected
Windows USB calls while forwarding unrelated functions normally. It preserves
`apio.dll.original`. Measurement payloads come from the real hardware.

A full free-Wine session completed **93,178 IOCTLs and 92,693 bulk transfers with
zero reported errors**, and the owner confirmed normal apparent communication.
CrossOver worked in an earlier trial, but is not part of the default setup.
These observations establish communication, not measurement accuracy.

## Limits

- One adapter/client; interface 0, bulk OUT `02` and IN `86`.
- Blocking transfers with finite timeouts. Full asynchronous cancellation and
  concurrent overlapped transfers are not implemented; the most recent
  overlapped result is retained per device handle.
- No port cycling, device reset, or isochronous support.
- Driver version/address/speed and legacy power metadata use compatibility
  values. Measurement data is not synthesized.
- The helper uses a random local port and per-run token. It is intended for a
  trusted local desktop; a local process can deny service by occupying its
  single connection. It is not a network USB-sharing service.
- Long sessions, reconnect behavior, other analyzer models, and complete AP2700
  functionality still need testing. Compare measurements with a known-good
  Windows installation before relying on them.

## Development and reproduction

[SETUP.md](SETUP.md) gives the individual build/install/test commands and pinned
hashes for another developer or AI. Core files:

| File | Purpose |
|---|---|
| `native/bridge.c` | macOS libusb helper |
| `include/apusb_protocol.h` | Loopback protocol |
| `shim/apbridge.c` | x86 Windows compatibility DLL |
| `tools/prepare_shim.py` | Safe copy, import patching, forwarding exports |
| `tools/test_bridge.py`, `tools/shim_smoke.c` | Protocol and hardware checks |
| `packaging/Launcher.swift` | Native app launcher and process lifecycle |
| `tools/package_app.py` | Private app bundle builder |

To repackage an existing, closed source-tree installation, move the previous
`build/package/AP2700.app` aside and run `python3 tools/package_app.py`. The
packager copies that prefix, including its local state; use a fresh installation
for a clean template. A source-only release must never include `build/`,
`research/`, logs, registry files, or vendor binaries.

The original bridge and launcher code is [MIT licensed](LICENSE). Wine, libusb,
and AP2700 are separate components with their own licenses. This project is not
affiliated with or endorsed by Audio Precision, Wine, or CodeWeavers.
