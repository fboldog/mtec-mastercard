#!/bin/sh
# Builds MTecTools.adf: xSysInfo-MTec.adf plus MasterInstall, MasterMount and Reboot.
# SCSIBench needs MUI, so its libraries and classes come too. To make room, xSysInfo
# and everything only it uses go: xSysInfo-MTec.adf still has it.
S=$HOME/development/claude-scratch/mtec/sb
I=../InstallDisk3.02
A=${1:-MTecTools.adf}
./mkxsysinfo.sh "$A" >/dev/null || exit 1
xdftool "$A" delete S/pci.db + delete Libs/openpci.library + delete Libs/68040.library + delete Libs/68060.library \
    + delete Libs/identify.library + delete Libs/mmu.library + delete Libs/68020.library + delete Libs/68030.library \
    + delete xSysInfo + delete xSysInfo.info + delete C/xSysInfoLoader \
    + write $S/x/SCSIBench/SCSIBench.020 C/SCSIBench \
    + write $S/mui/m/MUI/Libs/muimaster.library Libs/muimaster.library + makedir Libs/mui \
    + write $HOME/Amiberry/HardDrives/A2000-PicassoII/Libs/commodities.library Libs/commodities.library \
    + write $S/mui/m/MUI/Libs/mui/Gauge.mui Libs/mui/Gauge.mui + write $S/mui/m/MUI/Libs/mui/Scale.mui Libs/mui/Scale.mui \
    + delete S/Startup-Sequence + write disk/startup-tools S/Startup-Sequence \
    + write Reboot C/Reboot + write $I/MasterInstall C/MasterInstall + write $I/MasterMount C/MasterMount \
    + write $I/libs/MasoVision.library Libs/MasoVision.library + write $I/c/Echo C/Echo >/dev/null || exit 1
for l in asl diskfont iffparse info locale mathieeedoubbas mathieeedoubtrans icon; do
    xdftool "$A" write $I/libs/$l.library Libs/$l.library >/dev/null || exit 1
done
xdftool "$A" delete Libs/diskfont.library + write $HOME/Amiberry/HardDrives/A2000-PicassoII/Libs/diskfont.library Libs/diskfont.library >/dev/null || exit 1
for l in muiconfig muigfx muihelp muilocale muilowlevel muipop muirexx muiscreen muispecial; do
    xdftool "$A" write $S/mui/m/MUI/Libs/$l.library Libs/$l.library >/dev/null || exit 1
done
for c in muipropg muiwinborder muiwinscroller; do
    xdftool "$A" write $S/mui/m/MUI/Libs/$c.class Libs/$c.class >/dev/null || exit 1
done
for c in Scrollgroup Virtgroup Numericbutton Popasl; do
    xdftool "$A" write $S/mui/m/MUI/Libs/mui/$c.mui Libs/mui/$c.mui >/dev/null || exit 1
done
xdftool "$A" makedir Locale + makedir Locale/Catalogs + makedir Locale/Catalogs/english >/dev/null || exit 1
for c in $I/Locale/Catalogs/english/*.catalog; do
    xdftool "$A" write "$c" Locale/Catalogs/english/$(basename "$c") >/dev/null || exit 1
done
xdftool "$A" info | tail -3
