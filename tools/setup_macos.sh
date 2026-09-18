#!/bin/bash
# Build a private AP2700 app from the user's own installer. No vendor files in Git.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -ne 1 || ! -f "$1" ]]; then
  echo "Usage: $0 /path/to/AP2700Setup.exe" >&2
  exit 2
fi
INSTALLER="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
  echo 'This setup supports Apple Silicon macOS only.' >&2; exit 1
fi
if [[ "$(sw_vers -productVersion | cut -d . -f 1)" -lt 15 ]]; then
  echo 'This package targets macOS 15 or later.' >&2; exit 1
fi
if ! /usr/bin/arch -x86_64 /usr/bin/true; then
  echo 'Install Rosetta 2 with: softwareupdate --install-rosetta' >&2; exit 1
fi
if ! command -v brew >/dev/null || ! xcrun --find swiftc >/dev/null; then
  echo 'Install Homebrew and Apple command-line tools first (see README.md).' >&2; exit 1
fi
cd "$ROOT"
if [[ -e build/wine-prefix || -e build/package/AP2700.app ]]; then
  echo 'An installation already exists under build/. See README.md for rebuilding; existing settings were not changed.' >&2; exit 1
fi
export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_INSTALL_UPGRADE=1 HOMEBREW_NO_INSTALL_CLEANUP=1
for dependency in libusb mingw-w64; do
  if ! brew list --versions "$dependency" >/dev/null 2>&1; then
    brew install "$dependency"
  fi
done
if command -v python3 >/dev/null && python3 -c 'import sys; assert sys.version_info >= (3, 9)' 2>/dev/null; then
  PYTHON="$(command -v python3)"
else
  brew install python
  PYTHON="$(brew --prefix python)/bin/python3"
fi
"$PYTHON" -m venv .venv
.venv/bin/pip install 'pefile==2024.8.26'
mkdir -p build/wine-runtime research/vendor
ARCHIVE="${APUSB_WINE_ARCHIVE:-$ROOT/research/vendor/wine-stable-11.0_1-osx64.tar.xz}"
if [[ ! -f "$ARCHIVE" ]]; then
  curl --fail --location --retry 3 -o "$ARCHIVE" https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.0_1/wine-stable-11.0_1-osx64.tar.xz
fi
EXPECTED=b50dc50ec7f41d58b115a6b685d4d1315ba3c797bd3aa0f49213f2703cb82388
ACTUAL="$(shasum -a 256 "$ARCHIVE" | cut -d ' ' -f 1)"
if [[ "$ACTUAL" != "$EXPECTED" ]]; then
  echo 'Wine archive checksum mismatch; stopping.' >&2; exit 1
fi
tar -xJf "$ARCHIVE" -C build/wine-runtime
export WINEPREFIX="$ROOT/build/wine-prefix"
export WINEDEBUG=-all MVK_CONFIG_LOG_LEVEL=0 WINEDLLOVERRIDES='mscoree,mshtml='
WINE="$ROOT/build/wine-runtime/Wine Stable.app/Contents/Resources/wine/bin/wine"
"$WINE" wineboot -u
"$WINE" "$INSTALLER" /S '/D=C:\AP2700'
DLL="$WINEPREFIX/drive_c/AP2700/apio.dll"
ACTUAL="$(shasum -a 256 "$DLL" | cut -d ' ' -f 1)"
if [[ "$ACTUAL" != 512197f0205c16c09f804db58243e278843436de1e6ad5d0226be581b590bf90 ]]; then
  echo 'This installer contains an untested apio.dll. Stopping before patching. See SETUP.md.' >&2; exit 1
fi
LIBUSB="$(brew --prefix libusb)"
clang -std=c11 -Wall -Wextra -Werror -mmacosx-version-min=15.0 -Iinclude -I"$LIBUSB/include/libusb-1.0" \
  native/bridge.c -L"$LIBUSB/lib" -lusb-1.0 -o build/apusb-bridge
.venv/bin/python tools/prepare_shim.py --source "$WINEPREFIX/drive_c/AP2700" --output build/ap2700
cp "$DLL" "$DLL.original"
cp build/ap2700/apio.dll build/ap2700/apbridge.dll "$WINEPREFIX/drive_c/AP2700/"
i686-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror tools/shim_smoke.c -o build/shim_smoke.exe -lsetupapi
.venv/bin/python tools/test_bridge.py
# Stop only this private prefix's services before copying its template.
"${WINE%/wine}/wineserver" -k || true
"${WINE%/wine}/wineserver" -w
.venv/bin/python tools/package_app.py
echo "Ready: $ROOT/build/package/AP2700.app"
echo 'Drag AP2700.app into Applications, attach the USB–APIB adapter, and open it.'
