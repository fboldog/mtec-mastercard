#!/bin/sh
# Builds mastercard.device with the Bebbo toolchain and the NDK 3.2 headers.
cd "$(dirname "$0")" || exit 1
N=${NDK:-$HOME/amiga/AmigaOS/AmigaOS3.2/NDK3.2}
PATH=/opt/amiga/bin:$PATH
mkdir -p gen
python3 ../MTecPatch/gen/genstubs.py "$N" \
    exec=SysBase:FindTask,Forbid,Permit,Disable,Enable,AllocMem,FreeMem,AllocSignal,Signal,Wait,SetSignal,WaitPort,GetMsg,PutMsg,ReplyMsg,AddTask,AddTail,AddHead,Remove,Cause,FindName,OpenLibrary,CloseLibrary,OpenDevice,DoIO,SendIO,CheckIO,WaitIO,AbortIO,AddIntServer,OpenResource,CacheClearU \
    expansion=ExpansionBase:FindConfigDev,MakeDosNode,AddBootNode > gen/stubs.h || exit 1
# NewList is a macro in amiga.lib, which isn't linked.
# Two variants of the same driver, both named mastercard.device inside:
#   mastercard.device      moves data with interrupts off (polled), a little faster
#   mastercard-irq.device  moves data in the INT2 server (-DIRQXFER), so higher
#                          interrupts (audio, vertical blank, serial) keep running
for V in "mastercard.device:" "mastercard-irq.device:-DIRQXFER"; do
    m68k-amigaos-gcc -Os -m68020 -noixemul -nostdlib -nostartfiles -fno-builtin \
        -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-pointer-sign -Wno-array-bounds \
        ${V#*:} -I. -I../MTecPatch/gen -I"$N/Include_H" -o ${V%%:*} dev.c esp.c || exit 1
done
ls -l mastercard.device mastercard-irq.device
# The boot ROM, in both variants: rom/MCROM[-irq]-16k.bin for the emulator,
# rom/MCROM[-irq]-27C512.bin for the EPROM. Only our own code is in them.
for V in ":" "-irq:-DIRQXFER"; do
    m68k-amigaos-gcc -Os -m68020 -noixemul -nostdlib -nostartfiles -fno-builtin \
        -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-pointer-sign -Wno-array-bounds \
        -DROM_BUILD ${V#*:} -I. -I../MTecPatch/gen -I"$N/Include_H" -o rom/driver${V%%:*}.hunk dev.c esp.c || exit 1
    python3 rom/pack.py rom/driver${V%%:*}.hunk rom/diag.s rom/MCROM${V%%:*}-16k.bin rom/MCROM${V%%:*}-27C512.bin || exit 1
done
python3 ../MTecPatch/gen/genstubs.py "$N" \
    exec=SysBase:FindTask,Forbid,Permit,Disable,Enable,AllocMem,FreeMem,FindName,Remove,AddIntServer,RemIntServer,SetFunction,CacheClearU,OpenLibrary,CloseLibrary,OpenDevice,CloseDevice,DoIO \
    dos=DOSBase:Open,Close,VPrintf,VFPrintf,Flush,ReadArgs,FreeArgs,PrintFault,IoErr,Delay \
    expansion=ExpansionBase:FindConfigDev timer=TimerBase:ReadEClock > gen/tstubs.h || exit 1
m68k-amigaos-gcc -Os -m68020 -noixemul -nostdlib -nostartfiles -fno-builtin \
    -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-pointer-sign -Wno-array-bounds \
    -I. -I../MTecPatch/gen -I"$N/Include_H" -o MCDiag MCDiag.c || exit 1
ls -l MCDiag
