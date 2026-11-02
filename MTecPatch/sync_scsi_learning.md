# Synchronous SCSI on the M-Tec T1230 card: what we learned

**Status: dropped on 2026-10-31.** Sync SCSI was slower than async on this card,
gave read errors, and crashed the Amiga. Keep using async with `MTecPatch`.

Hardware: M-Tec T1230 (Viper) with `mtecscsi.device` 2.3, 53CF94 SCSI chip,
Kickstart 3.0 (exec 39.47), and a BlueSCSI Pico on SCSI ID 1 with an SD card of
about 8 GB. All numbers below are from that real hardware unless marked otherwise.

## Why we tried it

Async reads stopped at about 6 MB/s, and the driver never uses the chip's sync
registers. The async bus looked like the likely limit, and sync could run the bus
at up to 10 MB/s. That theory turned out to be wrong.

## What was built

- `MTecSync.c`: negotiates sync, then sets the chip's sync registers.
  `MTecSync RATE 5|6|8|10` or `MTecSync OFF`.
- `MTecTest CRC UNIT n`: checksums 32 areas of 512 KB across the disk. The first
  run is saved in `RAM:`, and later runs are compared with it.
- `MTecSync.adf`, built by `mksync.sh` from `disk/startup-sync`: runs an async
  reference, tries each rate, then returns to async.

No driver code was replaced. One hook was added in the RAM copy of the driver.

### How the tool works

- A sync agreement is state kept by the target, so the driver's normal code can
  carry on unchanged once the chip registers match.
- With interrupts off, the tool polls the chip through one TEST UNIT READY:
  select with ATN and stop, send SDTR, read the reply, send the command, take
  status and disconnect.
- The driver's code base is `ln_Name - $5D8`, checked against known bytes.
- The chip has one set of sync registers, so the tool refuses to run with more
  than one device on the bus.
- A hook at `$21CE` clears the registers when the driver sees a bus reset,
  because BlueSCSI drops the agreement at that point.
- Requested offset is 15. Chip clock is taken as 40 MHz, from the selection
  timeout value `$99` and clock factor register 0.

## Results

Negotiation worked first time. BlueSCSI accepted all four rates with offset 15,
and `MTecSync OFF` returned the card to async.

### First run: all settings at their defaults

Speeds are KB/s, from `devtest -b`. Async used 128 KB and 32 KB reads. After the
first run of errors `devtest` dropped to 64 KB and 16 KB, so the sync columns are
not directly comparable with async.

| Mode | Large reads | Small reads | Errors |
|---|---|---|---|
| Async | 5783 | 4948 | None |
| Sync 5 MB/s | ~1800 | ~1450 | Error 41 on every size, checksum run failed |
| Sync 6.67 MB/s | ~2050 | ~1650 | Error 41 on every size |
| Sync 8 MB/s | ~2150 | ~1700 | Error 41 on every size |
| Sync 10 MB/s | 4025 | 3221 | None in this one run |

Speeds for the failing rates are from runs cut short by errors, so they are rough.

### Runs with MasterInstall settings changed

One setting was changed at a time.

| Change | Result |
|---|---|
| `fast transfer` off | Froze at sync 5 MB/s |
| `disconnect/reselect` off | No freeze. Same error 41 pattern. Async also slower: 5350 / 3895 KB/s |
| `parity bit check` off | Error 41 at 5 MB/s, then crash. Async unchanged: 5811 / 4968 KB/s |
| `asynchronous transfer` changed | Froze. Error 41 at 5 MB/s, no change expected or seen |
| Repeat run to reproduce | Software failure `8100000C`, task `00219180` |
| All settings back to normal, cross-check | Error 41 at 5 MB/s, checksums failed, then froze during `devtest`. Async fine: 6064 / 5822 / 4991 KB/s |

## What the failures mean

- **Error 41** is `HFERR_DMA`. In this driver it is raised at `$20CE` when bytes
  transferred differ from bytes requested.
- **`8100000C`** is the fatal form of `AN_MemoryInsane`: exec's memory list failed
  a sanity check in `AvailMem(MEMF_LARGEST)`. So system memory really was damaged.
- **The pink flash** at the bottom of the screen is the driver writing `$F0F` to
  `COLOR00` at `$1CC0`, when status bit 6 is set. Yellow (`$FF0` at `$1CB6`) is the
  marker for the next bit down. On this chip family those bits are gross error and
  parity error, recalled from memory, not checked against the datasheet.
- **The driver ignores these errors.** It clears them with `andi.w #$1fff,d0` and
  carries on.

### Most likely cause (not proven)

1. In sync mode the device can send 15 bytes ahead without waiting.
2. The CPU empties the chip's 16-byte FIFO through the `$E9F000` data port in
   bursts, and falls behind.
3. The FIFO overflows and the chip raises gross error.
4. The driver ignores it, its byte count goes wrong, and error 41 follows.
5. Later reads may run past a buffer, which damages memory and causes the crash.

Steps 1-3 fit the pink flash. Steps 4-5 fit the logs. No test caught the overrun
itself.

### What this says about speed

Even at a clean 10 MB/s on the bus, reads were 25-35% slower than async. So the
async bus was not the limit. The more likely limit is the data port plus the CPU
copy loop, but this was never measured separately.

## Lessons

- **Failures are random.** Sync finished two full runs, then crashed on later
  runs with similar settings. Single runs can't prove that a setting caused or
  cured a freeze. `fast transfer` off and `disconnect/reselect` off were both
  judged from one run each, so neither result is firm.
- **The cross-check froze with the same settings as the first run,** which had
  finished. The two runs that finished (`MTecSync_tested.adf`,
  `MTecSync_disconnect_test.adf`) used the older build, with logs going straight
  to the floppy. The runs after the logging change all crashed or froze. That
  change doesn't touch the chip, and error 41 appeared in both builds, so it
  probably only moved where the damage lands, for example through different
  memory use or timing. This wasn't tested.
- **A matching checksum doesn't make a rate safe.** Rates 6.67 and 8 MB/s matched
  while `devtest` was logging errors, because retries can hide failures.
- **Keep the log off the floppy during risky steps.** A freeze corrupted the ADF
  while the log was open. Each step now logs to `RAM:` and is copied to the floppy
  between steps, with a pause. The ADF stayed valid through later crashes.
- **Amiberry can't test this.** Its emulated drive skips the message phase, so
  negotiation stops at step 3. It did show that the failure path recovers, and
  its speed figures mean nothing.
- **Watch the screen.** The driver's colour flashes report errors that the log
  never shows.

## Settings

| Option | Driver use | Best setting |
|---|---|---|
| `fast transfer` | Chooses the copy routine (`$187`). Bit polarity not confirmed | On |
| `disconnect/reselect` | Selects with ATN, allows reselection | On |
| `parity bit check` | Sets chip parity for the unit | On |
| `controller DMA` | Never read | Any |
| `asynchronous transfer` | Stored at `$198`, never read | Any |
| MasterMount `Public`, `24Bit DMA`, mask | Filesystem only | Your choice |

## Untested idea

A smaller sync offset, such as 4 or 8, might stop the FIFO overflow. It would prove
the theory, but it can't make the card faster, because sync at offset 15 was
already slower than async. The test would still carry the crash risk.

## Warning

Don't run `MTecSync` with volumes mounted. It can damage memory, and damaged memory
could be written to disk. A power cycle clears it. `MTecSync` runs only if started
by hand, and it isn't on `xSysInfo-MTec.adf`, `MTecTools.adf` or `MTecTest.adf`.

## Logs

- `MTecSync_tested.adf`: first run, default settings
- `MTecSync_disconnect_test.adf`: `disconnect/reselect` off
- `MTecSync_parity_tested.adf`: `parity bit check` off, ended in a crash
- `MTecSync_last_crash.adf`: last crash, with the pink flash seen
- `MTecSync_still_freeze.adf`: cross-check with all settings back to normal, froze
