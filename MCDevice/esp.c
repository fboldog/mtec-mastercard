/* esp.c - the NCR 53CF94 on the MasterCard, as a SCSI initiator.
 *
 * One command at a time, no disconnection, asynchronous transfers. The chip's
 * interrupts are caught by mc_irq on INT2, which latches the status, sequence
 * step and interrupt registers and signals the driver task. Data phases go
 * through the board's data port with the chip in DMA mode: the CPU moves 16
 * bytes each time the board says it can, then words at the end. Like the
 * original driver, this runs with interrupts disabled, so the board's data
 * request can't hold INT2 while the task is not looking.
 */
#include <exec/execbase.h>
#include <exec/errors.h>
#include <devices/scsidisk.h>
#include "mc.h"
#include "gen/stubs.h"

#define REG(mc, n) ((mc)->board[MC_REGS + ((n) << 3)])
#define SPIN_LIMIT 0x00080000UL     /* polls without progress before a DMA chunk gives up, ~0.5 s */
#define MAX_PHASES 64
#define FAST_POLLS 4000             /* about 1 ms on a 50 MHz 68030 */

static inline void wr(struct MC *mc, int n, UBYTE v) { REG(mc, n) = v; }
static inline UBYTE rd(struct MC *mc, int n) { return REG(mc, n); }

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

LONG mc_irq(struct MC *mc __asm("a1"))
{
    UBYTE st = rd(mc, R_STAT);
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
    for (k = 0; k < FAST_POLLS && !mc->ipending; k++)
        ;
    if (!mc->ipending) {
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
    wr(mc, R_CFG1, 0x40 | OWN_ID);      /* no interrupt for a SCSI bus reset */
    wr(mc, R_CLKCONV, 0);               /* 40 MHz */
    wr(mc, R_TIMEOUT, 0x99);            /* 250 ms select time-out at 40 MHz */
    wr(mc, R_CFG2, 0x08);               /* SCSI-2 */
    wr(mc, R_CFG3, 0x18);               /* fast clock */
    wr(mc, R_CFG4, 0x04);               /* active negation */
    wr(mc, R_SYNCPER, 0);
    wr(mc, R_SYNCOFF, 0);               /* asynchronous */
    (void)rd(mc, R_INTR);
    mc->ipending = 0;
}

void esp_busreset(struct MC *mc)
{
    wr(mc, R_CMD, C_BUSRESET);
    mc_delay(mc, 250000);
    esp_init(mc);
    mc_delay(mc, 2000000);              /* let the devices come back */
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
};
void fast16_in(struct Fast *f __asm("a0"));     /* four move.l per flag */
void fast16_out(struct Fast *f __asm("a0"));
void fast32_in(struct Fast *f __asm("a0"));     /* MCMODE_DMA32: 32 bytes per flag */
void movem16_in(struct Fast *f __asm("a0"));    /* MCMODE_MOVEM: two movem.l per flag */
__asm(
"  .text\n"
"  .globl _fast16_in\n"
"  _fast16_in:\n"
"  movem.l d2-d6/a2-a5,-(sp)\n"
"  movem.l (a0),a2-a4\n"
"  move.l 12(a0),d0\n"
"  movea.l 16(a0),a5\n"
"  move.l 20(a0),d1\n"
"  tst.l d0\n"
"  beq.s 9f\n"
"  1:move.l d1,d2\n"
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
"  tst.b (a5)\n"
"  bmi.s 9f\n"
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
"  movem.l (sp)+,d2-d6/a2-a5\n"
"  rts\n"
"  .globl _fast16_out\n"
"  _fast16_out:\n"
"  movem.l d2-d6/a2-a5,-(sp)\n"
"  movem.l (a0),a2-a4\n"
"  move.l 12(a0),d0\n"
"  movea.l 16(a0),a5\n"
"  move.l 20(a0),d1\n"
"  tst.l d0\n"
"  beq.s 9f\n"
"  1:move.l d1,d2\n"
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
"  tst.b (a5)\n"
"  bmi.s 9f\n"
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
"  movem.l (sp)+,d2-d6/a2-a5\n"
"  rts\n"
"  .globl _fast32_in\n"
"  _fast32_in:\n"
"  movem.l d2-d6/a2-a5,-(sp)\n"
"  movem.l (a0),a2-a4\n"
"  move.l 12(a0),d0\n"
"  movea.l 16(a0),a5\n"
"  move.l 20(a0),d1\n"
"  tst.l d0\n"
"  beq.s 9f\n"
"  1:move.l d1,d2\n"
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
"  tst.b (a5)\n"
"  bmi.s 9f\n"
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
"  movem.l (sp)+,d2-d6/a2-a5\n"
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
"  1:move.l d1,d2\n"
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
"  tst.b (a5)\n"
"  bmi.s 9f\n"
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
);

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

    n = left > 0x10000 ? 0x10000 : left;
    left = n;
    wr(mc, R_TCLO, n);
    wr(mc, R_TCMID, n >> 8);            /* 0 for 64 KB */
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
        if (!in)
            fast16_out(&f);
        else if (c->mode == MCMODE_DMA32)
            fast32_in(&f);
        else if (c->mode == MCMODE_MOVEM)
            movem16_in(&f);
        else
            fast16_in(&f);
        st->bursts += (left >> sh) - f.bursts;
        left -= (UBYTE *)f.buf - p;
        p = (UBYTE *)f.buf;
    }
    for (;;) {
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
        ULONG rest = rd(mc, R_TCLO) | (ULONG)rd(mc, R_TCMID) << 8;
        UBYTE fifo = rd(mc, R_FFLAGS) & 0x1F;
        ULONG moved;
        st->fflags = fifo;
        st->tcleft = rest;
        /* The counter is what counts; the TC bit is only checked for 64 KB,
           which loads as 0. (Emulators set the TC bit too early.) */
        if (!rest && n == 0x10000 && !(st_ & ST_TC))
            rest = n;                   /* none of it moved */
        moved = n - rest;
        if (in) {
            /* A last odd byte stays in the chip's FIFO. */
            while (fifo-- && p < c->data + c->actual + moved) {
                *p++ = rd(mc, R_FIFO);
                st->fifo++;
            }
        } else if (fifo) {
            moved -= fifo;              /* sent to the FIFO, not taken by the target */
            st->fifo += fifo;
            wr(mc, R_CMD, C_FLUSH);
        }
        c->actual += moved;
    }
    mc->iintr = rd(mc, R_INTR);
    st->stat = st_;
    st->intr = mc->iintr;
    st->seq = mc->iseq;
    mc->ipending = 1;
    Enable();
    return 0;
}

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

static LONG data_phase(struct MC *mc, struct Cmd *c)
{
    struct MCStats dummy, *st = c->st ? c->st : &dummy;
    if (c->actual >= c->len)
        return HFERR_Phase;             /* the target wants more than we have */
    return c->mode == MCMODE_PIO ? pio_phase(mc, c, st) : dma_phase(mc, c, st);
}

LONG esp_exec(struct MC *mc, UBYTE target, UBYTE lun, struct Cmd *c)
{
    UWORD k;
    int phases = 0;
    LONG err = 0;

    c->actual = 0;
    c->status = 0xFF;
    mc->ipending = 0;
    wr(mc, R_CMD, C_FLUSH);
    wr(mc, R_DESTID, target);
    wr(mc, R_SYNCPER, 0);
    wr(mc, R_SYNCOFF, 0);
    wr(mc, R_FIFO, 0x80 | lun);         /* IDENTIFY, no disconnection */
    for (k = 0; k < c->cdblen; k++)
        wr(mc, R_FIFO, c->cdb[k]);
    wr(mc, R_CMD, C_SELATN);
    if (!wait_int(mc, 2)) {
        esp_init(mc);
        return HFERR_SelTimeout;
    }
    if (mc->iintr & IN_DISC) {
        wr(mc, R_CMD, C_FLUSH);
        return HFERR_SelTimeout;        /* nobody answered */
    }
    for (;;) {
        UBYTE intr = mc->iintr, st = mc->istat;
        if (intr & IN_DISC)
            return err ? err : HFERR_Phase; /* gone before the status phase */
        if (intr & (IN_ILLEGAL | IN_RESET) || ++phases > MAX_PHASES) {
            esp_init(mc);
            return HFERR_Phase;
        }
        switch (st & ST_PHASE) {
        case PH_DATAOUT:
        case PH_DATAIN:
            if ((st & ST_PHASE) != (c->in ? PH_DATAIN : PH_DATAOUT) || c->actual >= c->len) {
                err = HFERR_Phase;      /* wrong direction or too much: give up */
                esp_busreset(mc);
                return err;
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
            c->status = rd(mc, R_FIFO);
            (void)rd(mc, R_FIFO);       /* COMMAND COMPLETE */
            wr(mc, R_CMD, C_MSGACC);
            if (!wait_int(mc, 5))
                goto stuck;
            return 0;
        case PH_MSGIN:
            wr(mc, R_CMD, C_TI);
            if (!wait_int(mc, 5))
                goto stuck;
            (void)rd(mc, R_FIFO);
            wr(mc, R_CMD, C_MSGACC);
            break;
        case PH_MSGOUT:
            wr(mc, R_CMD, C_FLUSH);
            wr(mc, R_FIFO, 0x08);       /* NO OPERATION */
            wr(mc, R_CMD, C_TI);
            break;
        default:                        /* command phase again, or reserved */
            esp_busreset(mc);
            return HFERR_Phase;
        }
        if (!wait_int(mc, 30))
            goto stuck;
    }
stuck:
    esp_busreset(mc);
    return HFERR_Phase;
}
