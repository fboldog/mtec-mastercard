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
| DMA transfer count | The CF94 has a 24-bit counter, but MCDevice uses only 16 bits and restarts the data phase every 64 KiB. | Next safe experiment. Enable the CF94 feature only in a separate IRQ test build and compare 64, 128, 256, 512 KiB and 1 MiB phases. |
| DMA16 | The `$C039` path is correct and fastest among the guaranteed board modes. | Production default. |
| DMA32 | Skipping every second `$C039` check was about 8% faster in MCDiag, but the flag only guarantees 16 bytes. | Diagnostic experiment only; do not auto-select. |
| IRQ wait | Small commands normally finish inside the short poll and report zero timer sleeps, so they consume CPU while waiting. | Tune `MC_FAST_POLLS` for throughput versus task responsiveness. |
| Bulk IRQ behavior | A 64 KiB read normally takes two server entries and zero timer sleeps. M-Tec's disassembled handler uses the same four-longword transfer and a similar run of DREQ tests. | `MC_FAST_POLLS` cannot materially accelerate the bulk inner loop. |
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
| IRQ DMA phase | 64 KiB | 64, 128, 256, 512 KiB, 1 MiB after 24-bit-counter support |
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
