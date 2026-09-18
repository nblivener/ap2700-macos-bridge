#!/bin/zsh
set -euo pipefail
apusb_root=${0:A:h}
export WINEPREFIX="$apusb_root/build/wine-prefix"
export WINEDEBUG=-all
export MVK_CONFIG_LOG_LEVEL=0
export APUSB_TRACE=${APUSB_TRACE:-0}
apusb_wine="$apusb_root/build/wine-runtime/Wine Stable.app/Contents/Resources/wine/bin/wine"
apusb_app="$WINEPREFIX/drive_c/AP2700"
if [[ ! -x "$apusb_wine" || ! -f "$apusb_app/apbridge.dll" ]]; then
  print -u2 'The free Wine AP2700 installation is missing. See README.md.'
  exit 1
fi
cd -- "$apusb_app"
apusb_log="$apusb_root/research/reports/wine-ap2700-$(date +%Y%m%d-%H%M%S).log"
print 'Starting AP2700 with free Wine and the native Mac USB bridge.'
print "Log: $apusb_log"
print 'Keep the USB–APIB adapter connected to macOS. Close AP2700 to end this session.'
exec /usr/bin/python3 "$apusb_root/tools/with_bridge.py" \
  "$apusb_wine" 'C:\AP2700\Ap2700.exe' > "$apusb_log" 2>&1
