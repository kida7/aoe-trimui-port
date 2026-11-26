#!/bin/sh
# ==============================================================================
# Age of Empires (Microsoft) - PortMaster Launcher for TrimUI Smart Pro
# ==============================================================================

PORTDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$PORTDIR"

# 0. Audio: the firmware "mute" flag (shmvar) can be 1 while MainUI is in the foreground; other launchers set it
# to 0 for the game. Do the same and put the old value back on exit.
SHMVAR=/usr/trimui/bin/shmvar
OLD_MUTE=
if [ -x "$SHMVAR" ]; then
    OLD_MUTE=$("$SHMVAR" mute 2>/dev/null)
    "$SHMVAR" mute 0 2>/dev/null || true
fi

# Arguments for the game exe. The "EGO Play" edition's Service.dll rejects "NOSTARTUP 1024" (it prints the arguments and exits with
# code 1, no window); the older edition wanted them. Put any arguments in Ports/aoe/game-args.txt (default: none).
GAME_ARGS=
[ -f "$PORTDIR/game-args.txt" ] && GAME_ARGS=$(cat "$PORTDIR/game-args.txt")

# 1. Setup Wine Prefix in RAM (/tmp) to avoid SD card wear and FAT32 symlink issues
export WINEPREFIX="/tmp/wine_aoe"
export WINEARCH=win32
export WINEDEBUG="${WINEDEBUG_OVERRIDE:--all}"
export WINEDLLOVERRIDES="ddraw=n,b;mscoree,mshtml="

if [ ! -f "$WINEPREFIX/system.reg" ] || [ ! -f "$WINEPREFIX/.update-timestamp" ]; then
    rm -rf "$WINEPREFIX"
    if [ -f "$PORTDIR/prefix_template.tar.gz" ]; then
        zcat "$PORTDIR/prefix_template.tar.gz" | tar -xf - -C /tmp/
    else
        mkdir -p "$WINEPREFIX/drive_c"
        mkdir -p "$WINEPREFIX/dosdevices"
    fi
    chown -R $(id -u):$(id -g) "$WINEPREFIX" 2>/dev/null
    echo 1794963566 > "$WINEPREFIX/.update-timestamp"
    # The template prefix has an empty system32: the MountMgr/winebus services (ImagePath ...\drivers\mountmgr.sys) could not
    # start, so Wine reported every drive as a fixed disk (GetDriveType=3) and AoE asked for a game CD.
    mkdir -p "$WINEPREFIX/drive_c/windows/system32/drivers"
    cp "$PORTDIR/wine/lib/wine/fakedlls/mountmgr.sys" "$PORTDIR/wine/lib/wine/fakedlls/winebus.sys" "$WINEPREFIX/drive_c/windows/system32/drivers/" 2>/dev/null
    # services.exe starts drivers through system32\winedevice.exe / plugplay.exe (and RpcSs through rpcss.exe): they must exist there
    for f in winedevice.exe plugplay.exe rpcss.exe; do
        cp "$PORTDIR/wine/lib/wine/$f" "$WINEPREFIX/drive_c/windows/system32/" 2>/dev/null
    done
    mkdir -p "$WINEPREFIX/dosdevices"
    ln -sf ../drive_c "$WINEPREFIX/dosdevices/c:"
    ln -sf / "$WINEPREFIX/dosdevices/z:"
    ln -sf "$PORTDIR/game" "$WINEPREFIX/dosdevices/d:"
    ln -sf "$PORTDIR/game" "$WINEPREFIX/drive_c/aoe"
fi

# Always point D: at THIS port's game folder. The prefix lives in /tmp and survives between launches, and another launcher (the
# test app) can create it first with D: pointing at its own game copy: the game then started the wrong copy and stuck on the splash.
ln -sfn "$PORTDIR/game" "$WINEPREFIX/dosdevices/d:"
ln -sfn "$PORTDIR/game" "$WINEPREFIX/drive_c/aoe"

# 2. Box86 & Wine paths
export BOX86_PATH="$PORTDIR/wine/bin"
export BOX86_LD_LIBRARY_PATH="$PORTDIR/wine/lib/wine:$PORTDIR/wine/lib"
export BOX86_DYNAREC=1
export BOX86_NOBANNER=1
# The "EGO Play" edition's Service.dll imports WLDAP32 (online login); Wine's wldap32 needs a native 32-bit libldap/liblber that the
# device lacks. Let box86 load it anyway; LDAP is only used for the online features.
export BOX86_ALLOWMISSINGLIBS=1
export BOX86_ALLOWMISSING_SYMBOLS=1

export LD_LIBRARY_PATH="$PORTDIR/wine/lib:$LD_LIBRARY_PATH"

# 2b. Apply registry once per prefix: AoE CD paths + X11 driver tweak
# (UseXVidMode=N: libXxf86vm crashes under box86 and winex11.drv then fails to load)
if [ ! -f "$WINEPREFIX/.aoe-reg-applied" ] || [ "$PORTDIR/aoe_reg.reg" -nt "$WINEPREFIX/.aoe-reg-applied" ]; then
    "$PORTDIR/box86" "$PORTDIR/wine/bin/wine" regedit /S "Z:$(echo "$PORTDIR/aoe_reg.reg" | tr '/' '\\')" >/dev/null 2>&1
    "$PORTDIR/box86" "$PORTDIR/wine/bin/wineserver" -w >/dev/null 2>&1
    touch "$WINEPREFIX/.aoe-reg-applied"
fi

# 3a. Controls: controls.ini -> gptokeyb config ([gamepad] + [gptokeyb] sections) in /tmp
GPTK=/tmp/aoe.gptk
awk '/^[ \t]*\[/ { sec=tolower($0); gsub(/[][ \t]/, "", sec); next }
     sec=="gamepad" || sec=="gptokeyb" { sub(/[ \t]*[#;].*/, ""); if ($0 ~ /=/) print }' "$PORTDIR/controls.ini" > "$GPTK" 2>/dev/null
[ -s "$GPTK" ] || echo "warning: no [gamepad] mapping generated from controls.ini" >> /tmp/aoe-controls.log
# controls.ini uses the labels printed on the device (A right, B bottom, X top, Y left). gptokeyb (SDL) names the
# face buttons the other way round on this pad (its "a" is the bottom button), so swap a<->b and x<->y here.
sed -i -e 's/^[ \t]*a[ \t]*=/@b =/' -e 's/^[ \t]*b[ \t]*=/@a =/' -e 's/^[ \t]*x[ \t]*=/@y =/' -e 's/^[ \t]*y[ \t]*=/@x =/' -e 's/^@//' "$GPTK" 2>/dev/null
rm -f /tmp/aoe-controls.log
TOG=$(awk '/^[ \t]*\[/ { sec=tolower($0); gsub(/[][ \t]/, "", sec); next } sec=="osk" { sub(/[ \t]*[#;].*/, ""); if ($1 ~ /^toggle/) { sub(/^[^=]*=/, ""); print } }' "$PORTDIR/controls.ini" 2>/dev/null | tr ',+' '  ')
for b in $TOG; do
    [ "$b" = "select" ] && b=back
    if grep -q "^[ \t]*$b[ \t]*=" "$GPTK" 2>/dev/null; then
        echo "warning: keyboard toggle button '$b' is also mapped in [gamepad]; pressing it will also act in the game" >> /tmp/aoe-controls.log
    fi
done
export XMIRROR_CONFIG="$PORTDIR/controls.ini"

# 3. Start Gamepad to Keyboard/Mouse mapper
start_gptokeyb() {
    if [ -f "$PORTDIR/gptokeyb" ]; then
        "$PORTDIR/gptokeyb" -c "$GPTK" "EMPIRESX.EXE" &
        GPTOK_PID=$!
    elif [ -f "$PORTDIR/gptokeyb2.armhf" ]; then
        "$PORTDIR/gptokeyb2.armhf" -c "$GPTK" "EMPIRESX.EXE" &
        GPTOK_PID=$!
    fi
}
start_gptokeyb
# After a screen-off/suspend the firmware (trimui_inputd) can recreate the pad's input device and gptokeyb keeps a dead fd (the
# sticks and buttons stop working).  Restart gptokeyb after every resume (Weston picks up the new uinput devices by hotplug).
SUSP0=$(dmesg 2>/dev/null | grep -c "PM: suspend exit")
(
    while sleep 3; do
        N=$(dmesg 2>/dev/null | grep -c "PM: suspend exit")
        if [ "$N" != "$SUSP0" ]; then
            SUSP0=$N; sleep 2
            killall -9 gptokeyb gptokeyb2.armhf 2>/dev/null; sleep 1
            start_gptokeyb
            echo "gptokeyb restarted after resume" >> /tmp/aoe-controls.log
        fi
    done
) &
RESUME_WATCH=$!

# 4. Launch game via Westonpack (Headless + Crusty SDL2 / Xwayland)
# NOTE: Do NOT use "drm gl" on TrimUI as it causes DRM Master lockup with MainUI.
# "headless noop kiosk $WGL" renders via SDL2 KMSDRM while running Xwayland cleanly.
# 3b. Screen output: weston runs headless (no real output), so mirror the game's X11 window
# to the real screen with xmirror (raw X11 protocol + SDL2 KMSDRM). See README.md in the port repo (aoe-trimui-port).
# First run after the user copied the game data: install the bundled cnc-ddraw (game-overlay/) into game/. The game's own
# ddraw.dll (the "EGO Play" edition ships one that needs Direct3D) is kept as ddraw.dll.ego. Skipped once it is in place.
OV="$PORTDIR/game-overlay"
if [ -f "$OV/ddraw.dll" ] && [ -d "$PORTDIR/game" ] && ! cmp -s "$OV/ddraw.dll" "$PORTDIR/game/ddraw.dll"; then
    if [ -f "$PORTDIR/game/ddraw.dll" ] && [ ! -f "$PORTDIR/game/ddraw.dll.ego" ]; then
        cp "$PORTDIR/game/ddraw.dll" "$PORTDIR/game/ddraw.dll.ego"
    fi
    cp "$OV/ddraw.dll" "$OV/ddraw.ini" "$OV/ddraw.ini.sw" "$PORTDIR/game/"
fi
if [ -f "$PORTDIR/game/ddraw.ini.sw" ]; then
    cp "$PORTDIR/game/ddraw.ini.sw" "$PORTDIR/game/ddraw.ini"   # cnc-ddraw, GDI renderer
fi
if [ -x "$PORTDIR/xmirror" ]; then
    XMIRROR_FPS="${XMIRROR_FPS:-60}" SDL_VIDEODRIVER=kmsdrm "$PORTDIR/xmirror" 0 > /tmp/xmirror.log 2>&1 &
    XM_PID=$!
fi

# The game checks the "CD": D: is declared cdrom (aoe_reg.reg), and it compares the volume label, which Wine reads from
# D:\.windows-label. The original Gold Edition disc label is "AOE".
[ -f "$PORTDIR/game/.windows-label" ] || printf 'AOE' > "$PORTDIR/game/.windows-label" 2>/dev/null

# 3c. Music: AoE's music is MIDI (SOUND\*.MID) and the device has no MIDI synthesizer. Run a small SoundFont synth (midisynth)
# that registers an ALSA sequencer port; Wine's winealsa MIDI driver looks for it when Wine starts, so start it first.
MS_PID=
SF=
for f in "$PORTDIR/soundfont.sf2" "$PORTDIR"/*.sf2; do [ -f "$f" ] && { SF="$f"; break; }; done
if [ -x "$PORTDIR/midisynth" ] && [ -n "$SF" ]; then
    "$PORTDIR/midisynth" "$SF" > /tmp/midisynth.log 2>&1 &
    MS_PID=$!
    i=0
    while [ $i -lt 6 ] && ! grep -q "ready" /tmp/midisynth.log 2>/dev/null; do sleep 1; i=$((i+1)); done
fi

# AoE reads DATA\, SOUND\ etc. relative to the working directory: run from game/
cd "$PORTDIR/game"
run_game() {
    BX="$PORTDIR/box86"; WGL="crusty_x11egl"
    if [ -f "$PORTDIR/weston/westonwrap32.sh" ]; then
        "$PORTDIR/weston/westonwrap32.sh" headless noop kiosk $WGL \
            $BX "$PORTDIR/wine/bin/wine" "D:\\EMPIRESX.EXE" $GAME_ARGS
    elif [ -f "$PORTDIR/weston/westonwrap.sh" ]; then
        "$PORTDIR/weston/westonwrap.sh" headless noop kiosk $WGL \
            $BX "$PORTDIR/wine/bin/wine" "D:\\EMPIRESX.EXE" $GAME_ARGS
    else
        "$PORTDIR/box86" "$PORTDIR/wine/bin/wine" "D:\\EMPIRESX.EXE" $GAME_ARGS
    fi
}

# Wine 6.0's GDI font cache sometimes crashes explorer.exe at startup under Box86
# ("Unhandled page fault ... find_cached_gdi_font"); the game window then never appears.
# Detect it in the log and restart the whole stack (max 4 tries). Log: /tmp/aoe-wine.log
rm -f /tmp/aoe.quit
ATT=0
while [ $ATT -lt 4 ] && [ ! -f /tmp/aoe.quit ]; do
    ATT=$((ATT+1))
    : > /tmp/aoe-wine.log
    T0=$(date +%s)
    run_game >> /tmp/aoe-wine.log 2>&1 &
    GP=$!
    RETRY=0
    while kill -0 $GP 2>/dev/null; do
        sleep 2
        if [ $(( $(date +%s) - T0 )) -lt 240 ] && grep -q "Unhandled page fault" /tmp/aoe-wine.log 2>/dev/null; then
            RETRY=1; break
        fi
    done
    if [ $RETRY = 1 ] && [ ! -f /tmp/aoe.quit ]; then
        echo "launch.sh: startup crash, retry $ATT" >> /tmp/aoe-retries.log
        "$PORTDIR/box86" "$PORTDIR/wine/bin/wineserver" -k >/dev/null 2>&1
        sleep 3
        kill -9 $GP 2>/dev/null
        killall -9 box86 wineserver wp_weston Xwayland seatd 2>/dev/null
        sleep 2
        continue
    fi
    wait $GP
    break
done

# 5. Cleanup on exit
[ -n "$RESUME_WATCH" ] && kill -9 "$RESUME_WATCH" 2>/dev/null
[ -n "$OLD_MUTE" ] && "$SHMVAR" mute "$OLD_MUTE" 2>/dev/null
if [ -n "$GPTOK_PID" ]; then
    kill -9 "$GPTOK_PID" 2>/dev/null
fi
[ -n "$XM_PID" ] && kill -9 "$XM_PID" 2>/dev/null
[ -n "$MS_PID" ] && kill "$MS_PID" 2>/dev/null
killall midisynth 2>/dev/null
killall -9 xmirror 2>/dev/null
killall -9 gptokeyb 2>/dev/null
killall -9 wineserver 2>/dev/null
killall -9 seatd 2>/dev/null

exit 0
