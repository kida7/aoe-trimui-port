# Age of Empires (Rise of Rome) on TrimUI Smart Pro (Box86 + Wine)

Port glue for `/mnt/SDCARD/Ports/aoe` on a TrimUI Smart Pro (A523, aarch64, glibc 2.33). The game is the Windows x86
`EMPIRESX.EXE`, run by Box86 v0.3.9 + Wine 6.0 inside Westonpack (headless weston + Xwayland). Status: the main menu
renders on the real screen; mouse/gamepad play, "insert CD" prompt and sound are **not verified** (see Known issues).

## What this repo contains (and what it does not)
Tracked: `port/` (launcher, gamepad map, registry, menu entries, `xmirror.c`), `overlay/` (small files that must replace/add
to the runtime: `game/ddraw.ini` with `renderer=gdi`, 32-bit `libexpat`/`libuuid` for `weston/lib_armhf`),
`portmaster-aoe/`, `data-ports/`, `tools/build-xmirror.sh`, `deploy.sh`.

**Not in git (ignored):** the game install (`game/`, user-supplied, commercial), and the runtimes: `wine/`, `weston/`,
`box86`, `gptokeyb*`, `prefix_template.tar.gz`, the built `xmirror` binary.

## Game edition: "EGO Play" (what works, what does not)
The game folder currently on the card is an **"EGO Play"** edition of Rise of Rome (`EGOPlay.ini`, `ego.bin`, `ego_gpu.dll`, `D3dHook.dll`,
`Shaders/`, `Service.dll`, `ddraw.dll` 140 KB, `Empiresx.exe`). Two things stop it under Wine, found by running it on the host's
Wine with the same files:
- **Arguments:** `Service.dll` rejects `NOSTARTUP 1024` (it prints the arguments and exits with code 1 after 0.5 s, no window). The
  game must be started with **no arguments**; `launch.sh` reads optional arguments from `Ports/aoe/game-args.txt` (default none).
- **Renderer:** the edition's own `ddraw.dll` needs a GPU/Direct3D 9 (`GPUBackend=1`, `renderer=direct3d9`) and shows "Could not
  initialize graphics system" otherwise; `GPUBackend=0`/`renderer=gdi` in `EGOPlay.ini` did not help. Fix: use **cnc-ddraw** (GDI,
  `overlay/game/ddraw.dll` + `ddraw.ini`, MIT) in its place; the original is kept as `game/ddraw.dll.ego`, `EGOPlay.ini` is untouched.
- **WLDAP32:** `Service.dll` imports it; Wine's wldap32 needs a native 32-bit libldap/liblber the device lacks. `launch.sh` sets
  `BOX86_ALLOWMISSINGLIBS=1`/`BOX86_ALLOWMISSING_SYMBOLS=1` so it loads (the LDAP part is only for online features).
Result: menu -> Single Player -> Random Map -> Start Game -> **in the game** on sp1 (map and HUD shown about 40 s after Start Game,
load average ~4-5). The previous edition (plain Gold Edition files) hung at "Starting a new game..." even on the host's Wine, so
that was a problem of that edition's data/setup, not of box86.

## Frame rate
The picture is mirrored by `xmirror` (raw X11 `GetImage` of the Xwayland window, shown with SDL2 KMSDRM), capped at 60 fps (`XMIRROR_FPS`).
Measured 35-39 fps in the menu and in a match on the device's CPU path.

## Stuck on the splash after another launcher ran (shared prefix)
The Wine prefix is `/tmp/wine_aoe` and survives between launches. If another launcher (for example a test app) creates it first, its
`dosdevices/d:` points at that app's own game copy (with its own GDI `ddraw.ini`). The port then started that copy instead of its own, and hung on the splash. `launch.sh` now re-points `D:` (and `drive_c/aoe`) at its own `game/` on every launch. Other launchers should use their
own `WINEPREFIX` (e.g. `/tmp/wine_aoeport`).

## Setting up a device
Easiest: unzip the release package to the SD card root, copy your game files (the folder that contains `EMPIRESX.EXE`) into
`Ports/aoe/game/`, start "Age of Empires". On first start `launch.sh` installs the bundled cnc-ddraw from `Ports/aoe/game-overlay/`.

1. Put the user's own game install in `Ports/aoe/game/` (needs `EMPIRESX.EXE`, `DATA`, `DATA2`, `SOUND`, ... plus cnc-ddraw's `ddraw.dll`).
2. Runtimes in `Ports/aoe/`: `box86` (v0.3.9 built from source commit `bb716a2`, armhf), `wine/` (Wine 6.0 32-bit x86 "lean"),
   `weston/` (Westonpack 0.2.7 with `westonwrap32.sh`), `gptokeyb`, `prefix_template.tar.gz`.
3. Build `xmirror` for aarch64 (`CC=aarch64-linux-gnu-gcc tools/build-xmirror.sh`).
4. `./deploy.sh <SDCARD root>` (copies the tracked files, adds the overlay, and removes `weston/lib_armhf/libXrandr.so.2`).

## How it works / non-obvious fixes
- **`libXrandr.so.2` must not be in `weston/lib_armhf`**: it SIGSEGVs natively inside `XRRQueryExtension`, which made
  `winex11.drv` fail to load ("no driver could be loaded", exit 0). Without it Wine falls back to Xinerama.
- **32-bit `libexpat` + `libuuid` are required** by the 32-bit fontconfig; without them gdi32 crashes (looks like a
  `SHGetFolderPathW` error in the log).
- **`launch.sh` runs from `game/`**: AoE reads `DATA\...` relative to its working directory; run from the port folder it
  just sits in the message loop.
- **Screen output via `xmirror`**: weston runs `headless noop` (the `drm` backend hangs on this device) and Crusty only
  presents OpenGL/EGL, so a GDI/X11 game never reached the screen. `xmirror` reads the biggest X11 window from Xwayland
  with raw X11 `GetImage` and shows it through SDL2/KMSDRM (about 17 fps at 1280x720). It draws a normal arrow mouse
  cursor (2x, `XMIRROR_CURSOR_SCALE`) at the X pointer and an on-screen keyboard (below).
  OpenGL through Crusty does not work: `Could not create SDL Window: OpenGL context already created`.
- **Retry on startup crash**: Wine 6.0's GDI font cache (`find_cached_gdi_font`) sometimes crashes `explorer.exe` under
  Box86 (about 1 run in 5); `launch.sh` detects `Unhandled page fault` in `/tmp/aoe-wine.log` and restarts the stack
  (max 4 tries, `/tmp/aoe-retries.log`).
- **How Wine presents the game as "the CD"** (this is what made Single Player work; before, it always said "You must insert
  a game CD"): `D:` is a symlink (`dosdevices/d:`) to `game/`, declared a CD-ROM by `HKLM\Software\Wine\Drives "d:"="cdrom"`
  and the AoE `CDPath` values are `D:\` (`aoe_reg.reg`). Wine learns the drive type from its MountMgr service, which
  could not start because the template prefix has an empty `system32`: `launch.sh` now copies `mountmgr.sys`/`winebus.sys`
  (from `wine/lib/wine/fakedlls`) to `system32\drivers` and `winedevice.exe`/`plugplay.exe`/`rpcss.exe` to `system32` when it
  creates the prefix. Before: `GetDriveTypeW("D:\") -> 3` (fixed disk); after: `-> 5` (CD-ROM). The game then reads the volume
  label (`GetVolumeInformationW`, which Wine takes from `D:\.windows-label`) and compares it: `launch.sh` writes
  `game/.windows-label` = `AOE`, the label of the original Gold Edition disc (read from the ISO's volume descriptor).
  Keep the key name `"d:"` (with colon) in `Drives`; `drive_c/aoe` (`C:\aoe`) is no longer used by `CDPath`.
- `aoe_reg.reg` follows the game's own Setupreg.exe output (InstallType 3 = installed on a hard disk, InstallationDirectory,
  EXE Path, Version 1.0b, DirectPlay lobby entry) with every path set to `D:\`; the retail PID is left out. 32-bit prefix: no
  WOW6432Node. Verified: Single Player menu and the Single Player Game (random map) screen open.
- `aoe_reg.reg` also: `Audio=alsa`; the `UseXVidMode=N` key has no effect (harmless).
- Tried and dropped: Wine 9/10 (same libXrandr crash; `winewayland` init fails silently), weston `drm` backend
  (hangs seatd), `crusty_glx` (GL context error), `renderer=opengl`.

## Audio
Wine uses its ALSA driver (`Audio=alsa` in `aoe_reg.reg`) with the **real 32-bit libasound** in `overlay/weston/lib_armhf`
(a copy of the firmware's own `/usr/lib/arm-linux-gnueabihf/libasound.so.2`, read from the system, nothing written to it).
Why: Wine's `winealsa.drv` needs the full libasound API; shim libraries that export only a few `snd_*` functions never load the driver.
With the real libasound Wine opens the system
`default` PCM from `/etc/asound.conf` (softvol -> dmix -> `hw:audiocodec`, 48 kHz S16). Verified: while the game runs
`/proc/asound/card0/pcm0p/sub0/status` is RUNNING and owned by `EMPIRESX.EXE` (closed before launch). Audibility and
volume not verified by ear. `launch.sh` re-applies `aoe_reg.reg` when it is newer than the prefix marker.
The 32-bit libasound needs glibc 2.34 symbols; it loads fine here (the device's 32-bit libc is newer than the 64-bit one).

What the game actually plays (measured with `WINEDEBUG=+dsound`): DirectSound starts without errors (primary + 9
secondary buffers), mixing runs continuously, the **main menu is silent while idle**, and every activated menu button
plays one sound buffer (click). AoE's music is Red Book CD audio, so with no CD there is no music. The firmware flag
`/usr/trimui/bin/shmvar mute` was 1 (muted) outside the game; `launch.sh` sets it to 0 like other launchers and puts
the old value back on exit (verified 1 -> 0 -> 1). Quick audible test: start the game, move the highlight with the d-pad
or arrow keys and press a menu button (click sound).

## Music (MIDI) -- implemented, NOT yet verified with the game
AoE's music is **MIDI** (`game/SOUND/MUSIC*.MID`, `XMUSIC*.MID`), not CD audio, which is why it plays on a PC (Windows has a GM
synth). The device has no MIDI synth or soundfont, and the ALSA sequencer only has "System" and "OSS sequencer", so there is no
music. `midisynth` (`port/midisynth.c` + `tsf.h` = TinySoundFont, MIT) is a tiny General MIDI synth: it creates an ALSA sequencer
client "AoE MIDI Synth" with one writable port, renders SoundFont 2 with TSF and plays through the ALSA `default` PCM (dmix).
`launch.sh` starts it before Wine (winealsa scans the sequencer ports when Wine starts) and kills it on exit; it needs
`Ports/aoe/midisynth` (build: `tools/build-midisynth.sh`) and a General MIDI soundfont `Ports/aoe/soundfont.sf2` (not in git;
tested with TimGM6mb.sf2, 5.7 MB, Debian package `timgm6mb-soundfont`). Verified on sp1: the synth starts, loads the font (136
presets), registers sequencer client 128 port 0 and opens the PCM next to a running game (dmix 32/64-bit coexistence).
Not verified yet: Wine finding the port, the game sending MIDI, and anything audible.

## Controls (`controls.ini`)
One INI file (`Ports/aoe/controls.ini`, edited on the device or here) holds all input mapping:
- `[gamepad]` pad button = key / mouse action. `a b x y` are the labels **printed on the device** (A right, B bottom, X top, Y left);
  gptokeyb/SDL names them the other way round on this pad (measured: pad A 0x131 gave BTN_RIGHT), so `launch.sh` swaps
  `a<->b` and `x<->y` when it generates the gptokeyb config. Verified: A -> left click, B -> right click, X -> space, Y -> h.
  Mapping details: (gptokeyb names: `a = mouse_left`, `y = h`, `l2 = 1`, `left_analog_up =
  mouse_movement_up`, ...). `launch.sh` turns `[gamepad]` + `[gptokeyb]` into `/tmp/aoe.gptk` at every launch (awk), so edits
  apply on the next start. Leave a button out to leave it unmapped.
- `[gptokeyb]` dead zone / mouse speed, passed through unchanged.
- `[osk]` the virtual keyboard buttons: `toggle`, `type`, `backspace`, `space`, `shift`, `enter`, `close`; value =
  `button[+button...][, alternative...]` (chords with `+`), buttons `a b x y l1 r1 l2 r2 l3 r3 select start menu`.
  Defaults: toggle=r3 type=a backspace=b space=x shift=y enter=start close=select,l3.
- If the toggle button is also mapped in `[gamepad]` the game would see it too; `launch.sh` writes a warning to
  `/tmp/aoe-controls.log`. xmirror logs unknown button names to `/tmp/xmirror.log`.
(`aoe.gptk` is gone: `controls.ini` replaces it.) Verified on the device with injected pad events: default and an edited
ini are both read, `[gamepad] y = j` shows up in the generated gptokeyb config, the conflict warning appears, a chord
(`l1+r1`), an alternative (`l3`) and the analog trigger (`r2`) work as osk buttons, and a button removed from `toggle` stops toggling.

## Virtual keyboard (for typing names/chat)
Press the toggle button (default **R3**, right stick click, not mapped by gptokeyb) to open/close it. While open (default buttons, all configurable in `[osk]`): d-pad / left stick move,
**A** types the key, **B** backspace, **X** space, **Y** shift (one shot), **Start** enter, **Select/L3** close.
`gptokeyb` is paused (SIGSTOP) while it is open so the pad does not also move the mouse, and resumed on close.
Keys go through a uinput keyboard that `xmirror` creates at startup (it is started before weston so weston sees it).
Verified on the device: R3 opens/closes it, navigation works, and injected Down-arrow taps move the AoE menu
highlight (so uinput -> weston -> Xwayland -> Wine delivery works). Not verified: typing into an actual game text field.
Test FIFO `/tmp/xmirror.cmd`: `toggle`, `type TEXT`, `warp X Y`, `where`, `tapkey CODE`, `up|down|left|right|ok|back|space|shift|enter|close`.
Note: Remote Debug locks the pad buttons while the screen is off; do not inject KEY_POWER blindly (it toggles the screen).

## Known issues / not verified
- Gamepad -> mouse/keyboard (gptokeyb) and actual gameplay: only the menu was verified.
- A random-map game starts and shows the map and HUD; actual play (input, speed, sound/music in-game) beyond that is not verified.
- Back up saves before touching the device; do not modify system files.

## Donate / Ủng hộ

If this port is useful to you, you can support the author:

- Bank: **TPBank**
- Account number: **0973886038**
