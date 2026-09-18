#!/bin/bash
# Change only the installed AP2700 app's Wine DPI; never restart a live session.
set -euo pipefail
SCALE="${1:-125}"
case "$SCALE" in 100|125|150|175|200) ;; *) echo 'Usage: set_scaling.sh [100|125|150|175|200]' >&2; exit 2 ;; esac
APP="${APUSB_APP:-/Applications/AP2700.app}"
WINE="$APP/Contents/Resources/Wine Stable.app/Contents/Resources/wine/bin/wine"
export WINEPREFIX="$HOME/Library/Application Support/AP2700/prefix"
export WINEDEBUG=-all MVK_CONFIG_LOG_LEVEL=0
if [[ ! -x "$WINE" || ! -d "$WINEPREFIX" ]]; then
  echo 'Install AP2700.app and open it once before changing its display scale.' >&2; exit 1
fi
DPI=$((96 * SCALE / 100))
"$WINE" reg add 'HKCU\Control Panel\Desktop' /v LogPixels /t REG_DWORD /d "$DPI" /f
"$WINE" reg query 'HKCU\Control Panel\Desktop' /v LogPixels
echo "AP2700 display scale set to $SCALE% ($DPI DPI). Save your work and reopen AP2700 when convenient to apply it."
