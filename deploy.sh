#!/bin/sh
# Copy the tracked port files onto a TrimUI SD card (or any folder).
#   deploy.sh /path/to/SDCARD            e.g. a mounted card, or "adb push" a staged copy
# It only writes files this repo owns; the game install and runtimes must already be in place (README.md).
set -e
SD=${1:?usage: deploy.sh SDCARD_ROOT}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$SD/Ports/aoe" "$SD/Ports/portmaster-aoe" "$SD/Data/ports"
cp "$HERE"/port/launch.sh "$HERE"/port/controls.ini "$HERE"/port/aoe_reg.reg "$HERE"/port/config.json "$HERE"/port/icon.png "$HERE"/port/xmirror.c "$SD/Ports/aoe/"
[ -f "$HERE/xmirror" ] && cp "$HERE/xmirror" "$SD/Ports/aoe/xmirror"
[ -f "$HERE/midisynth" ] && cp "$HERE/midisynth" "$SD/Ports/aoe/midisynth"
cp "$HERE"/port/midisynth.c "$HERE"/port/tsf.h "$SD/Ports/aoe/"
cp "$HERE"/portmaster-aoe/* "$SD/Ports/portmaster-aoe/"
cp "$HERE/data-ports/Age of Empires.sh" "$SD/Data/ports/"
mkdir -p "$SD/Ports/aoe/game" "$SD/Ports/aoe/weston/lib_armhf"
# cnc-ddraw (GDI renderer): the "EGO Play" edition ships its own GPU ddraw.dll that cannot start without Direct3D; keep it as ddraw.dll.ego
if [ -f "$SD/Ports/aoe/game/ddraw.dll" ] && ! cmp -s "$HERE/overlay/game/ddraw.dll" "$SD/Ports/aoe/game/ddraw.dll"; then
    [ -f "$SD/Ports/aoe/game/ddraw.dll.ego" ] || cp "$SD/Ports/aoe/game/ddraw.dll" "$SD/Ports/aoe/game/ddraw.dll.ego"
fi
cp "$HERE"/overlay/game/ddraw.dll "$HERE"/overlay/game/ddraw.ini "$HERE"/overlay/game/ddraw.ini.sw "$SD/Ports/aoe/game/"
cp "$HERE"/overlay/weston/lib_armhf/* "$SD/Ports/aoe/weston/lib_armhf/"
rm -f "$SD/Ports/aoe/weston/lib_armhf/libXrandr.so.2" "$SD/Ports/aoe/aoe.gptk"   # SIGSEGVs in XRRQueryExtension under Box86
chmod 755 "$SD/Ports/aoe/launch.sh" "$SD/Data/ports/Age of Empires.sh" 2>/dev/null || true
echo "deployed to $SD"
