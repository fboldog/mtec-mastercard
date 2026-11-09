# mtec-mastercard

A new SCSI driver and boot ROM for the MasterCard SCSI option of the M-Tec
T1230 (Viper) Amiga 1200 accelerator, which has an NCR 53CF94 chip.
`mastercard.device` replaces M-Tec's `mtecscsi.device` 2.3. It is written from
scratch in C, and the boot ROM contains no M-Tec code, so both can be shared.

The repo also has `MTecPatch`, a patch for M-Tec's own driver, and the tools
used to analyse the card. Everything else, including the hardware findings,
test history and the patched M-Tec ROM, is in [DETAILS.md](DETAILS.md).

## Added features

**The driver, `mastercard.device`:**
- **Large disks:** reads and writes up to 2 TB, with the TD64, NSD and ETD
  64-bit commands. The ETD ones check the medium change count.
- **SCSI commands:** `HD_SCSICMD` with automatic sense data, including 16-byte
  commands.
- **Commands M-Tec's driver lacks:** `TD_GETGEOMETRY`, `TD_GETNUMTRACKS`,
  `TD_EJECT`, `TD_CHANGESTATE`, write protection in `TD_PROTSTATUS` (CD-ROM, WORM
  and write-protected disks), and an NSD device query.
- **Interrupt-driven transfers:** the data moves in the card's interrupt
  server, so audio, the display and serial keep running during transfers. A
  polled variant, slightly faster but with interrupts off during each chunk,
  is also built.
- **Large reads in one go:** reads of 512 KB and more use the chip's 24-bit
  transfer counter instead of restarting every 64 KB.
- **Errors are caught:** parity and illegal-operation errors from the chip end
  the command with an error and reset the bus, instead of passing unnoticed.

**The boot ROM**, for a 27C512 EPROM in the card's socket:
- **Mounting:** reads each disk's RDB, loads the file systems stored in it
  (FFS, PFS3 and so on) with its own loader before DOS starts, and mounts the
  partitions.
- **Booting:** from a partition anywhere on the disk, not only below 2 GB.
  Tested in Amiberry with the boot partition at 2.5-3.5 GB.
- **Start-up:** waits up to 10 s for a drive that is still spinning up, and
  skips empty SCSI IDs after a disk whose RDB has "last disk" set.

**The boot menu:** hold the right mouse button at power-on. It lists the
devices and their partitions, and can skip the card's partitions for one boot.
Both buttons together still open Kickstart's own Early Startup menu.

![The boot menu, in Amiberry](images/bootmenu.png)

## Fixed bugs

Compared with M-Tec's `mtecscsi.device` 2.3:

| Problem in M-Tec's driver | In `mastercard.device` |
|---|---|
| Reads and writes past 2 GB fail: the 32-bit byte offset is treated as signed | Work up to 2 TB |
| Normal I/O can't reach past 4 GB: no TD64, NSD or ETD commands | All three supported |
| devtest finds only 4 GB of a 6 GB disk: its read-to-capacity search stops at sector 8388607 | The full capacity |
| `TD_GETGEOMETRY`, `TD_GETNUMTRACKS`, `TD_EJECT`, `ETD_` and NSD commands are missing | Implemented |
| `TD_REMOVE` with NULL stores it and later calls `Cause(NULL)` | Removes the handler |
| `HD_SCSICMD` without autosense returns the raw SCSI status in `io_Error` | `HFERR_BadStatus` (45) |
| A selection timeout leaves 44 in `scsi_Status` | `scsi_Status` is 0 |
| Chip error bits are cleared and ignored; in the sync tests this most likely let a FIFO overrun damage memory | Errors end the command and reset the bus |

`MTecPatch` 1.3 fixes the same command bugs for M-Tec's own driver, from RAM
or built into its ROM; see [DETAILS.md](DETAILS.md).

## Performance

Amiga 1200, T1230 at 42 MHz, Kickstart 3.0, BlueSCSI Pico with TurboMax.
devtest's read speeds, in KB/s:

| Read size | M-Tec 2.3 | `mastercard.device` from the ROM |
|---|---:|---:|
| 512 KB | 6789 | **7018** |
| 128 KB | **6504** | 6412 |
| 32 KB | **5350** | 5227 |

The board has no real DMA: the CPU moves every byte through its data port, so
transfers take nearly all of the CPU. How much CPU time is left for programs
during continuous 512 KB reads, measured by `MCCpuBench`:

| Driver | Read speed | CPU left for programs |
|---|---:|---:|
| M-Tec 2.3 | 6127 KB/s | 8.0% |
| `mastercard.device`, full speed (default) | 6538 KB/s | 0.0% |
| `mastercard.device`, cooperative (`MC_COOP_US_PER_64K=500`) | 6209 KB/s | 4.6% |

During transfers the vertical blank interrupt keeps its normal 20.0 ms rhythm
with the interrupt-driven driver; the polled variant delays it to 22-34 ms.
The measurements and the tuning behind them are in
[`MCDevice/performance.md`](MCDevice/performance.md).

## Building and using

You need the Bebbo `m68k-amigaos-gcc` in `/opt/amiga/bin`, the Amiga NDK 3.2,
Python 3, and Docker for the ROM, which is built with GCC 16.2 from the
`stefanreinauer/amiga-gcc:gcc-v16.2` image.

```sh
cd MCDevice && ./build.sh
```

- **EPROM:** program `MCDevice/rom/MCROM-irq-27C512.bin` into a 27C512 for
  the card's socket. Keep the original EPROM.
- **Without an EPROM:** `mastercard-prod-irq.device` can be loaded from
  `DEVS:` as `mastercard.device`, on a card whose ROM driver isn't running.
- **Test disks:** `mkadf.sh` builds them; see [DETAILS.md](DETAILS.md).

Copyrighted files are not in the repo and must not be added: M-Tec's disks and
ROM, FFS, MUI, OS libraries, reference manuals and every `.adf`. See
`.gitignore`.
