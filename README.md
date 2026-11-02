# mtec-mastercard

Analysis, a RAM patch and test tools for the SCSI side of the M-Tec T1230 (Viper)
Amiga 1200 accelerator: the MasterCard SCSI option, with `mtecscsi.device` 2.3 in
its boot ROM and an NCR 53CF94 chip.

`MTecPatch` adds disks over 2 GB, TD64, NSD and several missing commands. It runs
from RAM and nothing on the card is changed, so a reboot removes it.

**Current version:** `MTecPatch` 1.3. The driver then reports itself as
`mtecscsi.device` 2.32. It has passed the full test disk on real hardware:
[`mtectest-v1.3-real-hardware.log`](MTecPatch/logs/mtectest-v1.3-real-hardware.log).

## Test hardware

- M-Tec T1230 with 8 MB of RAM, Kickstart 3.0 (exec 39.47)
- ROM 39.106, `mtecscsi.device` 2.3
- BlueSCSI Pico at SCSI ID 1, with an SD card of about 8 GB (15712256 sectors)
- `SDH0`: FFS, 15-3106 MB. `SDH1`: PFS3, 3106-7200 MB, 4096-byte blocks

Results from this one setup may not apply to other cards or disks.

## What is in the repo

| Path | Contents |
|---|---|
| `MTecPatch/MTecPatch.c` | The patch |
| `MTecPatch/CmdTest.c` | Tests the added commands and error codes |
| `MTecPatch/MTecTest.c` | Disk scan, mount helper, read, write and file tests |
| `MTecPatch/RawTest.c` | Raw checks around 2 GB and 4 GB. **Overwrites unit 0** |
| `MTecPatch/MTecSync.c` | Sync SCSI experiment, dropped |
| `MTecPatch/Reboot.c` | Reboots the Amiga |
| `MTecPatch/disk/` | Startup scripts for the floppies |
| `MTecPatch/*.sh` | Program and floppy image builders |
| `MTecPatch/logs/` | Logs from real hardware |
| `MTecPatch/sync_scsi_learning.md` | Why sync SCSI was dropped |
| `MTecDump/` | ROM and RAM dump tool, plus the small `More` pager |

Copyrighted files are not in the repo and must not be added: the M-Tec disks and
ROM dump, FFS, MUI, OS libraries and every `.adf`. See `.gitignore`.

## Using the patch

```
MTecPatch          ; install, lasts until reboot
MTecPatch STATUS   ; show what it has handled
```

Run it before mounting partitions. The Kickstart 3.0 `Mount` command has no
`SectorsPerBlock` keyword, so `MTecTest` does the mounting:

```
MTecPatch
MTecTest SCAN              ; reads the RDB, writes RAM:MountList and RAM:MountAll
Execute RAM:MountAll
```

The patch loads after boot, so a partition you boot from is limited by the
original driver, and everything on it must sit below 2 GB.

## What changed in the driver

### Original driver

The original driver treats the 32-bit byte offset as signed, so `CMD_READ` and
`CMD_WRITE` break at 2 GB. It has no TD64 or NSD, and it lacks some commands that
tools expect.

| Command or error | Original driver | With `MTecPatch` 1.3 |
|---|---|---|
| `CMD_READ`, `CMD_WRITE` over 2 GB | Broken | `READ(10)` and `WRITE(10)` |
| `TD_READ64` family | `IOERR_NOCMD` | Added |
| `NSCMD_TD_READ64` family | `IOERR_NOCMD` | Added |
| `NSCMD_ETD_READ64` family | `IOERR_NOCMD` | Added |
| `NSCMD_DEVICEQUERY` | `IOERR_NOCMD` | Added, lists the supported commands |
| `TD_GETGEOMETRY` | `IOERR_NOCMD` | Added, from `READ CAPACITY` and `INQUIRY` |
| `TD_GETNUMTRACKS` | `IOERR_NOCMD` | Added |
| `TD_EJECT` | `IOERR_NOCMD` | Added, sends `START STOP UNIT` |
| `ETD_` commands | `IOERR_NOCMD` | Added, with the change count check |
| `TD_REMOVE` with NULL | Stores NULL, later calls `Cause(NULL)` | Removes the handler |
| `HD_SCSICMD` bad status, no autosense | Raw status in `io_Error` | `HFERR_BadStatus` (45) |
| `HD_SCSICMD` selection timeout, no autosense | Leaves 44 in `scsi_Status` | `scsi_Status` is 0 |
| `TD_RAWREAD` | `IOERR_NOCMD` | Unchanged |

Geometry is always reported as 255 heads and 63 sectors per track, because a
modern disk has no real geometry. The true size is in `dg_TotalSectors`.

### How it works

- It wraps the driver's `BeginIO`.
- Requests below 2 GB stay on `CMD_READ` and `CMD_WRITE`, so the driver's read
  cache stays correct and its retries still work.
- Requests at or above 2 GB and all 64-bit commands go to a small task, which
  issues `READ(10)` and `WRITE(10)` through `HD_SCSICMD`. That path skips the cache,
  so nothing at or above 2 GB is ever cached.
- Failed transfers on that path are tried up to 3 times.
- `SEEK` commands succeed without doing anything.

### Version history

| Version | Driver shown as | Change |
|---|---|---|
| 1.0 | 2.3 | Over 2 GB, TD64, NSD |
| 1.1 | 2.3 | `TD_GETGEOMETRY`, `TD_GETNUMTRACKS` |
| 1.2 | 2.31 | Patched driver marked with its own version and ID string |
| 1.3 | 2.32 | `TD_EJECT`, `ETD_` commands, `TD_REMOVE` NULL, `HFERR_BadStatus`, expanded NSD list |

### Fixes along the way

- **Deadlock:** the patch's task hung on its own `HD_SCSICMD`. Requests that use the
  task's reply port now go straight to the driver.
- **Test disk:** the prompt printed twice and `c` filled the console while waiting
  for Return. `MTecTest ASK` replaced `Ask`.
- **Mounting:** PFS3 blocks of 4096 bytes need `SectorsPerBlock`, which the old
  `Mount` lacks. `MTecTest SPB` sets it.
- **`SDH1` not mounting:** its handler wasn't started. `MTecTest` now starts it with
  `DeviceProc`.
- **Corrupt logs:** a freeze damaged the floppy while the log was open. The sync
  disk now logs to `RAM:` and copies to the floppy between steps.

## Test results

From the [real hardware log](MTecPatch/logs/mtectest-v1.3-real-hardware.log):

| Test | Result |
|---|---|
| Read test at 1-5 GB: `READ(10)`, TD64, NSD, plus `CMD_READ` up to 3 GB | 0 failures |
| File tests on `SDH0` and `SDH1` | OK |
| `CmdTest` | 0 failures |
| Raw write test at 1-5 GB, original data restored | 0 failures |
| `TD_GETGEOMETRY` | 15712256 x 512, C=978 H=255 S=63 |
| `TD_GETNUMTRACKS` | 249400 |
| Retries | 0 |
| Speed in `devtest` | 6040 / 5799 / 4922 KB/s, same as before the patch |

`Failed requests: 5` appears at the end of the log. The count is still 0 after the
read and file tests, so all five come from the later `CmdTest` and write steps,
where some failures are deliberate. They haven't been traced one by one.

### Not tested on real hardware

- **Selection timeout fix:** the full run prints `No empty unit to test a selection
  timeout`. It passed in Amiberry when `CmdTest` ran alone. The same line appears in
  the emulator's full run, so the cause is probably the earlier steps in that boot.
- **`TD_EJECT` in the eject direction:** only load has been sent. `CmdTest` needs an
  `EJECT` option first, then a SCSI CD-ROM can test it.
- **Real media changes:** `TD_ADDCHANGEINT`, `TD_REMOVE` and the change counter. It
  isn't known whether the driver notices a swap by itself.
- **Media with sectors other than 512 bytes.**
- **Booting** with the patch, because it can only load after boot.

## Findings

### Speed

Reads top out at about 6 MB/s, with or without the patch.

| MasterInstall `controller DMA` | MasterInstall `fast transfer` | xSysInfo |
|---|---|---|
| On | Off | 5.2 MB/s |
| Off | On | 5.3 MB/s |
| On | On | 5.4 MB/s |

- `controller DMA` is stored but never read by the driver, so these differences are
  probably noise.
- `fast transfer` chooses the driver's copy routine.
- MasterMount's `Public` and `24Bit DMA` only set the memory type used by the
  filesystem. They don't change how the driver transfers data.

### DMA

The driver moves data by CPU through the data port at `$E9F000`. The code shows no
DMA use. Whether the card could do DMA is a hardware question, still being checked
by measuring the 53CF94 pins:

| Signal | PLCC-84 pin | QFP-100 pin |
|---|---|---|
| `DREQ` | 72 | 100 |
| `DACK/` | 73 | 1 |
| `DBWR/` | 74 | 2 |
| `A2`/`DBRD/` | 59 | 84 |
| `CS/` | 56 | 81 |
| `RD/` | 55 | 80 |
| `WR/` | 54 | 79 |
| `INT/` | 52 | 76 |

### Sync SCSI

Dropped. Sync negotiated at every rate but was slower than async, gave read errors
and damaged memory. See [`sync_scsi_learning.md`](MTecPatch/sync_scsi_learning.md).

### Driver notes

- ROM board at `$E90000`, with data on even addresses. The driver is stored as a
  hunk file up to `$25D3`, which leaves about 6.7 KB free.
- Chip registers at `$E9C000`, spaced 8 bytes apart. Data port at `$E9F000`.
- The driver is copied to RAM. Its code base is `ln_Name - $5D8`.
- Command table at `$160A`, commands `$00`-`$1C`, dispatched at `$1036`.
- Interrupt dispatch table at `$23C2`.
- RDB `rdb_Flags` low byte: bit 3 no reselect, bit 6 sync (stored, never read).
- `MaCa` block at RDB `+$BC`, options byte at `$C1`.

| Options bit | Meaning |
|---|---|
| 0 | Controller DMA, never read |
| 1 | Fast transfer, chooses the copy routine |
| 2 | Parity |
| 3 | Hard disk write protect |
| 4 | Write cache |
| 5 | RDB write protect |

Addresses are offsets into the driver, from the ROM disassembly.

### Reimplementing the driver

Not worth it today. The patch covers the missing features from RAM, and a new driver
wouldn't be faster unless the hardware turns out to support DMA.

## Building

You need the Bebbo `m68k-amigaos-gcc` in `/opt/amiga/bin`, the Amiga NDK 3.2 and
Python 3. Set `NDK` if the headers aren't in `~/amiga/AmigaOS/AmigaOS3.2/NDK3.2`.

```sh
cd MTecPatch && ./build.sh
cd ../MTecDump && ./build.sh
```

### Floppy images

The floppy builders also need `xdftool` from amitools and files that aren't in the
repo. They use the extracted M-Tec install disk in `InstallDisk3.02/` and files in
`~/development/claude-scratch/mtec`, so they only run on the original machine as
they stand.

| Script | Image | Purpose |
|---|---|---|
| `MTecPatch/mkadf.sh` | `MTecTest.adf` | Full test run, log saved to the floppy |
| `MTecPatch/mkxsysinfo.sh` | `xSysInfo-MTec.adf` | xSysInfo 0.12.0 with the patch and partitions |
| `MTecPatch/mktools.sh` | `MTecTools.adf` | MasterInstall, MasterMount, SCSIBench, Reboot |
| `MTecPatch/mksync.sh` | `MTecSync.adf` | Sync SCSI test |
| `MTecDump/mkadf.sh` | `MTecDump.adf` | ROM dump |

## Warnings

- `MTecTest.adf`'s write test overwrites 128 KB at 1, 2, 3, 4 and 5 GB and then puts
  the data back. A crash or reset during it damages those areas. It asks first.
- `RawTest` overwrites its test areas on unit 0. Use a scratch disk only.
- `MTecSync` can damage memory. Don't run it with volumes mounted.
- Back up your disk before relying on the patch.
- Don't share the built `.adf` files. They contain copyrighted software.
