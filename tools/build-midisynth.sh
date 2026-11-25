#!/bin/sh
# Build midisynth (aarch64) from port/midisynth.c + port/tsf.h (TinySoundFont, MIT). Needs ALSA headers and the device's
# 64-bit libasound.so.2 to link against (copy it from /usr/lib on the device).
#   CC=aarch64-linux-gnu-gcc ALSA_INC=/usr/include ASOUND=/path/to/libasound.so.2 tools/build-midisynth.sh
set -e
CC=${CC:-aarch64-linux-gnu-gcc}
cd "$(dirname "$0")/.."
$CC -O2 -Wall -Wno-unused-function ${ALSA_INC:+-I $ALSA_INC} -o midisynth port/midisynth.c ${ASOUND:--lasound} -lm \
    -Wl,--unresolved-symbols=ignore-in-shared-libs
echo "built ./midisynth (copy next to launch.sh; also put a General MIDI soundfont there, e.g. TimGM6mb.sf2 as soundfont.sf2)"
