/* esp.c - the NCR 53CF94 on the MasterCard, as a SCSI initiator.
 *
 * One command at a time, no disconnection. Production commands are
 * asynchronous; the diagnostic interface can opt into synchronous transfers.
 * The chip's interrupts are caught by mc_irq on INT2, which latches the status,
 * sequence step and interrupt registers and signals the driver task. Data
 * phases go through the board's data port with the chip in DMA mode: the CPU
 * moves 16 bytes each time the board says it can, then words at the end. Like
 * the original driver, this runs with interrupts disabled, so the board's data
 * request can't hold INT2 while the task is not looking.
 */
#include <exec/execbase.h>
#include <exec/errors.h>
#include <devices/scsidisk.h>
#include "mc.h"
#include "gen/stubs.h"

void *memset(void *d, int c, unsigned long n);

#define REG(mc, n) ((mc)->board[MC_REGS + ((n) << 3)])
#define SPIN_LIMIT 0x00080000UL     /* polls without progress before a DMA chunk gives up, ~0.5 s */
#define MAX_PHASES 64
#define MAX_EXT_PHASE 0x00080000UL  /* 1 MiB failed on hardware; stop at 512 KiB */
#define CFG2_NORMAL (CFG2_S2FE | CFG2_ACDPE)

/* The short wait avoids a timer request and task switch for fast commands.
   80 polls/MHz preserves the measured ~1 ms window; override either value
   when building a tuning variant. */
#ifndef MC_CPU_MHZ
#define MC_CPU_MHZ 42
#endif
#ifndef MC_DIAG_MODES
#define MC_DIAG_MODES 1     /* MCDiag's transfer modes; the ROM build leaves them out */
#endif
#ifndef MC_FAST_POLLS
#define MC_FAST_POLLS (80UL * MC_CPU_MHZ)
#endif
#ifndef MC_IRQ_ROUNDS
#define MC_IRQ_ROUNDS 4             /* 32 flag tests, a few us, before leaving */
#endif
#if MC_FAST_POLLS < 1
#error MC_FAST_POLLS must be a positive integer
#endif
#if MC_IRQ_ROUNDS < 0
#error MC_IRQ_ROUNDS must be zero or a positive integer
#endif

static inline void wr(struct MC *mc, int n, UBYTE v) { REG(mc, n) = v; }
static inline UBYTE rd(struct MC *mc, int n) { return REG(mc, n); }

/* Illegal-operation errors include FIFO overflow, mismatched DMA direction
   and an overwritten command register. Parity has a distinct Amiga error
   code. The caller resets the bus before returning either error. */
static LONG chip_error(UBYTE st)
{
    if (st & ST_PE)
        return HFERR_Parity;
    if (st & ST_IOE)
        return HFERR_DMA;
    return 0;
}

/* Interrupt server, called with is_Data in a1. Returns non-zero if the
   interrupt was the chip's. The asm entry turns that into the Z flag. */
/* Reads and drops words while the board says it holds data, at most n. */
static ULONG drain(struct MC *mc, ULONG n)
{
    volatile UWORD *pw = (volatile UWORD *)(mc->board + MC_PORT);
    ULONG k = 0;
    while (k < n && ((mc->board[MC_DREQW] | mc->board[MC_DREQ16]) & 0x80)) {
        (void)*pw;
        k++;
    }
    return k;
}

#ifdef IRQXFER
static LONG irq_xfer(struct MC *mc);
#endif

LONG mc_irq(struct MC *mc __asm("a1"))
{
    UBYTE st;
#ifdef IRQXFER
    if (mc->x.active)
        return irq_xfer(mc);
#endif
    st = rd(mc, R_STAT);
    if (!(st & ST_INT)) {
        /* The board raises INT2 while it holds data. Outside a transfer
           nobody would move it and INT2 would never clear: drop it, and
           reset the chip if that isn't enough. */
        if ((mc->board[MC_DREQW] | mc->board[MC_DREQ16]) & 0x80) {
            mc->stray++;
            drain(mc, 4096);
            if ((mc->board[MC_DREQW] | mc->board[MC_DREQ16]) & 0x80) {
                wr(mc, R_CMD, C_RESET);
                wr(mc, R_CMD, C_NOP);
                (void)rd(mc, R_INTR);
            }
            return 1;
        }
        return 0;
    }
    mc->istat = st;
    mc->iseq = rd(mc, R_SEQ);
    mc->iintr = rd(mc, R_INTR);         /* reading this clears the interrupt */
    mc->ipending = 1;
    Signal(mc->task, mc->irqsig);
    return 1;
}
__asm(
"  .text\n"
"  .globl _mc_irq_entry\n"
"_mc_irq_entry:\n"
"  jsr _mc_irq\n"
"  tst.l d0\n"
"  rts\n");

void mc_delay(struct MC *mc, ULONG micros)
{
    mc->treq.tr_node.io_Command = TR_ADDREQUEST;
    mc->treq.tr_time.tv_secs = micros / 1000000;
    mc->treq.tr_time.tv_micro = micros % 1000000;
    DoIO((struct IORequest *)&mc->treq);
}

/* Waits for the chip's interrupt; returns 0 after the time-out. */
static int wait_int(struct MC *mc, ULONG secs)
{
    /* Most interrupts come within microseconds: watch for the server's flag
       for about a millisecond before paying for a timer request and a task
       switch. Slow ones (seeks, a missing target) still sleep. */
    ULONG k;
    for (k = 0; k < MC_FAST_POLLS && !mc->ipending; k++)
        ;
    if (!mc->ipending) {
        mc->sleeps++;
        mc->treq.tr_node.io_Command = TR_ADDREQUEST;
        mc->treq.tr_time.tv_secs = secs;
        mc->treq.tr_time.tv_micro = 0;
        SendIO((struct IORequest *)&mc->treq);
        while (!mc->ipending && !CheckIO((struct IORequest *)&mc->treq))
            Wait(mc->irqsig | (1UL << mc->tport.mp_SigBit));
        if (!CheckIO((struct IORequest *)&mc->treq))
            AbortIO((struct IORequest *)&mc->treq);
        WaitIO((struct IORequest *)&mc->treq);
    }
    if (!mc->ipending)
        return 0;
    mc->ipending = 0;
    return 1;
}

void esp_init(struct MC *mc)
{
    wr(mc, R_CMD, C_RESET);
    wr(mc, R_CMD, C_NOP);
    wr(mc, R_CFG1, CFG1_DISR | CFG1_PERE | OWN_ID);
    wr(mc, R_CLKCONV, 0);               /* 40 MHz */
    wr(mc, R_TIMEOUT, 0x99);            /* 250 ms select time-out at 40 MHz */
    wr(mc, R_CFG2, CFG2_NORMAL);
    wr(mc, R_CFG3, CFG3_FASTCLK);        /* 40 MHz clock, normal <=5 MB/s timing */
    wr(mc, R_CFG4, 0x04);               /* active negation */
    wr(mc, R_SYNCPER, 0);
    wr(mc, R_SYNCOFF, 0);               /* asynchronous */
    (void)rd(mc, R_INTR);
    mc->ipending = 0;
}

/* Resets the SCSI bus, then waits the 250 ms the standard allows a device
   before it must answer selection. A drive that is still spinning up answers
   NOT READY after that, which the commands retry. */
void esp_busreset(struct MC *mc)
{
    int k;
    wr(mc, R_CMD, C_BUSRESET);
    mc_delay(mc, 1000);                 /* the chip holds RST for about 25 us */
    esp_init(mc);
    for (k = 0; k < MAX_TARGET * MAX_LUN; k++)
        if (mc->units[k]) {
            mc->units[k]->sync_period = 0;
            mc->units[k]->sync_offset = 0;
#if MC_AUTO_SYNC
            mc->units[k]->sync_tried = 0;
#endif
        }
    mc_delay(mc, 250000);
}

/* The DMA16 inner loops, in assembly for speed. Each moves 16 bytes each
   time the flag is set, until bursts is 0 (at most 4096, one 64 KB chunk),
   the chip interrupts (the target stopped early) or limit polls pass
   without the flag. While waiting, the flag is tested 8 times before the
   chip's status is: each test is a slow card access, and the original
   driver gains its speed this way. The card's port answers at $F000-$F00F, emulators only
   to $F007; reading with movem.l was no faster on the card (run 5). */
struct Fast {
    volatile UBYTE *flag;
    volatile ULONG *port;
    void *buf;
    ULONG bursts;
    volatile UBYTE *stat;
    ULONG limit;
    UBYTE status;                  /* status sampled by the assembly loop, or 0 */
};
void fast16_in(struct Fast *f __asm("a0"));     /* four move.l per flag */
void fast16_out(struct Fast *f __asm("a0"));
void fast16_in_ready(struct Fast *f __asm("a0")); /* IRQ already observed $C039 */
void fast16_out_ready(struct Fast *f __asm("a0"));
void fast32_in(struct Fast *f __asm("a0"));     /* MCMODE_DMA32: 32 bytes per flag */
void movem16_in(struct Fast *f __asm("a0"));    /* MCMODE_MOVEM: two movem.l per flag */
void rom16_in(struct Fast *f __asm("a0"));      /* original ROM: CF94 count is the bound */
void rom16_in_ready(struct Fast *f __asm("a0"));
#if MC_IRQ_ROUNDS == 0
#define MC_ZERO_LIMIT_EXIT "tst.l d1\n  beq.s 9f\n  "
#else
#define MC_ZERO_LIMIT_EXIT ""
#endif
__asm(
"  .text\n"
"  .globl _fast16_in_ready\n"
"  _fast16_in_ready:\n"
"  movem.l d2/a2-a5,-(sp)\n"
"  movem.l (a0),a2-a4\n"
"  move.l 12(a0),d0\n"
"  movea.l 16(a0),a5\n"
"  move.l 20(a0),d1\n"
"  tst.l d0\n"
"  beq.s 9f\n"
"  bra.s 3f\n"
"  .globl _fast16_in\n"
"  _fast16_in:\n"
"  movem.l d2/a2-a5,-(sp)\n"
"  movem.l (a0),a2-a4\n"
"  move.l 12(a0),d0\n"
"  movea.l 16(a0),a5\n"
"  move.l 20(a0),d1\n"
"  tst.l d0\n"
"  beq.s 9f\n"
"  1:" MC_ZERO_LIMIT_EXIT "move.l d1,d2\n"
"  2:tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  move.b (a5),d1\n"
"  bpl.s 4f\n"
"  move.b d1,24(a0)\n"
"  bra.s 9f\n"
"  4:move.l 20(a0),d1\n"
"  subq.l #1,d2\n"
"  bne.s 2b\n"
"  bra.s 9f\n"
"  3:move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  subq.l #1,d0\n"
"  beq.s 9f\n"
"  tst.b (a2)\n"
"  bmi.s 3b\n"
"  bra.s 1b\n"
"  9:move.l a4,8(a0)\n"
"  move.l d0,12(a0)\n"
"  movem.l (sp)+,d2/a2-a5\n"
"  rts\n"
"  .globl _fast16_out_ready\n"
"  _fast16_out_ready:\n"
"  movem.l d2/a2-a5,-(sp)\n"
"  movem.l (a0),a2-a4\n"
"  move.l 12(a0),d0\n"
"  movea.l 16(a0),a5\n"
"  move.l 20(a0),d1\n"
"  tst.l d0\n"
"  beq.s 9f\n"
"  bra.s 3f\n"
"  .globl _fast16_out\n"
"  _fast16_out:\n"
"  movem.l d2/a2-a5,-(sp)\n"
"  movem.l (a0),a2-a4\n"
"  move.l 12(a0),d0\n"
"  movea.l 16(a0),a5\n"
"  move.l 20(a0),d1\n"
"  tst.l d0\n"
"  beq.s 9f\n"
"  1:" MC_ZERO_LIMIT_EXIT "move.l d1,d2\n"
"  2:tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  move.b (a5),d1\n"
"  bpl.s 4f\n"
"  move.b d1,24(a0)\n"
"  bra.s 9f\n"
"  4:move.l 20(a0),d1\n"
"  subq.l #1,d2\n"
"  bne.s 2b\n"
"  bra.s 9f\n"
"  3:move.l (a4)+,(a3)\n"
"  move.l (a4)+,(a3)\n"
"  move.l (a4)+,(a3)\n"
"  move.l (a4)+,(a3)\n"
"  subq.l #1,d0\n"
"  beq.s 9f\n"
"  tst.b (a2)\n"
"  bmi.s 3b\n"
"  bra.s 1b\n"
"  9:move.l a4,8(a0)\n"
"  move.l d0,12(a0)\n"
"  movem.l (sp)+,d2/a2-a5\n"
"  rts\n"
#if MC_DIAG_MODES                        /* the MCDiag comparison loops */
"  .globl _fast32_in\n"
"  _fast32_in:\n"
"  movem.l d2/a2-a5,-(sp)\n"
"  movem.l (a0),a2-a4\n"
"  move.l 12(a0),d0\n"
"  movea.l 16(a0),a5\n"
"  move.l 20(a0),d1\n"
"  tst.l d0\n"
"  beq.s 9f\n"
"  1:" MC_ZERO_LIMIT_EXIT "move.l d1,d2\n"
"  2:tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  move.b (a5),d1\n"
"  bpl.s 4f\n"
"  move.b d1,24(a0)\n"
"  bra.s 9f\n"
"  4:move.l 20(a0),d1\n"
"  subq.l #1,d2\n"
"  bne.s 2b\n"
"  bra.s 9f\n"
"  3:move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  subq.l #1,d0\n"
"  beq.s 9f\n"
"  tst.b (a2)\n"
"  bmi.s 3b\n"
"  bra.s 1b\n"
"  9:move.l a4,8(a0)\n"
"  move.l d0,12(a0)\n"
"  movem.l (sp)+,d2/a2-a5\n"
"  rts\n"
"  .globl _movem16_in\n"
"  _movem16_in:\n"
"  movem.l d2-d6/a2-a5,-(sp)\n"
"  movem.l (a0),a2-a4\n"
"  move.l 12(a0),d0\n"
"  movea.l 16(a0),a5\n"
"  move.l 20(a0),d1\n"
"  tst.l d0\n"
"  beq.s 9f\n"
"  1:" MC_ZERO_LIMIT_EXIT "move.l d1,d2\n"
"  2:tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  move.b (a5),d1\n"
"  bpl.s 4f\n"
"  move.b d1,24(a0)\n"
"  bra.s 9f\n"
"  4:move.l 20(a0),d1\n"
"  subq.l #1,d2\n"
"  bne.s 2b\n"
"  bra.s 9f\n"
"  3:movem.l (a3),d3-d4\n"
"  movem.l (a3),d5-d6\n"
"  movem.l d3-d6,(a4)\n"
"  lea 16(a4),a4\n"
"  subq.l #1,d0\n"
"  beq.s 9f\n"
"  tst.b (a2)\n"
"  bmi.s 3b\n"
"  bra.s 1b\n"
"  9:move.l a4,8(a0)\n"
"  move.l d0,12(a0)\n"
"  movem.l (sp)+,d2-d6/a2-a5\n"
"  rts\n"
"  .globl _rom16_in_ready\n"
"  _rom16_in_ready:\n"
"  movem.l d2/a2-a5,-(sp)\n"
"  movem.l (a0),a2-a4\n"
"  movea.l 16(a0),a5\n"
"  move.l 20(a0),d1\n"
"  bra.s 3f\n"
"  .globl _rom16_in\n"
"  _rom16_in:\n"
"  movem.l d2/a2-a5,-(sp)\n"
"  movem.l (a0),a2-a4\n"
"  movea.l 16(a0),a5\n"
"  move.l 20(a0),d1\n"
"  1:" MC_ZERO_LIMIT_EXIT "move.l d1,d2\n"
"  2:tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  tst.b (a2)\n"
"  bmi.s 3f\n"
"  move.b (a5),d1\n"
"  bpl.s 4f\n"
"  move.b d1,24(a0)\n"
"  bra.s 9f\n"
"  4:move.l 20(a0),d1\n"
"  subq.l #1,d2\n"
"  bne.s 2b\n"
"  bra.s 9f\n"
"  3:move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  move.l (a3),(a4)+\n"
"  tst.b (a2)\n"
"  bmi.s 3b\n"
"  bra.s 1b\n"
"  9:move.l a4,8(a0)\n"
"  movem.l (sp)+,d2/a2-a5\n"
"  rts\n"
#endif
);
#undef MC_ZERO_LIMIT_EXIT

/* The NCR 53CF94-2 requires DMA NOP before loading the three-byte counter.
   esp_exec keeps ENF set for the complete experimental command so ordinary
   commands retain their original zero-means-64-KiB behavior. */
static void load_count(struct MC *mc, ULONG n, int extended)
{
    if (extended)
        wr(mc, R_CMD, C_DMA | C_NOP);
    wr(mc, R_TCLO, n);
    wr(mc, R_TCMID, n >> 8);
    if (extended)
        wr(mc, R_TCHI, n >> 16);
}

static ULONG current_count(struct MC *mc, int extended)
{
    ULONG n = rd(mc, R_TCLO) | (ULONG)rd(mc, R_TCMID) << 8;
    if (extended)
        n |= (ULONG)rd(mc, R_TCHI) << 16;
    return n;
}

#ifdef IRQXFER
/* Interrupt-driven data phases. The task starts the chip's DMA and sleeps;
   the board raises INT2 while it holds data (or room), and the server moves
   it, 16 bytes per flag, until the flag stays clear for 8 tests. The chip's
   interrupt at the end finishes the chunk and wakes the task. Interrupts
   above INT2 (audio, vertical blank, serial) keep running throughout, which
   the polled transfers, done with interrupts off, don't allow. */

/* Moves what the board offers now; returns the bytes moved.  A $C039 sample
   cannot authorize a 16-byte synchronous burst: with offset four, hardware
   tests found the first longword correct and corruption immediately after it.
   Consume at most the negotiated offset, rounded down to four bytes and
   capped at twelve, from each fresh $C039 sample.  This preserves the proven
   offset-four path while testing whether larger offsets safely authorize two
   or three longwords.  Fall back to the word-ready path because a synchronous
   target may have fewer than four REQs outstanding.  Leaving a ready word on
   the board holds INT2 asserted and traps the machine in the level-2 chain. */
static ULONG irq_move(struct MC *mc, UBYTE *sampled_status)
{
    struct Xfer *x = &mc->x;
    volatile UBYTE *f16 = mc->board + MC_DREQ16, *fw = mc->board + MC_DREQW;
    volatile ULONG *pl = (volatile ULONG *)(mc->board + MC_PORT);
    volatile UWORD *pw = (volatile UWORD *)(mc->board + MC_PORT);
    ULONG start = x->left, n;
    UBYTE ready16;

    if (!x->sync && x->left >= 16 && (*f16 & 0x80)) {
        struct Fast f;
        f.flag = f16;
        f.port = (volatile ULONG *)(mc->board + MC_PORT);
        f.buf = x->p;
        f.bursts = x->left >> 4;
        f.stat = &REG(mc, R_STAT);
        /* Zero is a ready-entry-only mode: consume continuous DREQ, then
           return on its first clear sample instead of bridging a refill gap. */
        f.limit = MC_IRQ_ROUNDS;        /* otherwise, rounds of 8 flag tests */
        f.status = 0;
#if MC_DIAG_MODES
        if (x->in && x->rom)
            rom16_in_ready(&f);
        else
#endif
        if (x->in)
            fast16_in_ready(&f);
        else
            fast16_out_ready(&f);
        if (f.status & ST_INT)
            *sampled_status = f.status;
        x->st.bursts += ((UBYTE *)f.buf - x->p) >> 4;
        x->left -= (UBYTE *)f.buf - x->p;
        x->p = (UBYTE *)f.buf;
    }
    while (x->left) {
        ready16 = *f16;
        if (x->sync >= 4 && x->left >= 4 && (ready16 & 0x80)) {
            n = x->sync & ~3;
            if (n > 12)
                n = 12;
            if (n > x->left)
                n = x->left & ~3;
            do {
                if (x->in)
                    *(ULONG *)x->p = *pl;
                else
                    *pl = *(ULONG *)x->p;
                x->p += 4;
                x->left -= 4;
                x->st.longs++;
                n -= 4;
            } while (n);
            continue;
        }
        if (!((ready16 | *fw) & 0x80))
            break;
        if (x->left >= 2) {
            if (x->in)
                *(UWORD *)x->p = *pw;
            else
                *pw = *(UWORD *)x->p;
            x->p += 2;
            x->left -= 2;
        } else {                        /* the last byte of an odd length */
            if (x->in)
                *x->p = *pw >> 8;
            else
                *pw = (UWORD)*x->p << 8;
            x->p++;
            x->left = 0;
        }
        x->st.words++;
    }
    return start - x->left;
}

/* The chip is done with the chunk: settle the count like dma_phase does,
   latch the registers and wake the task. */
static void irq_finish(struct MC *mc, UBYTE st)
{
    struct Xfer *x = &mc->x;
    ULONG rest;
    UBYTE fifo, live;

    if (x->in)
        x->st.drained += drain(mc, 4096);
    mc->istat = st;
    mc->iseq = rd(mc, R_SEQ);
    rest = current_count(mc, x->extended);
    fifo = rd(mc, R_FFLAGS) & 0x1F;
    x->st.tcleft = rest;
    x->st.fflags = fifo;
    /* In synchronous Data-In, FFLAGS was latched when the phase began and is
       not a live FIFO count.  In particular, do not use it to read the FIFO;
       synchronous payload must cross the DMA/DACK port. */
    if (x->sync && x->in)
        fifo = 0;
    if (!x->extended && !rest && x->n == 0x10000 && !(st & ST_TC))
        rest = x->n;
    x->moved = x->n - rest;
    if (x->in) {
        UBYTE *end = x->p - (x->n - x->left) + x->moved;
        while (fifo-- && x->p < end) {
            *x->p++ = rd(mc, R_FIFO);
            x->st.fifo++;
        }
    } else if (fifo) {
        x->moved -= fifo;
        x->st.fifo += fifo;
        wr(mc, R_CMD, C_FLUSH);
    }
    x->st.dreq16 = mc->board[MC_DREQ16];
    x->st.dreq8 = mc->board[MC_DREQ8];
    x->st.dreqw = mc->board[MC_DREQW];
    mc->iintr = rd(mc, R_INTR);
    live = st;
    /* The pre-INTR status belongs to the completed transfer.  Reading INTR
       opens the phase latch when CFG2.ENF is set and, in either mode, a fresh
       read observes the phase for the target's next REQ.  Sync Data-In needs
       that next phase rather than the terminating Data-In sample. */
    if (x->sync)
        live = rd(mc, R_STAT);
    mc->istat = live;
    x->st.stat = st;
    x->st.stat_live = live;
    x->st.intr = mc->iintr;
    x->st.seq = mc->iseq;
    x->active = 0;
    mc->ipending = 1;
    Signal(mc->task, mc->irqsig);
}

/* The server while a transfer is running. Claims INT2 only if it moved data
   or the chip interrupted, so other INT2 sources (CIA-A) still get served. */
static LONG irq_xfer(struct MC *mc)
{
    ULONG moved;
    UBYTE st = 0;
    mc->x.st.irqs++;
    moved = irq_move(mc, &st);
    if (!(st & ST_INT))
        st = rd(mc, R_STAT);
    if (st & ST_INT) {
        irq_finish(mc, st);
        return 1;
    }
    return moved != 0;
}

/* One chunk, moved by the server. The task sleeps until the chip is done. */
static LONG irq_phase(struct MC *mc, struct Cmd *c, struct MCStats *st)
{
    struct Xfer *x = &mc->x;
    ULONG left = c->len - c->actual;
    int extended = c->mode == MCMODE_EXT24;
    int rom = c->mode == MCMODE_ROM16;
    ULONG max = extended ? MAX_EXT_PHASE : 0x10000;
    ULONG n = left > max ? max : left;

    memset(&x->st, 0, sizeof x->st);
    x->p = c->data + c->actual;
    x->left = n;
    x->n = n;
    x->in = c->in;
    x->extended = extended;
    x->rom = rom;
    x->sync = c->sync_offset;
    x->moved = 0;
    load_count(mc, n, extended);
    mc->ipending = 0;
    x->st.chunks = 1;
    x->active = 1;
    wr(mc, R_CMD, C_DMA | C_TI);
    if (!wait_int(mc, 30)) {
        Disable();
        x->active = 0;
        wr(mc, R_CMD, C_RESET);         /* drops the board's request */
        wr(mc, R_CMD, C_NOP);
        (void)rd(mc, R_INTR);
        Enable();
        return HFERR_DMA;
    }
    mc->ipending = 1;                   /* esp_exec goes on from the latched registers */
    c->actual += x->moved;
    st->bursts += x->st.bursts;
    st->longs += x->st.longs;
    st->words += x->st.words;
    st->fifo += x->st.fifo;
    st->chunks += x->st.chunks;
    st->drained += x->st.drained;
    st->irqs += x->st.irqs;
    st->tcleft = x->st.tcleft;
    st->dreq16 = x->st.dreq16;
    st->dreq8 = x->st.dreq8;
    st->dreqw = x->st.dreqw;
    st->fflags = x->st.fflags;
    st->stat = x->st.stat;
    st->stat_live = x->st.stat_live;
    st->intr = x->st.intr;
    st->seq = x->st.seq;
    return chip_error(x->st.stat);
}
#endif

#if !defined(IRQXFER) || MC_DIAG_MODES   /* an IRQ ROM moves all data in the server */
/* Moves one DMA chunk of a data phase. Runs disabled; ends at the chip's
   interrupt, whose registers it latches like mc_irq would.
   MCMODE_DMA16: 16 bytes each time $C039 says so, then words.
   MCMODE_DMA8:  8 bytes each time $C839 says so, then words.
   MCMODE_WORD:  one word each time $D039 says so. */
static LONG dma_phase(struct MC *mc, struct Cmd *c, struct MCStats *st)
{
    volatile UBYTE *dreqb = mc->board + (c->mode == MCMODE_DMA8 ? MC_DREQ8 : MC_DREQ16);
    volatile UBYTE *dreqw = mc->board + MC_DREQW;
    volatile ULONG *pl = (volatile ULONG *)(mc->board + MC_PORT);
    volatile UWORD *pw = (volatile UWORD *)(mc->board + MC_PORT);
    ULONG burst = c->mode == MCMODE_DMA8 ? 8 : c->mode == MCMODE_WORD ? 0x7FFFFFFF : 16;
    ULONG left = c->len - c->actual, n, spin = 0;
    UBYTE *p = c->data + c->actual, st_ = 0;
    UBYTE in = c->in;
    int extended = c->mode == MCMODE_EXT24;
    ULONG max = extended ? MAX_EXT_PHASE : 0x10000;

    n = left > max ? max : left;
    left = n;
    load_count(mc, n, extended);
    st->chunks++;
    Disable();
    wr(mc, R_CMD, C_DMA | C_TI);
    if (c->mode != MCMODE_DMA8 && c->mode != MCMODE_WORD && left >= 16) {
        /* The bulk of it in assembly: 16 bytes per flag, like the original.
           DMA32 and MOVEM are read comparisons; their writes are DMA16. */
        struct Fast f;
        UBYTE sh = in && c->mode == MCMODE_DMA32 ? 5 : 4;
        f.flag = dreqb;
        f.port = pl;
        f.buf = p;
        f.bursts = left >> sh;
        f.stat = &REG(mc, R_STAT);
        f.limit = SPIN_LIMIT / 8;      /* 8 flag tests per round */
        f.status = 0;
        if (!in)
            fast16_out(&f);
#if MC_DIAG_MODES
        else if (c->mode == MCMODE_DMA32)
            fast32_in(&f);
        else if (c->mode == MCMODE_MOVEM)
            movem16_in(&f);
        else if (c->mode == MCMODE_ROM16)
            rom16_in(&f);
#endif
        else
            fast16_in(&f);
        st_ = f.status;
        st->bursts += ((UBYTE *)f.buf - p) >> sh;
        left -= (UBYTE *)f.buf - p;
        p = (UBYTE *)f.buf;
    }
    for (;;) {
        if (st_ & ST_INT)
            break;
        if (left >= burst && (*dreqb & 0x80)) {
            ULONG *q = (ULONG *)p;
            if (in) {
                q[0] = *pl; q[1] = *pl;
                if (burst == 16) { q[2] = *pl; q[3] = *pl; }
            } else {
                *pl = q[0]; *pl = q[1];
                if (burst == 16) { *pl = q[2]; *pl = q[3]; }
            }
            p += burst; left -= burst;
            st->bursts++;
        } else if (left >= 2 && (left < burst || burst > 16) &&
                   ((burst > 16 ? 0 : *dreqb) | *dreqw) & 0x80) {
            if (in)
                *(UWORD *)p = *pw;
            else
                *pw = *(UWORD *)p;
            p += 2; left -= 2;
            st->words++;
        } else if (left == 1 && ((burst > 16 ? 0 : *dreqb) | *dreqw) & 0x80) {
            /* The last byte of an odd length: the board moves words, and the
               chip waits until the whole word is taken. The byte is the high
               half. */
            if (in)
                *p = *pw >> 8;
            else
                *pw = (UWORD)*p << 8;
            p++; left = 0;
            st->words++;
        } else {
            if ((st_ = rd(mc, R_STAT)) & ST_INT)
                break;
            if (++spin > SPIN_LIMIT)
                break;
            continue;
        }
        if (spin > st->maxpolls)
            st->maxpolls = spin;
        spin = 0;
    }
    st->dreq16 = mc->board[MC_DREQ16];
    st->dreq8 = mc->board[MC_DREQ8];
    st->dreqw = *dreqw;
    if (!(st_ & ST_INT)) {
        /* The data port never answered. A pending DMA request holds INT2,
           which nothing would clear once interrupts are on again, so the
           chip is reset first; the caller resets the bus. */
        st->stat = rd(mc, R_STAT);
        st->stat_live = st->stat;
        st->fflags = rd(mc, R_FFLAGS);
        wr(mc, R_CMD, C_RESET);
        wr(mc, R_CMD, C_NOP);
        (void)rd(mc, R_INTR);
        Enable();
        return HFERR_DMA;
    }
    /* The chip is done; the board may still hold the last words of a read,
       as when the target sent less than asked. Move them while there is
       room, then drop the rest, as the original driver does. */
    if (in) {
        while (((*dreqw | *dreqb) & 0x80) && left) {
            UWORD w = *pw;
            if (left >= 2) {
                *(UWORD *)p = w;
                p += 2; left -= 2;
            } else {
                *p++ = w >> 8;
                left = 0;
            }
            st->words++;
        }
        st->drained += drain(mc, 4096);
    }
    mc->istat = st_;
    mc->iseq = rd(mc, R_SEQ);
    {
        /* What the chip moved on the bus: n minus the transfer counter. */
        ULONG rest = current_count(mc, extended);
        UBYTE fifo = rd(mc, R_FFLAGS) & 0x1F;
        UBYTE live_fifo = c->sync_offset && in ? 0 : fifo;
        ULONG moved;
        st->fflags = fifo;
        st->tcleft = rest;
        /* The counter is what counts; the TC bit is only checked for 64 KB,
           which loads as 0. (Emulators set the TC bit too early.) */
        if (!extended && !rest && n == 0x10000 && !(st_ & ST_TC))
            rest = n;                   /* none of it moved */
        moved = n - rest;
        if (in) {
            /* A last odd byte stays in the chip's FIFO. */
            while (live_fifo-- && p < c->data + c->actual + moved) {
                *p++ = rd(mc, R_FIFO);
                st->fifo++;
            }
        } else if (live_fifo) {
            moved -= live_fifo;         /* sent to the FIFO, not taken by the target */
            st->fifo += live_fifo;
            wr(mc, R_CMD, C_FLUSH);
        }
        c->actual += moved;
    }
    mc->iintr = rd(mc, R_INTR);
    if (c->sync_offset)
        mc->istat = rd(mc, R_STAT);      /* INTR reopened the S2FE phase latch */
    st->stat = st_;
    st->stat_live = mc->istat;
    st->intr = mc->iintr;
    st->seq = mc->iseq;
    mc->ipending = 1;
    Enable();
    return chip_error(st_);
}
#endif

#if MC_DIAG_MODES
/* The data phase through the chip's FIFO, without DMA: Transfer Information
   moves what fits in the FIFO, the chip interrupts, the CPU empties or fills
   it, and again until the target changes phase. Interrupts stay on. */
static LONG pio_phase(struct MC *mc, struct Cmd *c, struct MCStats *st)
{
    UBYTE want = c->in ? PH_DATAIN : PH_DATAOUT;
    while ((mc->istat & ST_PHASE) == want && c->actual < c->len) {
        ULONG left = c->len - c->actual;
        UBYTE k = left > 16 ? 16 : left, j, fifo;
        if (!c->in) {
            wr(mc, R_CMD, C_FLUSH);
            for (j = 0; j < k; j++)
                wr(mc, R_FIFO, c->data[c->actual + j]);
        }
        st->chunks++;
        wr(mc, R_CMD, C_TI);
        if (!wait_int(mc, 5))
            return HFERR_Phase;
        {
            LONG err = chip_error(mc->istat);
            if (err)
                return err;
        }
        fifo = rd(mc, R_FFLAGS) & 0x1F;
        st->fflags = fifo;
        if (c->in) {
            while (fifo-- && c->actual < c->len) {
                c->data[c->actual++] = rd(mc, R_FIFO);
                st->fifo++;
            }
        } else {
            c->actual += k - fifo;      /* what the target took */
            st->fifo += k;
        }
        st->stat = mc->istat;
        st->stat_live = mc->istat;
        st->intr = mc->iintr;
        st->seq = mc->iseq;
        if (mc->iintr & IN_DISC)
            break;
    }
    if (c->in) {                        /* bytes left in the FIFO at the phase change */
        UBYTE fifo = rd(mc, R_FFLAGS) & 0x1F;
        while (fifo-- && c->actual < c->len) {
            c->data[c->actual++] = rd(mc, R_FIFO);
            st->fifo++;
        }
    }
    mc->ipending = 1;                   /* esp_exec goes on from the latched registers */
    return 0;
}

#endif

static LONG data_phase(struct MC *mc, struct Cmd *c)
{
    struct MCStats dummy, *st = c->st ? c->st : &dummy;
    if (c->actual >= c->len)
        return HFERR_Phase;             /* the target wants more than we have */
#if MC_DIAG_MODES
    if (c->mode == MCMODE_PIO)
        return pio_phase(mc, c, st);
#endif
#ifdef IRQXFER
#if MC_DIAG_MODES
    if (c->mode == MCMODE_DMA16 || c->mode == MCMODE_ROM16 || c->mode == MCMODE_EXT24)
#endif
        return irq_phase(mc, c, st);    /* the other modes stay polled, for MCDiag */
#endif
#if !defined(IRQXFER) || MC_DIAG_MODES
    return dma_phase(mc, c, st);
#endif
}

/* A target may remain in synchronous Data-In after the requested buffer is
   full. Transfer Pad is the controller-defined recovery: it consumes and
   discards input internally until the target changes phase, without exposing
   any additional bytes to the caller's buffer. */
static LONG pad_phase(struct MC *mc, struct Cmd *c, int extended)
{
    struct MCStats *st = c->st;
    UBYTE latched, live;
    LONG err;

    load_count(mc, 0x10000, extended);  /* bounded pad; phase change ends it early */
    mc->ipending = 0;
    wr(mc, R_CMD, C_DMA | C_PAD);
    if (!wait_int(mc, 5))
        return HFERR_Phase;
    latched = mc->istat;
    err = chip_error(latched);
    live = rd(mc, R_STAT);              /* INTR was read by mc_irq */
    mc->istat = live;
    if (st) {
        st->chunks++;
        st->pads++;
        st->tcleft = current_count(mc, extended);
        st->fflags = rd(mc, R_FFLAGS) & 0x1F;
        st->stat = latched;
        st->stat_live = live;
        st->intr = mc->iintr;
        st->seq = mc->iseq;
        st->dreq16 = mc->board[MC_DREQ16];
        st->dreq8 = mc->board[MC_DREQ8];
        st->dreqw = mc->board[MC_DREQW];
    }
    return err ? err : chip_error(live);
}

#if MC_SYNC_DIAG
static LONG sync_failed(struct MC *mc, struct MCSyncReq *r, UBYTE step, LONG err)
{
    r->step = step;
    r->stat = mc->istat;
    r->intr = mc->iintr;
    r->seq = mc->iseq;
    r->err = err;
    esp_busreset(mc);
    return err;
}

/* Negotiate one conservative SDTR, followed by TEST UNIT READY. This is used
   only by MCDiag. A zero requested offset resets the bus and returns every
   target to asynchronous operation. */
LONG esp_sync(struct MC *mc, UBYTE target, UBYTE lun, struct MCSyncReq *r)
{
    UBYTE msg[8], n = 0, k, status, clocks = 0;
    LONG err;

    r->period = r->offset = r->chip_period = r->step = 0;
    r->stat = r->intr = r->seq = 0;
    r->err = 0;
    if (!r->ask_offset) {
        esp_busreset(mc);
        return 0;
    }
    mc->ipending = 0;
    wr(mc, R_CFG3, CFG3_FASTCLK);        /* SDTR itself starts asynchronously */
    wr(mc, R_SYNCPER, 0);
    wr(mc, R_SYNCOFF, 0);
    wr(mc, R_CMD, C_FLUSH);
    wr(mc, R_DESTID, target);
    wr(mc, R_FIFO, 0x80 | lun);         /* IDENTIFY, no disconnection */
    wr(mc, R_CMD, C_SELATNS);
    if (!wait_int(mc, 2))
        return sync_failed(mc, r, 1, HFERR_SelTimeout);
    if ((err = chip_error(mc->istat)) != 0 || (mc->iintr & IN_DISC))
        return sync_failed(mc, r, 2, err ? err : HFERR_SelTimeout);
    if ((mc->istat & ST_PHASE) != PH_MSGOUT)
        return sync_failed(mc, r, 3, HFERR_Phase);

    wr(mc, R_CMD, C_FLUSH);
    wr(mc, R_FIFO, 0x01);               /* EXTENDED MESSAGE, SDTR */
    wr(mc, R_FIFO, 0x03);
    wr(mc, R_FIFO, 0x01);
    wr(mc, R_FIFO, r->ask_period);
    wr(mc, R_FIFO, r->ask_offset);
    wr(mc, R_CMD, C_TI);
    if (!wait_int(mc, 5))
        return sync_failed(mc, r, 4, HFERR_Phase);
    if ((err = chip_error(mc->istat)) != 0 || (mc->iintr & IN_DISC))
        return sync_failed(mc, r, 5, err ? err : HFERR_Phase);

    while ((mc->istat & ST_PHASE) == PH_MSGIN && n < sizeof msg) {
        wr(mc, R_CMD, C_FLUSH);
        wr(mc, R_CMD, C_TI);
        if (!wait_int(mc, 5))
            return sync_failed(mc, r, 6, HFERR_Phase);
        if ((err = chip_error(mc->istat)) != 0 || (mc->iintr & IN_DISC))
            return sync_failed(mc, r, 7, err ? err : HFERR_Phase);
        msg[n++] = rd(mc, R_FIFO);
        wr(mc, R_CMD, C_MSGACC);
        if (!wait_int(mc, 5))
            return sync_failed(mc, r, 8, HFERR_Phase);
        if ((err = chip_error(mc->istat)) != 0 || (mc->iintr & IN_DISC))
            return sync_failed(mc, r, 9, err ? err : HFERR_Phase);
    }
    if ((mc->istat & ST_PHASE) == PH_MSGIN)
        return sync_failed(mc, r, 10, HFERR_Phase);
    if (n == 5 && msg[0] == 1 && msg[1] == 3 && msg[2] == 1) {
        r->period = msg[3];
        r->offset = msg[4];
        if (!r->period || r->period < r->ask_period || r->offset > r->ask_offset || r->offset > 15)
            return sync_failed(mc, r, 11, HFERR_Phase);
        if (r->offset) {
            clocks = (r->period * 4 + 24) / 25; /* 40 MHz CF94 clock */
            if (clocks < 4)
                clocks = 4;
            r->chip_period = clocks;
        }
    } else if (!(n == 1 && msg[0] == 0x07)) { /* MESSAGE REJECT means async */
        return sync_failed(mc, r, 12, HFERR_Phase);
    }
    if ((mc->istat & ST_PHASE) != PH_CMD)
        return sync_failed(mc, r, 13, HFERR_Phase);

    wr(mc, R_CFG3, CFG3_FASTCLK |
       (r->offset && r->chip_period < 8 ? CFG3_FASTSCSI : 0));
    wr(mc, R_SYNCPER, r->chip_period);
    wr(mc, R_SYNCOFF, r->offset);
    wr(mc, R_CMD, C_FLUSH);
    for (k = 0; k < 6; k++)
        wr(mc, R_FIFO, 0);              /* TEST UNIT READY */
    wr(mc, R_CMD, C_TI);
    if (!wait_int(mc, 5))
        return sync_failed(mc, r, 14, HFERR_Phase);
    if ((err = chip_error(mc->istat)) != 0 || (mc->iintr & IN_DISC) ||
        (mc->istat & ST_PHASE) != PH_STATUS)
        return sync_failed(mc, r, 15, err ? err : HFERR_Phase);
    wr(mc, R_CMD, C_ICCS);
    if (!wait_int(mc, 5))
        return sync_failed(mc, r, 16, HFERR_Phase);
    if ((err = chip_error(mc->istat)) != 0)
        return sync_failed(mc, r, 17, err);
    status = rd(mc, R_FIFO);
    (void)rd(mc, R_FIFO);
    wr(mc, R_CMD, C_MSGACC);
    if (!wait_int(mc, 5))
        return sync_failed(mc, r, 18, HFERR_Phase);
    if ((err = chip_error(mc->istat)) != 0 || !(mc->iintr & IN_DISC))
        return sync_failed(mc, r, 19, err ? err : HFERR_Phase);
    if (status)
        return sync_failed(mc, r, 20, HFERR_BadStatus);
    r->stat = mc->istat;
    r->intr = mc->iintr;
    r->seq = mc->iseq;
    return 0;
}
#endif

LONG esp_exec(struct MC *mc, UBYTE target, UBYTE lun, struct Cmd *c)
{
    UWORD k;
    int phases = 0;
    int pads = 0;
    int extended = c->mode == MCMODE_EXT24;
    int manual_cdb = c->cdblen > 12;
    LONG err = 0;

    c->actual = 0;
    c->status = 0xFF;
    mc->ipending = 0;
    /* ENF also changes status-phase latching.  Keep it stable for the whole
       SCSI command; changing CFG2 between data and status poisoned the next
       command on the real CF94 even though the data itself was correct. */
    if (extended)
        wr(mc, R_CFG2, CFG2_NORMAL | CFG2_ENF);
    wr(mc, R_CFG3, CFG3_FASTCLK |
       (c->sync_offset && c->sync_period < 8 ? CFG3_FASTSCSI : 0));
    wr(mc, R_CMD, C_FLUSH);
    wr(mc, R_DESTID, target);
    wr(mc, R_SYNCPER, c->sync_period);
    wr(mc, R_SYNCOFF, c->sync_offset);
    wr(mc, R_FIFO, 0x80 | lun);         /* IDENTIFY, no disconnection */
    if (manual_cdb) {
        /* IDENTIFY plus a 16-byte CDB cannot fit in the 16-byte FIFO, and
           CF94 selection steps treat group-4 commands as six bytes.  Stop
           after IDENTIFY and send the CDB explicitly in command phase. */
        wr(mc, R_CMD, C_SELATNS);
    } else {
        for (k = 0; k < c->cdblen; k++)
            wr(mc, R_FIFO, c->cdb[k]);
        wr(mc, R_CMD, C_SELATN);
    }
    if (!wait_int(mc, 2)) {
        esp_init(mc);
        return HFERR_SelTimeout;
    }
    if (mc->iintr & IN_DISC) {
        wr(mc, R_CMD, C_FLUSH);
        if (extended)
            wr(mc, R_CFG2, CFG2_NORMAL);
        return HFERR_SelTimeout;        /* nobody answered */
    }
    for (;;) {
        UBYTE intr = mc->iintr, st = mc->istat;
        if ((err = chip_error(st))) {
            esp_busreset(mc);
            return err;
        }
        if (intr & IN_DISC) {
            if (extended)
                wr(mc, R_CFG2, CFG2_NORMAL);
            return err ? err : HFERR_Phase; /* gone before the status phase */
        }
        if (intr & (IN_ILLEGAL | IN_RESET) || ++phases > MAX_PHASES) {
            esp_init(mc);
            return HFERR_Phase;
        }
        switch (st & ST_PHASE) {
        case PH_DATAOUT:
        case PH_DATAIN:
            if ((st & ST_PHASE) != (c->in ? PH_DATAIN : PH_DATAOUT)) {
                err = HFERR_Phase;      /* wrong direction or too much: give up */
                esp_busreset(mc);
                return err;
            }
            if (c->actual >= c->len) {
                /* This is the standard 53C9x zero-residual data-phase case.
                   Keep it diagnostic-only until sync is proven harmless. */
                if (!c->sync_offset || !c->in || ++pads > 1 ||
                    (err = pad_phase(mc, c, extended))) {
                    if (!err)
                        err = HFERR_Phase;
                    esp_busreset(mc);
                    return err;
                }
                continue;
            }
            if ((err = data_phase(mc, c))) {
                esp_busreset(mc);
                return err;
            }
            mc->ipending = 0;           /* data_phase latched the registers */
            continue;
        case PH_STATUS:
            wr(mc, R_CMD, C_ICCS);
            if (!wait_int(mc, 5))
                goto stuck;
            if ((err = chip_error(mc->istat)))
                goto chip_failed;
            c->status = rd(mc, R_FIFO);
            (void)rd(mc, R_FIFO);       /* COMMAND COMPLETE */
            wr(mc, R_CMD, C_MSGACC);
            if (!wait_int(mc, 5))
                goto stuck;
            if ((err = chip_error(mc->istat)))
                goto chip_failed;
            if (extended)
                wr(mc, R_CFG2, CFG2_NORMAL);
            return 0;
        case PH_MSGIN:
            wr(mc, R_CMD, C_TI);
            if (!wait_int(mc, 5))
                goto stuck;
            if ((err = chip_error(mc->istat)))
                goto chip_failed;
            (void)rd(mc, R_FIFO);
            wr(mc, R_CMD, C_MSGACC);
            break;
        case PH_MSGOUT:
            wr(mc, R_CMD, C_FLUSH);
            wr(mc, R_FIFO, 0x08);       /* NO OPERATION */
            wr(mc, R_CMD, C_TI);
            break;
        case PH_CMD:
            if (!manual_cdb) {
                esp_busreset(mc);
                return HFERR_Phase;
            }
            wr(mc, R_CMD, C_FLUSH);
            for (k = 0; k < c->cdblen; k++)
                wr(mc, R_FIFO, c->cdb[k]);
            manual_cdb = 0;
            wr(mc, R_CMD, C_TI);
            break;
        default:                        /* reserved phase */
            esp_busreset(mc);
            return HFERR_Phase;
        }
        if (!wait_int(mc, 30))
            goto stuck;
    }
chip_failed:
    esp_busreset(mc);
    return err;
stuck:
    esp_busreset(mc);
    return HFERR_Phase;
}
