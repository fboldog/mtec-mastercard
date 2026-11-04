#!/bin/sh
# Builds the MasterCard boot ROM from source, with MTecPatch inside.
# $1 original ROM, 16 KB image or the 64 KB chip read (default: the chip read).
# The driver source is generated from it into build/src on the first run.
# Writes build/MTecROM-16k.bin for the emulator and build/MTecROM-27C512.bin for the chip.
cd "$(dirname "$0")" || exit 1
N=${NDK:-$HOME/amiga/AmigaOS/AmigaOS3.2/NDK3.2}
PATH=/opt/amiga/bin:$PATH
O=${1:-../mastercard-27C512-eprom.rom}
P=../MTecPatch
mkdir -p build || exit 1
python3 $P/gen/genstubs.py "$N" > $P/gen/stubs.h || exit 1
m68k-amigaos-gcc -S -Os -m68020 -mpcrel -noixemul -nostdlib -nostartfiles -fno-builtin \
    -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-pointer-sign -Wno-array-bounds \
    -DROM_BUILD -I$P/gen -I"$N/Include_H" -I$P -o build/patch.s $P/MTecPatch.c || exit 1
python3 build.py "$O" build/patch.s build
