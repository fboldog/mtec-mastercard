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
| `MTecPatch/mkrom.py`, `mkrom.sh` | Builds a boot ROM with the patch inside, by patching the original image |
| `MCDevice/` | `mastercard.device`, a new driver written from scratch in C. Work in progress |
| `MTecROM/` | Builds the same boot ROM from source: turns the ROM into assembler source, then assembles it with the patch |
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
original driver, and everything on it must sit below 2 GB. The ROM version below
removes that limit.

### Patch in ROM

`mkrom.sh` builds the same `MTecPatch.c` with `ROM_BUILD` and puts it into a copy of
the card's ROM. The driver then reports itself as `mtecscsi.device` 2.40.

- **Placement:** the patch goes right after the driver, at image `$25E0`. It is
  3120 bytes, which leaves 3568 bytes of the ROM free.
- **Loading:** the loader's copy length grows from `$23E0` to `$3010` bytes. The
  device's init routine becomes the patch's `rom_entry`, which runs the original
  init and then installs the patch. Six `DiagArea` bytes change; the autoconfig
  data and the driver stay as they were.
- **No relocation:** the blob is built PC-relative and keeps its variables in the
  code. `mkrom.py` refuses a blob with relocations, and an original ROM that
  differs from the one it was written for.
- **RAM version:** the RAM `MTecPatch` sees the ROM patch and stays out, so the
  test floppies still work. `MTecPatch STATUS` shows the ROM patch's counters.
- **Driver's own requests:** its probes use `HD_SCSICMD` without autosense, so they
  now get `HFERR_BadStatus` instead of the raw status. The driver only checks those
  for zero, non-zero and 44, and the patch keeps 44, so its behaviour is unchanged.
- **Output:** `MTecPatchROM-16k.bin` for Amiberry, and `MTecPatchROM-27C512.bin` to
  program, with the image in all four quarters (see [ROM layout](#rom-layout)). Neither goes in git,
  because both contain M-Tec's driver.

Results in Amiberry, with Kickstart 3.1:

| Test | Original ROM | Patched ROM |
|---|---|---|
| Boot from an FFS partition at 2.5-3.5 GB | "Not a DOS disk in device DH0" | Boots, 548 reads above 2 GB, file written there matches on the host |
| `MTecTest.adf` full run | Passes with the RAM patch | Passes: read test, file tests on both partitions, `CmdTest` |

Not tested yet: real hardware, and Kickstart 3.0.

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
| 1.3 ROM | 2.40 | The same code built into the boot ROM |

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
- **The ROM version,** which has only run in Amiberry.

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

### ROM layout

The ROM is a socketed TI TMS27C512-10 EPROM, 64 KB. Read in a programmer, it
holds the same 16 KB image twice, at `$0000` and `$8000`. `$4000`-`$7FFF` and
`$C000`-`$FFFF` are all zero. The card shows the 16 KB at `$0000` on even
addresses of `$E90000`-`$E97FFF`.

On the board, socket pins 1 (`A15`) and 27 (`A14`) are tied to ground (0.8 Ω to
pin 14), so the card only ever reads the first 16 KB of the chip. The rest of a
27C512 can't be reached without cutting traces. The programmer image still holds
the patched image in all four quarters, which costs nothing and also suits a
chip that ends up in a socket wired differently.

Jumpers: JP1 and JP2 next to the socket switch the memory and the card on and off.
The unlabelled 3-pin jumper below the EPROM is for the LED and nothing else.

Offsets below are into the 16 KB image:

| Offset | Contents |
|---|---|
| `$0000`-`$003F` | Autoconfig data, so a bad image can hide the SCSI board |
| `$0040`-`$01FF` | `DiagArea`, nibble-wide: each byte is the high nibbles of two image bytes |
| `$0200`-`$25DF` | Driver, byte-wide, copied to RAM as is |
| `$25E0`-`$3FFF` | Zero, 6688 bytes free |

The decoded `DiagArea` is `$DE` bytes, flags `$10` (nibble-wide, config time).

- **`DiagPoint` (`$46`):** `AllocMem(9184)`, then copies 9184 bytes (`$23E0`) of the
  driver from board `+$400` with `movep.l`. It then fixes up a `RomTag` at `$B4`
  in the RAM copy of the `DiagArea`.
- **`RomTag`:** `RTF_COLDSTART|RTF_AUTOINIT`, version 2, `NT_DEVICE`, priority 1.
  Name, ID string and the autoinit table (data size, function table, data table,
  init routine) are offsets into the copied driver.
- **`BootPoint` (`$40`):** jumps to the start of the copied driver, which is its
  boot code.
- **Space:** the copy length is a single constant, and there is no checksum. The
  driver can grow to `$3E00` bytes, which leaves 6688 bytes for new code. That code
  is copied without relocation, so it must be position-independent.

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

### New driver

`MCDevice/` is a new driver, `mastercard.device`, written from scratch in C. It
uses M-Tec's code only as a description of the hardware (see `MCDevice/mc.h`):
- **Chip:** the 53CF94 sits at board `$C000`, register n at `+n*8`. Its interrupt
  goes to INT2.
- **Data phases:** the chip runs a DMA Transfer Information. The CPU moves 16 bytes
  through the data port at `$F000` each time `$C039` bit 7 is set, then words while
  `$D039` bit 7 is set. The polled build does this with interrupts off; the
  recommended IRQ build does it from the card's INT2 server, so higher-priority
  interrupts remain available.

Current performance findings and the 42 MHz tuning plan are in
[`MCDevice/performance.md`](MCDevice/performance.md).

It supports, without a patch:
- `HD_SCSICMD` with autosense, including 16-byte CDBs sent in a separate
  command phase because IDENTIFY plus 16 bytes cannot fit in the CF94 FIFO
- TD64, NSD and ETD commands, the ETD ones checking the change count
- reads and writes up to 2 TB
- `TD_GETGEOMETRY`, `TD_EJECT`, `TD_CHANGESTATE` and NSD device query

So far it loads from disk only. It refuses to start while M-Tec's ROM driver is
running, because both would drive the chip.

Results in Amiberry, on a card whose ROM has the DiagArea flag cleared:
- **devtest:** probe, geometry and every packet test pass, including the ETD
  change-count checks.
- **Integrity:** 64 × 64 KB, and 16 × 512 bytes from a buffer that isn't
  word-aligned, all pass.
- **RawTest** (built with `-DDEV_NAME`) on a 6 GB disk: 0 failures across 2 GB and
  4 GB, and up to 5 GB. A check on the host found every pattern at its exact byte
  offset.

#### DMA test disk for the real card

`MCDevice/mkadf.sh` builds `MCTest.adf`: `mastercard.device`, `MCDiag`, devtest
and a startup script. It runs with the original EPROM still in.

`MCDiag TAKEOVER` first takes the card from the M-Tec driver:
1. It freezes the driver's process while it waits. The driver otherwise polls
   the units every 0.5 s.
2. It removes the driver's interrupt server.
3. It makes the driver's BeginIO fail every request and renames it.

The hard disk can't be used again until a reboot.

Then, for each unit, every test runs in each transfer mode:

| Mode | How the data moves |
|---|---|
| `DMA16` | 16 bytes for each `$C039` flag, then words. The driver's normal mode, and the reference |
| `DMA8` | 8 bytes for each `$C839` flag, then words |
| `WORD` | one word for each `$D039` flag |
| `PIO` | the chip's FIFO, without the board's DMA logic |

The timed speed section also runs transfer-loop comparisons:

| Mode | Experiment |
|---|---|
| `DMA32` | moves 32 bytes per `$C039`; faster, but the flag only guarantees 16 |
| `MOVEM` | uses `movem.l`; retained as the known-slower comparison |
| `ROM16` | matches M-Tec's four-`move.l` loop and trusts the CF94 transfer counter instead of checking a software buffer bound |
| `EXT24` | enables the CF94 24-bit counter and compares single 128, 256 and 512 KiB phases with ordinary 64 KiB phases; validated 512 KiB phases are also used automatically for normal reads of at least 512 KiB |

`ROM16` is read-only and diagnostic because a faulty stuck DREQ would have no
software buffer bound. `EXT24` follows NCR's required DMA-NOP/count-load
sequence, restores the normal configuration after each command and caps each
phase at 512 KiB. MCDiag allocates about 1.1 MiB for the large comparisons and
reports a clean skip if that memory is unavailable.

The tests:
- **INQUIRY, REQUEST SENSE and MODE SENSE**, with odd and even lengths.
- **Reads:** of 1, 8 and 128 blocks, of 64 KB + 1 block (two DMA chunks), of the
  last block, and into buffers 1, 2 and 3 bytes off a word boundary.
- **WRITE BUFFER / READ BUFFER:** every write mode against every read mode. This
  checks both directions against a known pattern and only touches the drive's
  own buffer.

Each line shows what the data port did:
- 16-byte bursts, four-byte longword moves, word moves and chip-FIFO bytes
- how many DMA or PIO commands it took
- the longest wait
- the flags and chip registers at the end

The defaults only read the disk. `Execute S:write-test` adds disk writes 300
blocks before the end: each block is saved, written in every mode, read back,
checked, and restored.

`Execute S:sync-test` is a separate, experimental read-only test. It asks the
target for conservative 5 MB/s synchronous transfers at offset 4, compares
16- and 24-bit-count reads through 512 KiB, and resets the SCSI bus back to
asynchronous mode before returning. The validated 512 KiB path reaches about
4672 KiB/s. `Execute S:sync-write-test` is the separate Data-Out test: it saves
one block near the end of the medium asynchronously, tests a synchronous write
and read, then resets to async and restores and verifies the original. Normal
driver operation never enables sync automatically. After both tests pass,
`Execute S:sync-fast-test` performs a read-only negotiated ladder from 5 to
10 MB/s, validating two 512 KiB reads and resetting the bus at every step.

On the real card (A1200, Kickstart 3.0, BlueSCSI Pico, original EPROM with the
takeover), see `MCDevice/logs/`:

| Check | First run | After the fixes |
|---|---|---|
| Reads, all four modes | identical | identical, 74 ok, 0 failed |
| Odd lengths (INQUIRY 37, 255) | hung until the time-out | pass in all modes |
| Disk writes of 1, 8 and 129 blocks, all modes | pass, restored | pass, restored, 93 ok, 0 failed |
| Stray board interrupts | none | none |
| devtest read speed, 512 / 128 / 32 KB | 3668 / 3573 / 3031 KB/s | 5918 / 5666 / 4699 KB/s (run 7) |

Read speed over the runs, all on the same BlueSCSI Pico:

| Run | Change | devtest read, 512 / 128 / 32 KB |
|---|---|---|
| 1 | first version, C data loop | 3668 / 3573 / 3031 KB/s |
| 2 | assembly loop for 16-byte bursts | 5377 / 5184 / 4178 KB/s |
| 3 | poll for the chip interrupt before sleeping | 5478 / 5268 / 4444 KB/s |
| 5 | `movem.l` and a `dbpl` loop end: slower, reverted | 5144 / 4943 / 4220 KB/s |
| 7 | test the flag 8 times before the chip status | 5918 / 5666 / 4699 KB/s |
| 14 | IRQ loop cleanup, fixed 16-byte CDBs, extended count for 512 KiB reads | 5976 / 5613 / 4619 KB/s |
| - | M-Tec's own driver | 6024 / 5796 / 4964 KB/s |

What the real board showed:
- **`$C839` works:** the 8-byte flag is real hardware, not only an M-Tec setting.
- **The flags:** they read `$39` at rest and `$B9` when set, so only bit 7 counts.
- **Odd lengths:** the board moves whole words, so the last byte of an odd length
  takes one more word access, of which only the high byte counts.
- **Speed:** reads are CPU-bound. M-Tec's driver still reads at about 6 MB/s.
- **BlueSCSI:** it takes WRITE BUFFER, but doesn't give the data back, so the
  buffer tests skip on it.

In Amiberry, with the original ROM and the takeover, DMA16 and WORD pass every
read and write test. The emulator models neither `$C839` nor the chip's PIO
transfers, so `NODMA8` and `NOPIO` leave those modes out there.

#### Two variants

`MCDevice/build.sh` builds the same driver twice. Both are named
`mastercard.device` inside, so you install one of them as `DEVS:mastercard.device`.

| File | How data moves | Trade-off |
|---|---|---|
| `mastercard.device` | the driver task copies each 64 KB chunk with all interrupts off | slightly faster, but nothing else runs during a chunk: about 12 ms each on the real card |
| `mastercard-irq.device` (`-DIRQXFER`) | the INT2 server copies the data whenever the board raises INT2 for it, as M-Tec's driver does | audio, the vertical blank and serial keep running during transfers |

MCDiag measures the difference: its SPEED lines give the longest gap between
vertical blank interrupts during the reads (20 ms on PAL when nothing delays
them).

On the real card (runs 7-9, same BlueSCSI):

| | Polled | Interrupt |
|---|---|---|
| devtest read, 512 / 128 / 32 KB | 5918 / 5666 / 4699 KB/s | 5892 / 5620 / 4676 KB/s |
| MCDiag, 16 reads of 64 KB | 3724 KB/s | 3829 KB/s |
| Longest vertical blank gap during reads | 22-34 ms in the polled modes | 20.06 ms, as when idle (20.04) |
| Server entries / timer sleeps per command | - | 2 / 0 |

The interrupt build is the recommended one: the same speed within 1%, and
audio, serial and the display keep running during transfers.

`devtest`'s CPU percentage is not a count of CPU cycles spent by the driver.
It times how long a priority -10 spin task remains scheduled, using that task's
`tc_Launch` and `tc_Switch` hooks. Hardware interrupts do not switch tasks, so
time spent copying data inside an interrupt server is counted as idle whenever
the spin task was current when the interrupt arrived. Conversely, MCDevice can
show 100% when its own task is current as the immediate data interrupt arrives.
This explains why M-Tec's driver reports 13% despite executing essentially the
same CPU copy loop. The percentage is useful for task availability, but cannot
compare the true copy cost of two interrupt-driven drivers.

`MCDevice/MCCpuBench.c` measures this independently: a low-priority task runs
calibrated register-only work during an idle interval and during continuous
disk reads. Hardware interrupt time necessarily reduces completed work, without
depending on Exec task-switch hooks. The v29 CPU-availability test disk runs it
against the original and production drivers in one boot.

On the real card under continuous 512 KiB reads, the original driver left 8.0%
of calibrated low-priority computation available at 6127 KiB/s. MCDevice left
0.0% at 6538 KiB/s. Thus the original is somewhat more task-friendly, but its
1-19% `devtest` load was not genuine CPU use: about 92% was unavailable. See
[`performance.md`](MCDevice/performance.md) for the method and full logs.

devtest's 8 KB latency on the BlueSCSI reads 2.09 ms polled and 3.02 ms with
interrupts, but MCDiag's per-command times differ by only tens of
microseconds. All of devtest's latencies on this drive sit near whole
milliseconds (1.02, 2.09, 3.01), which suggests the drive works in about 1 ms
steps. A small timing change can then cost a full step. A spinning disk would
confirm it.

#### Boot ROM

`MCDevice/build.sh` also builds boot ROMs with nothing from M-Tec in them:
- `rom/MCROM-16k.bin` (polled) and `rom/MCROM-irq-16k.bin` (interrupt-driven), for
  the emulator;
- `rom/MCROM-27C512.bin` and `rom/MCROM-irq-27C512.bin`, the same image in all four
  quarters, for the EPROM.

| Image | Contents |
|---|---|
| `$0000`-`$003F` | Autoconfig data: the card's own manufacturer and product numbers, so the driver finds its board |
| `$0040`-`$01FF` | Our DiagArea (`rom/diag.s`), nibble-wide |
| `$0200`- | The driver, byte-wide, with a relocation table (`rom/pack.py`) |

At boot:
1. **Autoconfig:** expansion.library runs the DiagArea's DiagPoint.
2. **Loading the driver:** DiagPoint copies the driver into RAM, relocates it, and
   fills in the RomTag in the DiagArea from the driver's own, adding
   `RTF_COLDSTART`.
3. **Starting the driver:** exec starts it with the other residents, after
   timer.device and before dos.library.
4. **Mounting:** the driver resets the bus and waits the 250 ms the SCSI standard
   allows. After a warm reboot, a drive can otherwise still hang in the transfer
   that was cut off; M-Tec's ROM resets the bus too. Then it looks at targets 0-6,
   LUN 0. A disk that isn't ready yet is started and given up to 10 s. On each
   disk with 512-byte blocks it reads the RDB, and:
   - loads each file system stored in the RDB with its own hunk loader, unless
     FileSystem.resource already has that version or a newer one, since
     dos.library isn't running yet;
   - adds a boot node for each partition: bootable ones with their boot
     priority, the others with -128, so they mount but don't boot;
   - stops the scan if the RDB has "last disk" set (`RDBFF_LAST` or
     `RDBFF_LASTTID`), so empty higher IDs cost no time: 250 ms each otherwise.
     amitools' `rdbtool` sets these flags on every RDB it makes, so clear them
     on a disk that has others after it.
5. **Booting:** BootPoint starts dos.library, which boots from the highest
   boot priority.

**Boot menu:** holding the right mouse button at power-on opens it; both
buttons stay Kickstart's own Early Startup menu. The driver first finds the
devices and reads their RDBs, then shows a table like CSA's: each device (SCSI
ID, vendor and product, type, size), followed by its partitions (drive name,
file system type such as `DOS\3` or `PFS\3`, boot priority, size), with `*` or
`-` in the Mount and Boot columns. Up to 16 partitions are listed. **Boot**
(Return) continues; **Skip card this boot** (Esc) leaves the card's partitions
unmounted for this boot only.

The menu uses the system's own look: a screen like Workbench's, with the
default 3D pens and the system font, and a GadTools window. Each table cell is
drawn at its own column, so the columns line up in any font, and the padding
comes from the font's baseline. The title bar is Intuition's own and matches a
Shell window's pixel for pixel. gadtools.library is in the Kickstart ROM, but
ramlib only starts it once DOS runs, so the menu starts it itself with
`FindResident` and `InitResident`. If anything fails to open, the boot just
continues.

The ROM build leaves out MCDiag's private command and its comparison modes
(`MC_DIAG_MODES=0`): the normal driver only uses DMA16 and the automatic
extended count, and an interrupt ROM moves all data in the server. That saved
about 2.3 KB. The relocation table uses 16-bit offsets, saving about 300 bytes
more. `build.sh` compiles the ROM with GCC 16.2 from the
`stefanreinauer/amiga-gcc:gcc-v16.2` container when Docker runs; `ROM_GCC=`
builds it with the local toolchain. With the local GCC 15.2 the interrupt ROM
leaves 230 bytes free and the polled one 324. The disk drivers keep everything
and the local toolchain; the production one is byte-identical to the validated
build.

Tested in Amiberry with Kickstart 3.1, from the ROM alone, no floppy:
- **Booting:** from an FFS partition at 2.5-3.5 GB, with FFS 46 loaded from the
  RDB.
- **A second file system:** a PFS3 partition, with pfs3aio loaded from the RDB,
  mounted read/write.
- **Writing:** a file copied on the Amiga matches on the host.

Both variants pass, and the boot menu was tested with an always-open build:
both disks listed, Return boots normally, Esc mounts nothing. The real card
hasn't run the ROM yet.

Not done yet:
- the boot ROM on the real card
- disks with other block sizes and LUNs above 0 at boot
- disconnection
- detecting medium changes without a command

## Building

You need the Bebbo `m68k-amigaos-gcc` in `/opt/amiga/bin`, the Amiga NDK 3.2 and
Python 3. Set `NDK` if the headers aren't in `~/amiga/AmigaOS/AmigaOS3.2/NDK3.2`.

```sh
cd MTecPatch && ./build.sh
cd ../MTecDump && ./build.sh
```

### ROM image

```sh
cd MTecPatch && ./mkrom.sh ../mastercard-27C512-eprom.rom
```

It takes the 64 KB chip read or the 16 KB dump, and both give the same result.

### ROM image from source

```sh
cd MTecROM && ./build.sh ../mastercard-27C512-eprom.rom
```

This builds the whole ROM from assembler source instead of patching bytes in the
original. That makes it possible to change the driver itself, or add code such as
a boot menu, with labels in place of fixed offsets.

1. `dis2src.py` turns the ROM into `build/src/driver.s` and `build/src/diag.s`.
   It follows the code from the entry points and tables named in `symbols.txt`,
   and checks every instruction by assembling it again. 8626 of the driver's 9184
   bytes come out as instructions, and the rest are strings, tables and padding.
   The source is M-Tec's code, so it is generated from your own ROM into the
   ignored `build/` folder and never goes in git.
2. `build.py` first assembles the source without the patch and checks that it
   gives the original 16 KB image byte for byte, including the autoconfig data and
   the nibble-wide DiagArea.
3. It then assembles `MTecPatch.c`, compiled to assembler, in the same unit right
   after the driver. The patch calls the driver's init by name (`_mtec_init`), and
   the DiagArea takes its copy length and init routine from the symbols.

The result, `build/MTecROM-27C512.bin`, is identical to the one `mkrom.sh` makes
(SHA-1 `9b882e35a12d3d4cbcfe40c6211b04d0475d2b84`), so the emulator results above
apply to it.

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
