#!/bin/sh
# Builds MTecDump and More with the Bebbo toolchain and the NDK 3.2 headers.
N=${NDK:-$HOME/amiga/AmigaOS/AmigaOS3.2/NDK3.2}
PATH=/opt/amiga/bin:$PATH
python3 gen/genstubs.py "$N" > gen/stubs.h
for T in MTecDump More; do
    m68k-amigaos-gcc -Os -m68000 -noixemul -nostdlib -nostartfiles -fno-builtin \
        -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-pointer-sign -Wno-array-bounds \
        -Igen -I"$N/Include_H" -I. -o $T $T.c || exit 1
done
