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
| CF94 error handling | Illegal-operation and parity status bits were recorded but ignored. Parity reporting was disabled. This made sync or aggressive DMA experiments unsafe. | Parity reporting and abort-on-parity are enabled; parity returns `HFERR_Parity`, illegal operation returns `HFERR_DMA`, and the bus is reset. Hardware testing exposed and localized the pre-existing 16-byte-CDB FIFO overflow. |
| Short interrupt poll | `FAST_POLLS=4000` represented about 1 ms at 50 MHz, but was machine-specific. | It is now `80 * MC_CPU_MHZ`, with a 42 MHz default (3360 polls) and build-time overrides. |
| DMA transfer count | The original path uses 16 bits and restarts the data phase every 64 KiB. The NCR 53CF94/96-2 manual documents a 24-bit counter behind `CFG2.ENF`, loaded after DMA NOP. | Three matched-size runs passed through 512 KiB. The 512 KiB result was consistently 2.0-3.5% faster, so normal reads of at least 512 KiB now use extended-count phases capped at 512 KiB. Writes and shorter reads retain DMA16. |
| DMA16 | The `$C039` path is correct and fastest among the guaranteed board modes. | Production default. |
| DMA32 | Skipping every second `$C039` check was about 8% faster in MCDiag, but the flag only guarantees 16 bytes. | Diagnostic experiment only; do not auto-select. |
| Original-ROM loop | M-Tec omits MCDevice's software burst bound and trusts the CF94 counter. This removes `subq`/`beq` from each 16 bytes but a stuck DREQ could overwrite memory. | Rejected for production. In the fair IRQ comparison it measured 3844 KiB/s against DMA16's 3841 KiB/s: no useful gain for losing the software bound. |
| IRQ loop entry | IRQ code tested `$C039`, then the assembly loop tested it again and saved four unused data registers. The loop also discarded a status value that C reread. | Implemented: ready-entry variants consume the guaranteed 16 bytes immediately, DMA16 saves only the registers it uses, and sampled interrupt status is returned to C. The first hardware run passed all normal integrity tests. |
| IRQ wait | Small commands normally finish inside the short poll and report zero timer sleeps, so they consume CPU while waiting. | Tune `MC_FAST_POLLS` for throughput versus task responsiveness. |
| Bulk IRQ behavior | A 64 KiB read normally takes two server entries and zero timer sleeps. M-Tec's disassembled handler uses the same four-longword transfer and a similar run of DREQ tests. | `MC_FAST_POLLS` cannot materially accelerate the bulk inner loop. |
| IRQ duration | Exec recommends deferring lengthy interrupt work, but the board holds INT2 active while DREQ is asserted. Returning without moving the data would immediately retrigger the PORTS interrupt, and the byte-exact original-ROM disassembly uses no mask or acknowledge register. | Keep the IRQ copy. A software interrupt alone does not solve this; a card-local mask would require new hardware evidence. |
| Disconnect/reselect | IDENTIFY forbids disconnect and the driver executes one request at a time. | Low priority for one solid-state target; potentially important with multiple targets or mechanical disks. |
| Synchronous SCSI | Production commands remain asynchronous. Previous sync tests used the old driver, offset 15, and showed FIFO/illegal-operation risk with no speed gain. | Offset 4 now negotiates and no longer deadlocks, but the first 512-byte read uses 248 word moves after one burst and stops before status phase. This cannot improve throughput; production sync is rejected. |
| Write performance | Correctness and restore tests exist, but comparable write-throughput measurements do not. | Add restored-area write timing before tuning the output loop. |

## Initial 42 MHz tuning matrix

Only safe settings belong in an automatic sweep. Sync and DMA32 remain explicit
experiments.

| Parameter | Baseline | Sweep |
|---|---:|---:|
| `MC_FAST_POLLS` | 3360 | 512, 1024, 1680, 2520, 3360 |
| `MC_IRQ_ROUNDS` | 4 | 1, 2, 4, 8 |
| IRQ DMA phase | 64 KiB | MCDiag `EXT24`: 128, 256 and 512 KiB |
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
parity, illegal-operation, short-transfer and data-integrity tests on the card.

The combined `dbpl` formulation was already measured in run 5, including a
four-`move.l` comparison, and was 3-5% slower. The flag commonly drops while
the FIFO refills, so its correction path matters more than the steady-state
instruction timing suggests. `ROM16` instead measures the exact trade made by
M-Tec: no software burst counter at all. It is not a candidate for the default
until prolonged tests show a meaningful benefit and its reduced fault
containment is accepted explicitly.

`EXT24` is based primarily on NCR's
[53CF94/96-2 Fast SCSI Controller data manual](https://www.bitsavers.org/components/ncr_symbios/scsi/53CF94/53CF94_96-2_Fast_SCSI_Controller_Data_Manual_Apr1993.pdf):
set `CFG2.ENF`, issue DMA NOP, then load registers `$00`, `$01` and `$0E`.
MCDevice clears ENF after the complete experimental command so the normal
zero-count value continues to mean 64 KiB rather than the extended 16 MiB
maximum.

AMD's
[Am53CF94/Am53CF96 datasheet](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/datasheets/17348.pdf)
is a useful cross-check because it implements the same register and command
model, including ENF, the 24-bit counter and Select with ATN and Stop. It is not
treated as proof that the chips are identical: AMD explicitly documents
vendor-specific differences, including Configuration Register 4 bit 0.

## First hardware experiment run

The first run of `MCTest-perf-irq-tested.adf` completed the normal read suite
with 84 passes, no failures and one unsupported WRITE/READ BUFFER skip. Its
complete console capture is in
[`logs/mcdiag-10-perf-experiments.log`](logs/mcdiag-10-perf-experiments.log).

| Test | Result | Interpretation |
|---|---:|---|
| IRQ DMA16, 64 KiB x16 | 3856 KiB/s | Safe baseline; data and guard checks passed. |
| DMA32, 64 KiB x16 | 4188 KiB/s | About 8.6% faster, but still unsafe because `$C039` guarantees only 16 bytes. |
| MOVEM, 64 KiB x16 | 3474 KiB/s | Confirms that `movem.l` is slower. |
| ROM16, 64 KiB x16 | 3944 KiB/s | About 2.3% faster in this build, but it accidentally used the polled path; the next image routes it through IRQ for a fair comparison. |
| EXT24, 128/256/512 KiB | 5488/5685/5679 KiB/s | All three data checks passed, but this build did not time matching DMA16 command sizes. The corrected sweep reports both modes at each size. |
| EXT24, 1 MiB | invalid | The impossible 12393 KiB/s result and corrupted VBlank duration show memory/state corruption even though the immediate data comparison passed. Single phases are therefore capped at 512 KiB. |

The following `READ_CAPACITY_16` returned `HFERR_DMA`. The corrected second run
removed the 1 MiB phase but reproduced that failure, so it was not caused only
by the oversized phase. Requests larger than 512 KiB remain valid in normal
DMA16 mode because the driver divides them into 64 KiB phases.

### Corrected matched-size run

The second capture is in
[`logs/mcdiag-11-perf-corrected.log`](logs/mcdiag-11-perf-corrected.log). It
completed with 86 passes, no failures and one unsupported buffer-command skip.

| Size | DMA16 | EXT24 | EXT24 difference |
|---:|---:|---:|---:|
| 128 KiB | 5392 KiB/s | 5491 KiB/s | +1.8% |
| 256 KiB | 5600 KiB/s | 5588 KiB/s | -0.2% |
| 512 KiB | 5606 KiB/s | 5804 KiB/s | +3.5% |

The ordinary devtest 512 KiB path reached 5877 KiB/s in the same run, so EXT24
had not yet established a throughput advantage. More importantly, the following
`READ_CAPACITY_16` again returned `HFERR_DMA`.

### ENF-lifetime and 16-byte-CDB run

The third capture is in
[`logs/mcdiag-12-perf-enf-lifetime.log`](logs/mcdiag-12-perf-enf-lifetime.log).
Keeping ENF stable through command-complete did not fix READ CAPACITY(16): the
new immediate check failed before entering a data phase, and devtest reproduced
it. The EXT24 comparisons themselves all passed and were about 2% faster:

| Size | DMA16 | EXT24 | EXT24 difference |
|---:|---:|---:|---:|
| 128 KiB | 5377 KiB/s | 5525 KiB/s | +2.8% |
| 256 KiB | 5560 KiB/s | 5682 KiB/s | +2.2% |
| 512 KiB | 5561 KiB/s | 5672 KiB/s | +2.0% |

The failure was introduced before EXT24, when the driver began treating CF94
status bit 6 as fatal. NCR calls that bit Illegal Operation Error, not Gross
Error; FIFO overflow is one documented cause. MCDevice put IDENTIFY plus a
16-byte group-4 CDB into the chip's 16-byte FIFO, while the CF94 selection steps
only recognize group 4 as a reserved six-byte command. Earlier builds ignored
the resulting error. The corrected path uses Select with ATN and Stop, sends a
NO OPERATION message, and transfers the 16-byte CDB separately in command phase.

### Clean 16-byte-CDB run and promotion decision

The fourth capture is in
[`logs/mcdiag-13-perf-cdb16-fixed.log`](logs/mcdiag-13-perf-cdb16-fixed.log).
The immediate READ CAPACITY(16) check and devtest's later check both passed.
MCDiag completed with 87 passes, no failures and one unsupported buffer-command
skip; devtest's normal 512 KiB result was 5881 KiB/s.

| Size | DMA16 | EXT24 | EXT24 difference |
|---:|---:|---:|---:|
| 128 KiB | 5440 KiB/s | 5422 KiB/s | -0.3% |
| 256 KiB | 5611 KiB/s | 5681 KiB/s | +1.2% |
| 512 KiB | 5565 KiB/s | 5686 KiB/s | +2.2% |

Across the three fair runs, EXT24 averaged about +1.4%, +1.1% and +2.6% at
128, 256 and 512 KiB respectively. Only the 512 KiB result was positive in all
three runs. The production threshold is therefore deliberately conservative:
reads of at least 512 KiB use one or more extended phases of at most 512 KiB;
shorter reads and all writes keep the bounded 16-bit-count path. The diagnostic
DMA16 mode remains an unmodified baseline for future comparisons.

### Production-threshold validation

The fifth capture is in
[`logs/mcdiag-14-perf-auto-ext512.log`](logs/mcdiag-14-perf-auto-ext512.log).
With automatic extended counting enabled for large reads, MCDiag again completed
with 87 passes, no failures and one unsupported skip. Both READ CAPACITY(16)
checks passed. The normal devtest results changed as intended:

| Transfer | Before automatic EXT24 | Automatic EXT24 | Difference |
|---:|---:|---:|---:|
| 512 KiB | 5881 KiB/s | 5976 KiB/s | +1.6% |
| 128 KiB | 5614 KiB/s | 5613 KiB/s | unchanged |
| 32 KiB | 4608 KiB/s | 4619 KiB/s | unchanged/noise |

The paired diagnostic 512 KiB measurement was 5702 KiB/s for DMA16 and
5809 KiB/s for EXT24 (+1.9%). This confirms the production threshold is active
only where intended. At 512 KiB the IRQ driver is now within 0.8% of the
previously measured 6024 KiB/s M-Tec result while retaining vertical-blank and
higher-priority interrupt service.

### Production write validation

The opt-in write run is in
[`logs/mcdiag-15-perf-auto-ext512-write.log`](logs/mcdiag-15-perf-auto-ext512-write.log).
It completed with 106 passes, no failures and the expected unsupported
WRITE/READ BUFFER skip. Every DMA16, DMA8, WORD and PIO write/read combination
passed for 1 and 8 blocks; the 129-block DMA writes passed as well. The original
data was restored after every test area. Automatic extended counting remains
read-only, so all of these writes used the bounded DMA16/DMA8/WORD/PIO paths.

This clean write baseline enables the next isolated experiment. `MCDiag SYNC`
uses the driver's private command to request SDTR factor 50 (5 MB/s), offset 4,
then compares read-only 512 B, 4 KiB, 64 KiB, 128 KiB and 512 KiB commands with
an asynchronous reference. It rejects any faster/larger counterproposal,
detects parity and illegal-operation errors, and always resets the SCSI bus
back to asynchronous mode before returning. Production I/O never negotiates
sync.

### First synchronous-transfer hardware result

The first offset-4 build negotiated successfully: the disk replied with period
factor 50 and offset 4, and the CF94 accepted period register value 8. The
machine then locked before the first synchronous read completed. The last
screen output was the successful negotiation line; no synchronous read result
was written to the log.

This exposed an IRQ-path assumption rather than an SDTR failure. `irq_move()`
used the board's 16-byte-ready flag while at least 16 bytes remained and used
the word-ready flag only for the final shorter tail. With a synchronous offset
of four, the target can stop after four outstanding bytes. The board can
therefore assert word-ready and INT2 without ever reaching the 16-byte-ready
threshold. Returning without reading the word leaves INT2 asserted and traps
the CPU in the level-2 interrupt chain.

The IRQ path now consumes word-ready data whenever a 16-byte batch is not
available, regardless of the remaining command length. Normal asynchronous
transfers retain the 16-byte fast path. The next diagnostic starts with 512 B
and 4 KiB sync reads before attempting 64, 128 and 512 KiB. Its word/burst
counters will also show the performance consequence: offset 4 is expected to
force word transfers, so successful synchronous handshaking alone does not
imply a speed improvement on this board interface.

### Second synchronous-transfer hardware result

The complete second-run logs are
[`read`](logs/mcdiag-16-sync-v2-read.log),
[`write`](logs/mcdiag-16-sync-v2-write.log), and
[`sync`](logs/mcdiag-16-sync-v2-sync.log). The ordinary read run completed with
87 passes, no failures and one unsupported skip; `devtest` measured
5929/5612/4602 KiB/s for 512/128/32 KiB requests. The write run completed with
106 passes, no failures and one skip, including restoration of every modified
medium area.

The word-ready fallback removed the machine lock. SDTR again completed at
factor 50, offset 4. The first 512-byte synchronous read then moved all 512
bytes but stopped with `HFERR_Phase`: one 16-byte burst, 248 word moves,
`STAT=91`, `INTR=10`, `SSTEP=CC`, and FIFO-flags value 8. The driver reset the
bus and successfully returned the target to asynchronous operation.

This result matches the AMD controller description more closely than an FIFO
overflow theory. In synchronous Data-In, the transfer counter decrements on
DACK and reaching zero can complete the DMA command while the FIFO is not
empty. The FIFO-flags register is also latched on entry to synchronous Data-In,
so the ordinary asynchronous completion code cannot interpret its value as a
live FIFO count. A correct sync-only completion tail would need separate phase
and FIFO handling.

That additional work cannot make sync a performance win on this card. The
512-byte command already demonstrates the limiting path: after the one initial
burst, 496 of 512 bytes crossed the CPU port as individual words. The normal
asynchronous path moves all 512 bytes as 32 bounded 16-byte bursts. Larger sync
reads would therefore converge on the measured slow WORD path rather than the
DMA16 result. Sync remains useful as a controller diagnostic, but is not a
production optimization.

The local NCR 53CF94/96-2 manual identifies the likely phase-tail bug. A DMA
Transfer Information command completes only after its counter is zero, its FIFO
is empty, and the target asserts REQ for the next byte. The pre-Interrupt Status
sample describes the command which just completed; after Interrupt is read, a
fresh Status sample observes the next live phase (and also reopens the explicit
phase latch when `CFG2.ENF` is enabled). The v2 IRQ path reused the pre-Interrupt
Data-In sample and mistook the following Status phase for excess input. The next
build preserves both values for logging, uses the fresh phase after Interrupt,
and does not treat the phase-entry FIFO-flags latch as a current byte count. A
small-read v3 run is justified to validate controller correctness, not
performance.

The v3 run is in
[`logs/mcdiag-17-sync-v3-sync.log`](logs/mcdiag-17-sync-v3-sync.log), with its
ordinary baseline in
[`logs/mcdiag-17-sync-v3-read.log`](logs/mcdiag-17-sync-v3-read.log). The
baseline passed with 84 successes, no failures and one skip; `devtest` measured
5976/5605/4597 KiB/s. Sync again moved exactly 512 bytes without a lock. The
new register pair was `st91>11`: reading Interrupt cleared the interrupt bit but
the live bus phase remained Data-In. This disproves the stale-phase hypothesis.

The generic NetBSD
[`ncr53c9x` driver](https://github.com/NetBSD/src/blob/trunk/sys/dev/ic/ncr53c9x.c)
documents and handles this exact state: if its requested data count is already
zero while the target remains in Data phase, it issues a DMA Transfer Pad
command. The controller then receives and discards the excess internally until
the target changes phase. The next diagnostic uses that controller-defined
recovery only for synchronous Data-In, with a 64 KiB pad bound, five-second
timeout, and the existing bus-reset failure path. It does not enlarge or write
past the caller's buffer, and the asynchronous reference comparison still
validates the original 512 bytes.

The v4 log is
[`logs/mcdiag-18-sync-v4-sync.log`](logs/mcdiag-18-sync-v4-sync.log), with the
matching ordinary run in
[`logs/mcdiag-18-sync-v4-read.log`](logs/mcdiag-18-sync-v4-read.log). The
ordinary path remained clean at 84/0/1 and 5969/5620/4618 KiB/s. In this build
the phase had already advanced by the post-Interrupt sample (`st91>13`), so
Transfer Pad was not needed (`q0`). The 512-byte command completed in 2321 us,
but its data first differed from the asynchronous reference at byte offset 4,
exactly the negotiated synchronous offset.

This makes controller status handling secondary to payload integrity. Offset
four cannot continuously fill the board's 16-byte-ready window, and the first
failure occurring at that boundary points at the mixed initial-burst/word path.
The next diagnostic first performs one 512-byte synchronous read using only the
word-ready handshake and logs the first 16 reference and received bytes on any
mismatch. It attempts the existing DMA16 test only if that conservative WORD
read is exact. This distinguishes a burst-window violation from corruption in
the underlying synchronous/DACK path without increasing the tested transfer
size.

The sync log's three pre-test failures were unrelated: REQUEST SENSE returned
pending sense data once and cleared it, so the three following calls correctly
returned different bytes. MCDiag now runs this state-consuming command only
once instead of treating repeated payloads as a transfer-mode comparison.

The v5 logs are
[`logs/mcdiag-19-sync-v5-sync.log`](logs/mcdiag-19-sync-v5-sync.log) and
[`logs/mcdiag-19-sync-v5-read.log`](logs/mcdiag-19-sync-v5-read.log). The
ordinary path again passed at 84/0/1 and devtest measured 5973/5604/4633 KiB/s.
The isolated synchronous WORD read was byte-exact: 512 bytes moved as 256 word
handshakes in 2815 us, with no burst and no Transfer Pad (`b0 w256 q0`). This
proves that SDTR, the CF94 synchronous period/offset programming, and the
board's word-ready DACK path all work together.

The following interrupt-driven DMA16 attempt entered the 16-byte routine once,
then moved 248 words, exhausted its programmed count in Data-In, and required
Transfer Pad before returning `HFERR_DMA` (`b1 w248 q1 tcFFF8`). Together with
v4's first mismatch at the negotiated offset, this isolates the fault to the
board's 16-byte-ready/burst semantics during synchronous transfers. The IRQ
mover now suppresses that path whenever the command has a non-zero synchronous
offset. It still accepts either `$C039` or `$D039` before moving one word, as
the existing tail path did; it does not consume 16 bytes from one `$C039`
sample. Asynchronous commands retain their existing fast 16-byte path.

The conservative path was intended to establish a correct interrupt-driven
synchronous baseline rather than assume a speed improvement. The later ENF
tests show that word-at-a-time transfers can nevertheless become fast when the
controller uses one uninterrupted 24-bit-count phase. Production commands
remain asynchronous; synchronization is still enabled only by the diagnostic
command.

The v6 logs are
[`logs/mcdiag-20-sync-v6-sync.log`](logs/mcdiag-20-sync-v6-sync.log) and
[`logs/mcdiag-20-sync-v6-read.log`](logs/mcdiag-20-sync-v6-read.log). The normal
run remained clean at 5984/5613/4702 KiB/s. Every synchronous read from 512
bytes through 512 KiB was byte-exact, every command used only word accesses
(`b0`), no Transfer Pad was needed (`q0`), and the diagnostic completed at
90/0/1 before its deliberate asynchronous-reset restore. This establishes a
correct interrupt-driven synchronous baseline.

The timing exposed a potentially important distinction. Transfers split into
ordinary 16-bit-count chunks reached about 0.53-0.66 MiB/s, but the single 512
KiB ENF/24-bit phase reached 4589 KiB/s and completed its last command in
111351 us. Both paths moved one word per ready test and compared exactly. The
v7 diagnostic therefore runs matched 16-bit and 24-bit-count transfers at 64,
128, 256 and 512 KiB. It also fixes the sub-KiB throughput calculation; the
very large v6 VBlank-gap values are instrumentation anomalies and not used as
transfer timings.

The v7 logs are
[`logs/mcdiag-21-sync-v7-sync.log`](logs/mcdiag-21-sync-v7-sync.log) and
[`logs/mcdiag-21-sync-v7-read.log`](logs/mcdiag-21-sync-v7-read.log). All
matched reads were byte-exact, used no bursts or Transfer Pad (`b0 q0`), and
the sync run finished at 95/0/1. The ordinary baseline also remained clean at
84/0/1 and 5978/5600/4591 KiB/s. At 64 KiB the reported SYNC16 and SYNC24 rates
were effectively equal, while the single 24-bit-count phase pulled ahead as
the transfer grew:

| Size | 16-bit chunks | 24-bit phase |
|---:|---:|---:|
| 64 KiB | 857 KiB/s | 861 KiB/s |
| 128 KiB | 592 KiB/s | 1669 KiB/s |
| 256 KiB | 706 KiB/s | 2455 KiB/s |
| 512 KiB | 689 KiB/s | 5701 KiB/s |

The final absolute value cannot be accepted: it exceeds the negotiated 5 MB/s
bus rate, and every synchronous VBlank sample in that section contains the
same impossible approximately 1762-second gap. Correctness, chunk counts and
the relative ENF advantage remain useful, but the concurrent VBlank/EClock
instrumentation has contaminated the aggregate timing. V8 removes that probe
from synchronous tests and instead sums the four per-command microsecond
measurements made directly around `DoIO`, reporting their minimum and maximum
as a variability check.

The v8 logs are
[`logs/mcdiag-22-sync-v8-sync.log`](logs/mcdiag-22-sync-v8-sync.log) and
[`logs/mcdiag-22-sync-v8-read.log`](logs/mcdiag-22-sync-v8-read.log). Data again
compared exactly at every size and the runs finished at 95/0/1 and 84/0/1; the
ordinary devtest result was 5977/5592/4695 KiB/s. Direct command timings made
the anomaly more specific. The 256 KiB 24-bit commands were stable at
102284-103265 us, but the four 512 KiB commands ranged from 21320 to 113816 us.
The fastest sample would require over 24 MiB/s and too few 42 MHz CPU cycles
for 262144 iterations of the word mover.

The register table in the
[`NCR 53CF94/96-2 Data Manual`](../docs/53CF94_96-2_Fast_SCSI_Controller_Data_Manual_Apr1993.pdf)
confirms that synchronous-period value 8 with a 40 MHz clock and FAST SCSI
clear transfers one byte every eight clocks: exactly 5.0 MB/s. The 21 ms sample
therefore cannot be a valid 512 KiB transfer. V8 poisoned and compared the
destination only around the complete four-command group, so an early false
completion could be hidden by a later correct read of the same LBA. V9 poisons
and validates every repetition immediately, selects different regions within
the captured 512 KiB reference where the transfer size permits, and prints all
four command times. This separates stale/short-transfer acceptance from an
EClock measurement fault.

The v9 logs are
[`logs/mcdiag-23-sync-v9-sync.log`](logs/mcdiag-23-sync-v9-sync.log) and
[`logs/mcdiag-23-sync-v9-read.log`](logs/mcdiag-23-sync-v9-read.log). Every
individually poisoned command compared exactly, including alternating halves
of the 512 KiB reference during the 256 KiB tests. The run again completed at
95/0/1 with `b0 q0`; the ordinary baseline was 84/0/1 and
5970/5601/4590 KiB/s. This rules out a prior correct repetition merely hiding
a later false completion in the destination buffer.

The implausible readings persisted and are now visible individually. The
256 KiB 24-bit sequence was `[102417 9039 102455 8222]` us and the 512 KiB
sequence was `[112851 17503 109898 109638]` us. Even the shortest values still
passed immediate full-buffer and guard-byte validation. V10 records an
independent AmigaDOS `DateStamp()` interval around each command alongside the
EClock value. At 50 Hz this clock is coarse, but a legitimate 512 KiB transfer
at the negotiated 5 MB/s must span at least about five ticks. It does not
modify the driver or the transfer loop.

The v10 logs are
[`logs/mcdiag-24-sync-v10-sync.log`](logs/mcdiag-24-sync-v10-sync.log) and
[`logs/mcdiag-24-sync-v10-read.log`](logs/mcdiag-24-sync-v10-read.log). The
independent wall clock agrees with the now-stable large-transfer EClock result:
four byte-exact 512 KiB ENF reads took 112407, 108454, 108613 and 108796 us,
each spanning five or six 20 ms ticks. Their combined rate was 4672 KiB/s,
about 95.7% of the negotiated 5.0 MB/s wire rate after converting decimal MB/s
to KiB/s. The commands remained a single phase with no bursts or Transfer Pad
(`b0 c1 q0`), and the run completed at 95/0/1. The normal path remained clean
at 84/0/1 and 5980/5609/4703 KiB/s.

Read-side synchronous transport is therefore correct and near the negotiated
bus limit for 512 KiB ENF phases. Before normal I/O can enable it, Data-Out
must be validated. V11 adds a separate opt-in `S:sync-write-test`: it saves one
block near the end of the medium asynchronously, negotiates sync, writes and
reads a pattern synchronously, resets the bus to async, then restores and
verifies the original block asynchronously. The restore path runs after any
sync write/read failure; the ordinary read-only sync script remains unchanged.

The v11 logs are
[`logs/mcdiag-25-sync-v11-write.log`](logs/mcdiag-25-sync-v11-write.log) and
[`logs/mcdiag-25-sync-v11-read.log`](logs/mcdiag-25-sync-v11-read.log). The
synchronous one-block write and readback were exact (`b0 w256 q0`), the bus
reset returned the target to async, and the original LBA 15711956 was restored
and verified asynchronously. The run finished at 96/0/1. This validates both
synchronous directions and the failure-safe restoration sequence.

The CF94 manual requires Configuration 3 `FASTSCSI` to be clear at transfer
rates of 5 MB/s or below, and set with `FASTCLK` for rates above 5 MB/s. Earlier
diagnostics inherited `$18` from initialization at every rate; v12 uses `$08`
for async/5 MB/s and selects `$18` per command only when the negotiated chip
period is below eight clocks. Its separate read-only `S:sync-fast-test` resets
between steps and asks for SDTR factors 50, 43, 37, 31 and 25, corresponding
to the CF94's supported 5.0, 5.7, 6.6, 8.0 and 10.0 MB/s timings. Each accepted
rate must pass two individually poisoned 512 KiB ENF reads before proceeding.

The v12 logs are
[`logs/mcdiag-26-sync-v12-fast.log`](logs/mcdiag-26-sync-v12-fast.log),
[`logs/mcdiag-26-sync-v12-sync.log`](logs/mcdiag-26-sync-v12-sync.log),
[`logs/mcdiag-26-sync-v12-write.log`](logs/mcdiag-26-sync-v12-write.log), and
[`logs/mcdiag-26-sync-v12-read.log`](logs/mcdiag-26-sync-v12-read.log). Clearing
`FASTSCSI` at 5 MB/s preserved both directions and the restored-block result.
BlueSCSI accepted every requested factor through 25/10 MB/s, and all ladder
buffers compared exactly. Normal timing samples nevertheless plateaued around
4.5-4.7 MiB/s from factor 50 upward; the CPU word mover is already the limit.
The factor-25 display of 23215 KiB/s is physically impossible for this bus and
is another short-clock anomaly, not a production result. There is no sustained
benefit from negotiating above 5 MB/s with the current mover.

V5 provides a bounded next optimization: its one unsafe 16-byte synchronous
burst matched through byte three and first differed at byte four, exactly the
negotiated offset. V13 therefore never treats `$C039` as permission for 16
bytes. It rechecks the flag before each single `move.l`, consumes only four
bytes, and falls back to `$D039` words whenever that condition is unavailable.
The new `l` statistic counts these longword moves separately from 16-byte
bursts (`b`) and word moves (`w`). This tests whether using the proven-good
first longword reduces CPU-loop overhead while remaining bounded by offset
four.

The v13 logs are
[`logs/mcdiag-27-sync-v13-sync.log`](logs/mcdiag-27-sync-v13-sync.log),
[`logs/mcdiag-27-sync-v13-write.log`](logs/mcdiag-27-sync-v13-write.log), and
[`logs/mcdiag-27-sync-v13-read.log`](logs/mcdiag-27-sync-v13-read.log). Both
directions remained byte-exact, the original block was restored, and the runs
finished at 95/0/1 and 96/0/1. A 512 KiB phase used 32768 longword moves for
one quarter of the payload and 196608 words for the remaining three quarters.
Stable read samples fell from roughly 4.6 MiB/s to 4.39 MiB/s, so the first
implementation is correct but slower.

The slowdown has a direct software cause: the longword test read `$C039`, then
the fallback expression read `$C039` a second time before `$D039`. V14 retains
the bounded longword operation but reuses the first `$C039` sample. Its word
fallback now performs the same number of flag reads as v12. The iterative ADF
also uses `MCDiag TAKEOVER LOADONLY` at startup, installing the replacement
driver without automatically running the full asynchronous baseline.

The v14 logs are
[`logs/mcdiag-28-sync-v14-sync.log`](logs/mcdiag-28-sync-v14-sync.log) and
[`logs/mcdiag-28-sync-v14-write.log`](logs/mcdiag-28-sync-v14-write.log).
Both directions remained byte-exact, the original block was restored, and the
runs again finished at 95/0/1 and 96/0/1. Reusing the `$C039` sample removed
the v13 slowdown: the four 512 KiB extended-counter reads reported 76.950,
77.664, 73.221 and 73.225 ms. The independent 50 Hz intervals agreed that
these commands spanned only three or four ticks. This apparent 6.8 MiB/s is
above the negotiated 5 MB/s wire limit, so it is not a credible throughput
claim even though every destination was poisoned and fully compared. It is
additional evidence that timing this interrupt-driven path from the calling
task is unreliable. Correctness and the exact mover counts are trustworthy:
each phase used 32768 longword moves and 196608 word moves, with no FIFO or
Transfer Pad bytes.

The ordinary 16-bit counter split a 512 KiB command into eight phases and
remained very slow at about 0.68 MiB/s. Any production synchronous path must
therefore use the single-phase 24-bit counter. Before enabling that path, v15
extends the restored write test from one block to 64 KiB and uses the 24-bit
counter in both directions. It saves the complete region asynchronously,
writes and verifies it synchronously, then resets the bus and restores and
verifies all 64 KiB asynchronously.

The v15 result is
[`logs/mcdiag-29-sync-v15-write.log`](logs/mcdiag-29-sync-v15-write.log).
The 64 KiB extended-counter Data-Out and Data-In phases were both byte-exact,
used one phase with `l4096 w24576`, and required no FIFO or Transfer Pad bytes.
After the bus returned to asynchronous mode, the complete original 64 KiB
region was restored and verified. The run finished at 96/0/1. This closes the
correctness gap left by the earlier one-block write, but it does not yet show
a write-side speed benefit: the synchronous write took 129.743 ms while its
readback reported 34.801 ms. V16 therefore expands the same failure-safe test
to one complete 512 KiB extended-counter phase before production negotiation
is considered.

The v16 result is
[`logs/mcdiag-30-sync-v16-write.log`](logs/mcdiag-30-sync-v16-write.log).
The full 512 KiB synchronous Data-Out and readback were byte-exact, each used
one extended-counter phase with `l32768 w196608`, and the complete original
region at LBA 15710932 was restored and verified asynchronously. The run again
finished at 96/0/1. Data-Out took 98.675 ms (about 5.1 MiB/s), while the paired
Data-In took 168.904 ms (about 3.0 MiB/s). Together with the earlier samples,
this proves large synchronous writes are correct but confirms that measured
sync speed varies substantially between otherwise identical commands.

V17 therefore does not enable sync automatically. It adds an explicit
diagnostic control that leaves factor-50/offset-4 negotiation active for the
unit, runs ordinary `CMD_READ` traffic through the driver's normal API, and
then resets the bus to asynchronous mode. Normal synchronous data phases use
the proven 24-bit counter from 64 KiB upward; smaller phases retain the proven
16-bit counter. This isolates the production-path experiment from boot and
unrelated targets.

The v17 result is
[`logs/mcdiag-31-sync-v17-driver.log`](logs/mcdiag-31-sync-v17-driver.log).
Negotiation remained active through ordinary `CMD_READ` and `HD_SCSICMD`
traffic, every API/capacity check completed, and the final explicit bus reset
restored asynchronous operation. Normal-driver read rates were 5041, 1431 and
444 KiB/s for 512, 128 and 32 KiB transfers. The comparable asynchronous
baseline is roughly 5980, 5600 and 4700 KiB/s. Offset-4 sync is therefore a
17% regression at 512 KiB and a severe regression at ordinary smaller sizes;
it must not be enabled automatically.

The remaining bounded optimization is a larger negotiated offset. The IRQ
mover already rechecks `$C039` before every four-byte move, so an offset of
eight or more may naturally keep the flag asserted for additional safe
longwords without restoring the corrupt 16-byte burst. V18 adds a read-only
factor-50 offset ladder (4, 8, 12 and 15), resetting to async and checking two
individually poisoned 512 KiB buffers after each step.

The v18 result is
[`logs/mcdiag-32-sync-v18-offset.log`](logs/mcdiag-32-sync-v18-offset.log).
BlueSCSI accepted every requested offset, all eight 512 KiB reads were
byte-exact, every asynchronous reset succeeded, and the run finished at
88/0/1. Larger offsets did not change the mover at all: offsets 4, 8, 12 and
15 each used exactly `l32768 w196608`. Throughput remained dominated by the
same implausibly alternating short/long timing samples, so no offset was a
measurable winner.

The counts show why: rechecking `$C039` after every longword makes the mover
safe but the flag drops after that first four-byte access regardless of the
negotiated offset. V19 tests a still-bounded interpretation of the earlier v5
failure boundary. One fresh `$C039` sample may authorize no more than the
negotiated offset, rounded down to a longword: 4, 8 or 12 bytes, never the old
unsafe 16-byte burst. The same read-only offset ladder will stop immediately
if that scaling assumption is wrong.

The v19 result is
[`logs/mcdiag-33-sync-v19-offset-burst.log`](logs/mcdiag-33-sync-v19-offset-burst.log).
All offsets and resets passed again at 88/0/1. More importantly, the exact
mover counts scaled as predicted: offset 4 used `l32768 w196608` (25% of bytes
by longword), offset 8 used `l65536 w131072` (50%), and offsets 12/15 used
`l98304 w65536` (75%). This proves that consuming no more than the negotiated
offset after one fresh `$C039` sample is safe for Data-In on this target.

The timing still cannot select a winner: offset 15 reported an impossible
10.9 MiB/s at a negotiated 5 MB/s, while offset 12 was slower than offset 8
despite identical mover counts. Offset 8 is the conservative candidate because
it halves the word traffic, uses an even power-of-two credit, and produced two
similar large-read samples. V20 leaves offset 8 active only for the existing
read-only ordinary-driver suite, then explicitly resets the bus to async. Disk
writes remain untested and disabled at that offset.

The v20 result is
[`logs/mcdiag-34-sync-v20-driver-offset8.log`](logs/mcdiag-34-sync-v20-driver-offset8.log).
Every ordinary API and capacity check passed, and the final reset restored
asynchronous operation. Offset 8 produced 5983, 5596 and 4720 KiB/s for 512,
128 and 32 KiB reads. These are effectively the same as the established async
baseline (roughly 5980/5600/4700) and dramatically better than offset 4
(5041/1431/444). Command latency likewise recovered from about 20 ms to 3 ms.

This is the first normal-I/O sync configuration without a performance
regression. It is not ready for automatic use until the same bounded offset-8
grouping is validated for Data-Out. V21 therefore repeats the failure-safe
512 KiB synchronous write/readback test at offset 8, followed by a bus reset
and full asynchronous restoration and verification of the original region.

### Startup logging stall

The normal log contains no unexplained command stall. Its longest deliberately
slow operations are about 100 ms for a 64 KiB WORD read and 319 ms for a 4 KiB
PIO read. MCDiag's timer stops before each log line is printed, however, and the
startup run previously called `Flush()` on `DF0:` after every line. Physical
floppy writes can therefore create visible multi-second pauses absent from the
SCSI timings. The startup sequence now logs the stable MCDiag read suite to RAM
and copies the completed log to `DF0:` before appending devtest output. The
opt-in sync diagnostic continues logging directly to floppy so a hardware hang
still leaves the last completed checkpoint.

The two fair IRQ ROM16 runs differed from bounded DMA16 by 0.08% and 1.7%.
That small, unstable gain does not justify removing the software buffer bound.
The safe ready-entry changes remain: they avoid one known-ready DREQ read, save
only the registers used by the loop, and return the already-sampled chip status.

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
