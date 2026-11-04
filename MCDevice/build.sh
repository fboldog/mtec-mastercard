#!/bin/sh
# Builds mastercard.device with the Bebbo toolchain and the NDK 3.2 headers.
cd "$(dirname "$0")" || exit 1
N=${NDK:-$HOME/amiga/AmigaOS/AmigaOS3.2/NDK3.2}
PATH=/opt/amiga/bin:$PATH
mkdir -p gen
python3 ../MTecPatch/gen/genstubs.py "$N" \
    exec=SysBase:FindTask,Forbid,Permit,Disable,Enable,AllocMem,FreeMem,AllocSignal,Signal,Wait,SetSignal,WaitPort,GetMsg,PutMsg,ReplyMsg,AddTask,AddTail,Remove,Cause,FindName,OpenLibrary,CloseLibrary,OpenDevice,DoIO,SendIO,CheckIO,WaitIO,AbortIO,AddIntServer \
    expansion=ExpansionBase:FindConfigDev > gen/stubs.h || exit 1
# NewList is a macro in amiga.lib, which isn't linked.
m68k-amigaos-gcc -Os -m68020 -noixemul -nostdlib -nostartfiles -fno-builtin \
    -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-pointer-sign -Wno-array-bounds \
    -I. -I../MTecPatch/gen -I"$N/Include_H" -o mastercard.device dev.c esp.c || exit 1
ls -l mastercard.device
python3 ../MTecPatch/gen/genstubs.py "$N" \
    exec=SysBase:FindTask,Forbid,Permit,Disable,Enable,AllocMem,FreeMem,FindName,Remove,RemIntServer,SetFunction,CacheClearU,OpenLibrary,CloseLibrary,OpenDevice,CloseDevice,DoIO \
    dos=DOSBase:Open,Close,VPrintf,VFPrintf,Flush,ReadArgs,FreeArgs,PrintFault,IoErr,Delay \
    expansion=ExpansionBase:FindConfigDev timer=TimerBase:ReadEClock > gen/tstubs.h || exit 1
m68k-amigaos-gcc -Os -m68020 -noixemul -nostdlib -nostartfiles -fno-builtin \
    -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-pointer-sign -Wno-array-bounds \
    -I. -I../MTecPatch/gen -I"$N/Include_H" -o MCDiag MCDiag.c || exit 1
ls -l MCDiag
