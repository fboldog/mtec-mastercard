#!/bin/sh
# Builds the bootable MTecDump.adf. $1 overrides the output file, $2 the startup script.
I=../InstallDisk3.02
A=${1:-MTecDump.adf}
S=${2:-startup-sequence}
rm -f "$A"
xdftool "$A" format MTECDUMP ffs + boot install + makedir C + makedir S \
    + write MTecDump + write "$S" S/Startup-Sequence + write More C/More \
    + write $I/c/Copy C/Copy + write $I/c/Type C/Type + write $I/c/Dir C/Dir \
    + write $I/c/List C/List + write $I/c/Info C/Info + write $I/c/Echo C/Echo
