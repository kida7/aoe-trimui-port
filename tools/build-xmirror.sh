#!/bin/sh
# Build xmirror (aarch64, glibc >= 2.17) from port/xmirror.c.
# Needs an aarch64 C compiler, e.g. on tcua: Ubuntu 18.04 gcc-7-aarch64-linux-gnu unpacked from apt debs (no root),
# or any aarch64-linux-gnu-gcc.   usage: CC=aarch64-linux-gnu-gcc tools/build-xmirror.sh
set -e
CC=${CC:-aarch64-linux-gnu-gcc}
cd "$(dirname "$0")/.."
$CC -O2 -Wall -o xmirror port/xmirror.c -ldl
echo "built ./xmirror (copy it next to launch.sh on the device, chmod 755)"
