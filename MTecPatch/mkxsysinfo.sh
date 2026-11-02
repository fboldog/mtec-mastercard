#!/bin/sh
# Builds xSysInfo-MTec.adf from xSysInfo 0.12.0: loads MTecPatch and mounts the
# RDB partitions before xSysInfo starts. $1 output.
I=../InstallDisk3.02; S=$HOME/development/claude-scratch/mtec
A=${1:-xSysInfo-MTec.adf}
cp "$HOME/Downloads/xsysinfo-0.12.0.adf" "$A" || exit 1
chmod u+w "$A"
xdftool "$A" delete S/Startup-Sequence + write disk/startup-xsysinfo S/Startup-Sequence \
    + makedir L + write MTecPatch C/MTecPatch + write MTecTest C/MTecTest \
    + write $S/pt/fs20.bin L/pfs3aio + write $S/pt/ffs46.bin L/FFS46 >/dev/null || exit 1
for c in Assign Execute Info Mount; do
    xdftool "$A" write $I/c/$c C/$c >/dev/null || exit 1
done
xdftool "$A" info | tail -3
