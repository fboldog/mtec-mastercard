#!/bin/sh
# Builds MCTest.adf, the real-hardware test disk for mastercard.device.
# AmigaDOS commands come from the user's own InstallDisk3.02, devtest from
# the scratch folder (https://github.com/cdhooper/amiga_devtest).
# DRIVER=mastercard-irq.device puts the interrupt-driven variant on the disk.
cd "$(dirname "$0")" || exit 1
I=../InstallDisk3.02; S=$HOME/development/claude-scratch/mtec
A=${1:-MCTest.adf}
STARTUP=${STARTUP:-disk/startup-sequence}
./build.sh >/dev/null || exit 1
rm -f "$A"
xdftool "$A" format MCTEST ffs + boot install + makedir C + makedir S + makedir Devs \
    + write "$STARTUP" S/Startup-Sequence + write disk/write-test S/write-test + write disk/sync-test S/sync-test + write disk/sync-write-test S/sync-write-test + write disk/sync-fast-test S/sync-fast-test + write disk/sync-driver-test S/sync-driver-test + write disk/sync-driver-offset8-test S/sync-driver-offset8-test + write disk/sync-offset-test S/sync-offset-test + write disk/mtec-speed S/mtec-speed \
    + write ${DRIVER:-mastercard.device} Devs/mastercard.device + write MCDiag C/MCDiag \
    + write ../MTecDump/More C/More + write $S/dt/devtest_1.9/devtest C/devtest >/dev/null || exit 1
for c in Echo Execute Wait Type Version Info Dir Assign MakeDir; do
    xdftool "$A" write $I/c/$c C/$c >/dev/null || exit 1
done
xdftool "$A" list
