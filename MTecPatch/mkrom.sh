#!/bin/sh
# Builds the boot ROM with MTecPatch inside, from the original ROM.
# $1 original ROM, 16 KB image or the 64 KB chip read (default: the chip read).
# Writes MTecPatchROM-16k.bin for the emulator and MTecPatchROM-27C512.bin for the chip.
N=${NDK:-$HOME/amiga/AmigaOS/AmigaOS3.2/NDK3.2}
PATH=/opt/amiga/bin:$PATH
O=${1:-../mastercard-27C512-eprom.rom}
python3 gen/genstubs.py "$N" > gen/stubs.h || exit 1
# ORIG_INIT: the blob sits at driver offset $23E0, the driver's init at $40.
m68k-amigaos-gcc -Os -m68020 -mpcrel -noixemul -nostdlib -nostartfiles -fno-builtin \
    -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-pointer-sign -Wno-array-bounds \
    -DROM_BUILD -DORIG_INIT=0x23a0 -Igen -I"$N/Include_H" -I. -o MTecPatchROM.hunk MTecPatch.c || exit 1
python3 mkrom.py "$O" MTecPatchROM.hunk MTecPatchROM-16k.bin MTecPatchROM-27C512.bin
