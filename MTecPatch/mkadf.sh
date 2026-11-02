#!/bin/sh
# Builds the bootable MTecTest.adf. FFS46 and pfs3aio are copied from the user's own files.
# $1 output, $2 optional startup script.
I=../InstallDisk3.02; S=$HOME/development/claude-scratch/mtec
A=${1:-MTecTest.adf}; ST=${2:-disk/startup-sequence}
rm -f "$A"
xdftool "$A" format MTECTEST ffs + boot install + makedir C + makedir S + makedir L \
    + write "$ST" S/Startup-Sequence \
    + write MTecPatch C/MTecPatch + write MTecTest C/MTecTest + write CmdTest C/CmdTest + write ../MTecDump/More C/More \
    + write $S/dt/devtest_1.9/devtest C/devtest \
    + write $S/pt/fs20.bin L/pfs3aio + write $S/pt/ffs46.bin L/FFS46 >/dev/null || exit 1
for c in Assign Copy Date Delete Dir DiskChange Echo EndIf Execute If Info Mount Quit Stack Type Version Wait Why; do
    xdftool "$A" write $I/c/$c C/$c >/dev/null || exit 1
done
xdftool "$A" info | tail -3
