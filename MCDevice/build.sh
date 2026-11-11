#!/bin/sh
# Builds mastercard.device with the Bebbo toolchain and the NDK 3.2 headers.
cd "$(dirname "$0")" || exit 1
N=${NDK:-$HOME/amiga/AmigaOS/AmigaOS3.2/NDK3.2}
# These only tune CPU-side waiting. They do not change the CF94's 40 MHz
# clock. Empty optional values leave esp.c's defaults in effect.
: "${MC_CPU_MHZ:=42}"
: "${MC_FAST_POLLS:=}"
: "${MC_IRQ_ROUNDS:=}"
: "${MC_COOP_US_PER_64K:=}"
# DOCKER=image runs this same script in a toolchain container, for example
#   DOCKER=stefanreinauer/amiga-gcc:gcc-v16.2 ./build.sh
if [ -n "$DOCKER" ]; then
    R=$(cd .. && pwd)
    exec docker run --rm -u "$(id -u):$(id -g)" -v "$R:/w" -v "$N:/ndk:ro" -e NDK=/ndk \
        -e MC_CPU_MHZ="$MC_CPU_MHZ" -e MC_FAST_POLLS="$MC_FAST_POLLS" \
        -e MC_IRQ_ROUNDS="$MC_IRQ_ROUNDS" -e MC_COOP_US_PER_64K="$MC_COOP_US_PER_64K" \
        -e IN_DOCKER=1 -e ROM_GCC= -w /w/MCDevice "$DOCKER" ./build.sh
fi
# The ROM (boot code, driver and menu) is built with GCC 16.2 from a container;
# the disk drivers keep the local toolchain their hardware runs validated.
# ROM_GCC= (empty) builds the ROM locally too.
: "${ROM_GCC=stefanreinauer/amiga-gcc:gcc-v16.2}"
romas() {
    if [ -n "$ROM_GCC" ]; then
        docker run --rm -u "$(id -u):$(id -g)" -v "$(cd .. && pwd):/w" \
            -w /w/MCDevice "$ROM_GCC" m68k-amigaos-as "$@"
    else
        m68k-amigaos-as "$@"
    fi
}
romcc() {
    if [ -n "$ROM_GCC" ]; then
        docker run --rm -u "$(id -u):$(id -g)" -v "$(cd .. && pwd):/w" -v "$N:/ndk:ro" \
            -w /w/MCDevice "$ROM_GCC" m68k-amigaos-gcc -I/ndk/Include_H "$@"
    else
        m68k-amigaos-gcc -I"$N/Include_H" "$@"
    fi
}
PATH=/opt/amiga/bin:$PATH
mkdir -p gen
python3 ../MTecPatch/gen/genstubs.py "$N" \
    exec=SysBase:FindTask,Forbid,Permit,Disable,Enable,AllocMem,FreeMem,AllocSignal,Signal,Wait,SetSignal,WaitPort,GetMsg,PutMsg,ReplyMsg,AddTask,AddTail,AddHead,Remove,Cause,FindName,OpenLibrary,CloseLibrary,OpenDevice,DoIO,SendIO,CheckIO,WaitIO,AbortIO,AddIntServer,OpenResource,CacheClearU \
    expansion=ExpansionBase:FindConfigDev,MakeDosNode,AddBootNode > gen/stubs.h || exit 1
python3 ../MTecPatch/gen/genstubs.py "$N" \
    exec=SysBase:Wait,OpenLibrary,CloseLibrary,FindResident,InitResident \
    intuition=IntuitionBase:OpenScreenTagList,CloseScreen,OpenWindowTagList,CloseWindow,GetScreenDrawInfo,FreeScreenDrawInfo \
    graphics=GfxBase:SetAPen,Move,Draw,Text,TextLength \
    gadtools=GadToolsBase:GetVisualInfoA,FreeVisualInfo,CreateContext,CreateGadgetA,FreeGadgets,GT_GetIMsg,GT_ReplyIMsg,GT_RefreshWindow,GT_BeginRefresh,GT_EndRefresh,DrawBevelBoxA \
    > gen/bootstubs.h || exit 1
python3 ../MTecPatch/gen/genstubs.py "$N" \
    dos=DOSBase:MakeDosEntry,AddDosEntry,RemDosEntry,FreeDosEntry,AttemptLockDosList,UnLockDosList > gen/cdfsstubs.h || exit 1
# NewList is a macro in amiga.lib, which isn't linked.
# MC_CPU_MHZ scales the default short task-context interrupt poll. The two
# explicit values are useful for reproducible tuning builds.
TUNE_DEFS="-DMC_CPU_MHZ=$MC_CPU_MHZ"
if [ -n "$MC_FAST_POLLS" ]; then
    TUNE_DEFS="$TUNE_DEFS -DMC_FAST_POLLS=$MC_FAST_POLLS"
fi
if [ -n "$MC_IRQ_ROUNDS" ]; then
    TUNE_DEFS="$TUNE_DEFS -DMC_IRQ_ROUNDS=$MC_IRQ_ROUNDS"
fi
if [ -n "$MC_COOP_US_PER_64K" ]; then
    TUNE_DEFS="$TUNE_DEFS -DMC_COOP_US_PER_64K=$MC_COOP_US_PER_64K"
fi
# Four disk variants, all named mastercard.device inside. Test variants retain
# the private SDTR controls; production and ROM builds compile them out.
#   mastercard.device          polled diagnostic comparison
#   mastercard-irq.device      normal interrupt-driven diagnostic driver
#   mastercard-sync-irq.device opt-in offset-8 auto-negotiation experiment
#   mastercard-prod-irq.device stripped production-equivalent disk driver
for V in "mastercard.device:-DMC_SYNC_DIAG=1" \
         "mastercard-irq.device:-DIRQXFER -DMC_SYNC_DIAG=1" \
         "mastercard-sync-irq.device:-DIRQXFER -DMC_SYNC_DIAG=1 -DMC_AUTO_SYNC=1" \
         "mastercard-prod-irq.device:-DIRQXFER"; do
    m68k-amigaos-gcc -Os -m68020 -noixemul -nostdlib -nostartfiles -fno-builtin \
        -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-pointer-sign -Wno-array-bounds \
        -s $TUNE_DEFS ${V#*:} -I. -I../MTecPatch/gen -I"$N/Include_H" -o ${V%%:*} dev.c esp.c || exit 1
done
ls -l mastercard.device mastercard-irq.device mastercard-sync-irq.device mastercard-prod-irq.device
# The boot ROM, in both variants: rom/MCROM[-irq]-16k.bin for the emulator,
# rom/MCROM[-irq]-27C512.bin for the EPROM. Only our own code is in them.
# The DiagArea (boot code) with the same toolchain; BLOBX = 2 * pack.py's BLOB.
romas -m68020 --defsym BLOBX=1536 -o rom/diag.o rom/diag.s || exit 1
# The ROM's compressor, salvador (ZX0), from the submodule; built here.
if [ ! -x gen/salvador ] || [ -n "$(find ../external/salvador/src -newer gen/salvador -name '*.c' | head -1)" ]; then
    [ -f ../external/salvador/src/salvador.c ] || { echo "external/salvador is missing: git submodule update --init"; exit 1; }
    make -s -C ../external/salvador CC="${HOSTCC:-cc}" OBJDIR="$PWD/gen/salvador-obj" APP="$PWD/gen/salvador" >/dev/null || exit 1
fi
for V in ":" "-irq:-DIRQXFER"; do
    romcc -Os -m68020 -noixemul -nostdlib -nostartfiles -fno-builtin \
        -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-pointer-sign -Wno-array-bounds \
        -DROM_BUILD -DMC_DIAG_MODES=0 $TUNE_DEFS ${V#*:} -I. -I../MTecPatch/gen -o rom/driver${V%%:*}.hunk dev.c esp.c bootui.c || exit 1
    python3 rom/pack.py rom/driver${V%%:*}.hunk rom/diag.o rom/MCROM${V%%:*}-16k.bin rom/MCROM${V%%:*}-27C512.bin || exit 1
done
python3 ../MTecPatch/gen/genstubs.py "$N" \
    exec=SysBase:FindTask,Forbid,Permit,Disable,Enable,AllocMem,FreeMem,FindName,Remove,AddIntServer,RemIntServer,SetFunction,CacheClearU,OpenLibrary,CloseLibrary,OpenDevice,CloseDevice,DoIO \
    dos=DOSBase:Open,Close,VPrintf,VFPrintf,Flush,ReadArgs,FreeArgs,PrintFault,IoErr,Delay,DateStamp \
    expansion=ExpansionBase:FindConfigDev timer=TimerBase:ReadEClock > gen/tstubs.h || exit 1
m68k-amigaos-gcc -Os -m68020 -noixemul -nostdlib -nostartfiles -fno-builtin \
    -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-pointer-sign -Wno-array-bounds \
    -I. -I../MTecPatch/gen -I"$N/Include_H" -o MCDiag MCDiag.c || exit 1
python3 ../MTecPatch/gen/genstubs.py "$N" \
    exec=SysBase:FindTask,AllocMem,FreeMem,AllocSignal,FreeSignal,SetSignal,Signal,Wait,AddTask,RemTask,OpenLibrary,CloseLibrary,OpenDevice,CloseDevice,DoIO,TypeOfMem \
    dos=DOSBase:Open,Close,VPrintf,VFPrintf,Flush,ReadArgs,FreeArgs,PrintFault,IoErr,Delay \
    timer=TimerBase:ReadEClock > gen/cpustubs.h || exit 1
m68k-amigaos-gcc -Os -m68020 -noixemul -nostdlib -nostartfiles -fno-builtin \
    -fomit-frame-pointer -fno-toplevel-reorder -Wall -Wextra -Wno-array-bounds \
    -I. -I../MTecPatch/gen -I"$N/Include_H" -o MCCpuBench MCCpuBench.c || exit 1
ls -l MCDiag MCCpuBench
