# MCDevice performance work

This document is about the new `mastercard.device` in `MCDevice/`, not the
RAM/ROM patch for M-Tec's original driver.

## Baseline

The current real-hardware baseline is an Amiga 1200 with a 42 MHz 68030 and a
BlueSCSI Pico. The NCR/AMD 53CF94 clock remains 40 MHz; the CPU clock only
changes CPU-side polling and memory/data-port throughput.

The interrupt-driven build is the normal baseline. It reads within 1% of the
polled build while allowing higher-priority interrupts to run. The polled build
keeps all interrupts disabled for about 12 ms per 64 KiB chunk and is retained
as a comparison.

## Findings and status

| Item | Finding | Status |
|---|---|---|
| CF94 error handling | Gross and parity status bits were recorded but ignored. Parity reporting was disabled. This made sync or aggressive DMA experiments unsafe. | Implemented on this branch, with real-hardware validation pending: parity reporting and abort-on-parity are enabled; parity returns `HFERR_Parity`, gross error returns `HFERR_DMA`, and the bus is reset. |
| Short interrupt poll | `FAST_POLLS=4000` represented about 1 ms at 50 MHz, but was machine-specific. | It is now `80 * MC_CPU_MHZ`, with a 42 MHz default (3360 polls) and build-time overrides. |
| DMA transfer count | The normal path uses 16 bits and restarts the data phase every 64 KiB. AMD documents a 24-bit counter behind `CFG2.ENF`, loaded after DMA NOP. | Implemented as private MCDiag mode `EXT24`, capped at 1 MiB and never selected automatically. It compares 128, 256, 512 KiB and 1 MiB phases with normal DMA16. Real-hardware validation pending. |
| DMA16 | The `$C039` path is correct and fastest among the guaranteed board modes. | Production default. |
| DMA32 | Skipping every second `$C039` check was about 8% faster in MCDiag, but the flag only guarantees 16 bytes. | Diagnostic experiment only; do not auto-select. |
| Original-ROM loop | M-Tec omits MCDevice's software burst bound and trusts the CF94 counter. This removes `subq`/`beq` from each 16 bytes but a stuck DREQ could overwrite memory. | Implemented as read-only MCDiag mode `ROM16`; diagnostic only. Guard bytes and data are checked after timed reads. |
| IRQ loop entry | IRQ code tested `$C039`, then the assembly loop tested it again and saved four unused data registers. The loop also discarded a status value that C reread. | Implemented: ready-entry variants consume the guaranteed 16 bytes immediately, DMA16 saves only the registers it uses, and sampled interrupt status is returned to C. Hardware validation pending. |
| IRQ wait | Small commands normally finish inside the short poll and report zero timer sleeps, so they consume CPU while waiting. | Tune `MC_FAST_POLLS` for throughput versus task responsiveness. |
| Bulk IRQ behavior | A 64 KiB read normally takes two server entries and zero timer sleeps. M-Tec's disassembled handler uses the same four-longword transfer and a similar run of DREQ tests. | `MC_FAST_POLLS` cannot materially accelerate the bulk inner loop. |
| IRQ duration | Exec recommends deferring lengthy interrupt work, but the board holds INT2 active while DREQ is asserted. Returning without moving the data would immediately retrigger the PORTS interrupt, and the byte-exact original-ROM disassembly uses no mask or acknowledge register. | Keep the IRQ copy. A software interrupt alone does not solve this; a card-local mask would require new hardware evidence. |
| Disconnect/reselect | IDENTIFY forbids disconnect and the driver executes one request at a time. | Low priority for one solid-state target; potentially important with multiple targets or mechanical disks. |
| Synchronous SCSI | MCDevice deliberately selects asynchronous operation. Previous sync tests used the old driver and do not prove MCDevice cannot support it, but they showed FIFO/gross-error risk and no speed gain. | Do not enable until error handling has passed on hardware. Then test offsets 4 and 8 with automatic async fallback. |
| Write performance | Correctness and restore tests exist, but comparable write-throughput measurements do not. | Add restored-area write timing before tuning the output loop. |

## Initial 42 MHz tuning matrix

Only safe settings belong in an automatic sweep. Sync and DMA32 remain explicit
experiments.

| Parameter | Baseline | Sweep |
|---|---:|---:|
| `MC_FAST_POLLS` | 3360 | 512, 1024, 1680, 2520, 3360 |
| `MC_IRQ_ROUNDS` | 4 | 1, 2, 4, 8 |
| IRQ DMA phase | 64 KiB | MCDiag `EXT24`: 128, 256, 512 KiB and 1 MiB |
| Small-transfer mode | IRQ | IRQ or polled below 4, 8 and 16 KiB |
| Board transfer | DMA16 | DMA16 |
| SCSI transfer | asynchronous | asynchronous |

Each result must include:

- 512-byte, 8 KiB, 32 KiB, 128 KiB and 512 KiB read latency/throughput;
- restored-area write throughput;
- CPU availability and maximum vertical-blank gap;
- transfer counts, interrupt entries and timer sleeps;
- an integrity comparison after every setting.

The intended output is three profiles: maximum throughput, balanced, and
maximum responsiveness. A single universal optimum is not expected because the
target's command latency and the accelerator RAM timing matter as much as CPU
clock.

## 68030 inner-loop analysis

The MC680x0 reference timing tables confirm the real-card results. These are
nominal core timings with a warm instruction cache; MasterCard and RAM wait
states are additional.

| Sequence | Nominal 68030 result | Decision |
|---|---:|---|
| `tst.b (a2)` plus `bmi` | `tst` is 2 clocks plus the 3-clock `(An)` fetch; an immediate-memory `btst #7` costs more. | Keep `tst.b` and the sign branch. |
| Four `move.l (a3),(a4)+` | About 7 clocks per longword, 28 clocks per 16-byte board request, before bus wait states. | Best safe DMA16 copy tested. |
| Two source `movem.l` plus one destination `movem.l` | About 44 clocks for the three transfers, plus pointer adjustment. | Slower in theory and on the card; retain only as a diagnostic comparison. |
| `subq.l` plus `beq` | 6 clocks on normal iterations; the comparable `dbcc` path is also 6 and is slower when the counter expires. | Keep the current counter loop. |
| Eight longword moves per `$C039` test | Saves one flag access and loop branch per 32 bytes, explaining the measured DMA32 gain. | Not a production setting: `$C039` only guarantees 16 bytes. |

The hot part of `fast16_in` is about 62 bytes, so it fits comfortably in the
68030's 256-byte instruction cache. Alignment or further opcode substitution is
therefore unlikely to produce a material gain. The remaining safe opportunities
are outside the four-move kernel: reduce avoidable DREQ/status accesses and use
the CF94's extended transfer counter to avoid restarting a long data phase every
64 KiB. The latter must remain an opt-in IRQ-build experiment until it passes
parity, gross-error, short-transfer and data-integrity tests on the card.

The combined `dbpl` formulation was already measured in run 5, including a
four-`move.l` comparison, and was 3-5% slower. The flag commonly drops while
the FIFO refills, so its correction path matters more than the steady-state
instruction timing suggests. `ROM16` instead measures the exact trade made by
M-Tec: no software burst counter at all. It is not a candidate for the default
until prolonged tests show a meaningful benefit and its reduced fault
containment is accepted explicitly.

`EXT24` is based on AMD's
[Am53CF94/Am53CF96 datasheet](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/datasheets/17348.pdf):
set `CFG2.ENF`, issue DMA NOP, then load registers `$00`, `$01` and `$0E`.
MCDevice clears ENF after the phase so the normal zero-count value continues to
mean 64 KiB rather than the extended 16 MiB maximum.

Motorola's MC68030 User's Manual adds the following constraints:

- The instruction tables assume two-clock reads and writes. MasterCard and RAM
  wait states add directly to the operand reads and writes, so the 28-clock
  DMA16 figure is only a lower bound.
- The 68030 data cache is write-through. A cache hit can remove an operand read,
  but every write still performs an external bus cycle. Consequently a SCSI
  read must perform both the uncached card-port read and the Fast-RAM write for
  every longword; cache settings cannot eliminate either half.
- A SCSI write may benefit from cached or burst reads of sequential Fast RAM if
  the accelerator memory implements the required synchronous burst handshake.
  The card-port write remains external. This makes restored-area write timing
  worth measuring separately rather than assuming read and write symmetry.
- Card flags and the `$F000` data port are device registers and must be
  cache-inhibited by the system mapping or hardware `CIIN` signal. Globally
  clearing or changing the user's caches in the driver is neither necessary nor
  a valid transfer optimization.

## Interpreting devtest CPU usage

The CPU percentage printed by `devtest` is a scheduler-availability metric, not
CPU instruction accounting. The program creates a priority -10 task that spins
continuously. Its `tc_Launch` hook records the EClock when Exec schedules it and
its `tc_Switch` hook accumulates the elapsed time when Exec schedules another
task. Reported CPU load is `100 * (total - scheduled_idle) / total`.

An interrupt does not cause a task switch. If a SCSI interrupt arrives while
the low-priority task is current, the whole interrupt-server copy is included in
`scheduled_idle`, even though the processor is busy moving every byte. If the
same interrupt arrives while the device task is current, that time is reported
as busy. MCDevice starts the CF94 transfer from its task and the board can assert
INT2 immediately, which accounts for its 100% result. M-Tec's handler can run
after the benchmark's idle task has become current and therefore reports only
13% despite its equivalent four-longword CPU copy loop.

At 6 MB/s the four moves per 16 bytes alone require about 1.5 million
memory-to-memory `move.l` instructions per second. Their nominal 68030 core time
already exceeds 10 million clocks per second before polling, loop overhead and
card/RAM wait states. A true 13% CPU cost at 42 MHz is therefore not credible.
Do not change scheduling merely to improve this reported percentage. Use
throughput, command latency, maximum vertical-blank gap and integrity as the
primary measurements; a separate cycle/progress measurement is needed for true
CPU consumption.

The Amiga ROM Kernel Reference Manual confirms the mechanism: interrupts run
in supervisor mode on the common system stack, outside task-stack execution.
Only when an interrupt is about to return to user mode does Exec check whether
the scheduler must run. The task fields used by `devtest` are explicitly the
callbacks for a task losing and getting the CPU, so they do not bracket ordinary
interrupt entry and exit.

The same reference says interrupt code should finish promptly and defer lengthy
work through a software interrupt or a signalled task. That advice cannot be
applied mechanically here. The board's data request itself appears to drive the
shared level-2 PORTS interrupt and remains active until the CPU services the data
port. A deferred routine would leave INT2 pending and prevent useful task
execution unless the board or CF94 path provides a separate way to mask or
acknowledge DREQ while preserving the transfer. The original ROM's interrupt
handler contains no such access: it also copies until DREQ drops.

## Build-time tuning

The default is 42 MHz:

```sh
cd MCDevice
./build.sh
```

For a different CPU clock:

```sh
MC_CPU_MHZ=50 ./build.sh
```

For a reproducible 42 MHz tuning build, override either CPU-side wait directly:

```sh
MC_CPU_MHZ=42 MC_FAST_POLLS=1680 MC_IRQ_ROUNDS=4 ./build.sh
```

`MC_FAST_POLLS` is the task-context wait before using timer.device.
`MC_IRQ_ROUNDS` is the number of eight-test DREQ polling rounds retained by the
interrupt server before it returns. Both must be positive integers. The build
script passes these settings to disk-loaded and ROM variants, and also preserves
them when `DOCKER=...` is used.

The clock value does not alter CF94 timing registers. Those continue to describe
the card's 40 MHz SCSI-controller clock.

## A4091 comparison

The A4091 is useful as a correctness reference, but its low CPU use does not
describe the MasterCard data path.

- A4091 commit `ca3b1f8` adds `START UNIT` during attachment; it is not a
  performance change.
- The related polling fix is `94cd375` (`siop_poll(): Turn interrupts on before
  exiting`). Its timeout path failed to release an interrupt-disable level. The
  intended call was corrected to `bsd_splx(s)` in `89468e8`; `43a67be` later
  made every BSD SPL wrapper pair map one-to-one to Exec `Disable()`/`Enable()`.
- Leaking an interrupt-disable level made later timer delays busy-poll and could
  eventually leave interrupts masked. This explains abnormally high CPU use
  after the A4091 error path, but it does not make its DMA engine faster.
- MCDevice's comparable paths have been audited. The IRQ timeout and both exits
  from the polled DMA phase pair every `Disable()` with `Enable()`; MCDiag's
  takeover drain is paired as well.

The A4091's NCR53C710 is a bus-master controller: its SCRIPTS engine follows
scatter/gather DMA addresses and moves payload directly between SCSI and Amiga
RAM. The MasterCard's 53CF94 uses its DMA interface and the card's DMA data port,
but the 68030 still moves each word/longword between that port and RAM. Therefore
an A4091 result near 5% CPU cannot be reached by applying its interrupt-balance
fix to MCDevice. MCDevice can reduce flag polls, phase restarts and task wakeups,
but payload-copy CPU time remains unless the MasterCard hardware has an
undocumented bus-master path.
