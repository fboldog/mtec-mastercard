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
#define SPIN_LIMIT 0x00800000UL     /* polls before a data phase gives up, some seconds */
#define MAX_PHASES 64

static inline void wr(struct MC *mc, int n, UBYTE v) { REG(mc, n) = v; }
static inline UBYTE rd(struct MC *mc, int n) { return REG(mc, n); }

/* Interrupt server, called with is_Data in a1. Returns non-zero if the
   interrupt was the chip's. The asm entry turns that into the Z flag. */
LONG mc_irq(struct MC *mc __asm("a1"))
{
    UBYTE st = rd(mc, R_STAT);
    if (!(st & ST_INT))
        return 0;
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

/* Moves one DMA chunk of a data phase. Runs disabled; ends at the chip's
   interrupt, whose registers it latches like mc_irq would. */
static LONG data_phase(struct MC *mc, struct Cmd *c)
{
    volatile UBYTE *dreq16 = mc->board + MC_DREQ16, *dreqw = mc->board + MC_DREQW;
    volatile ULONG *pl = (volatile ULONG *)(mc->board + MC_PORT);
    volatile UWORD *pw = (volatile UWORD *)(mc->board + MC_PORT);
    ULONG left = c->len - c->actual, n, spin = 0;
    UBYTE *p = c->data + c->actual, st;

    if (!left)
        return HFERR_Phase;             /* the target wants more than we have */
    n = left > 0x10000 ? 0x10000 : left;
    left = n;
    wr(mc, R_TCLO, n);
    wr(mc, R_TCMID, n >> 8);            /* 0 for 64 KB */
    Disable();
    wr(mc, R_CMD, C_DMA | C_TI);
    if (c->in) {
        for (;;) {
            if (left >= 16 && (*dreq16 & 0x80)) {
                ULONG *q = (ULONG *)p;
                q[0] = *pl; q[1] = *pl; q[2] = *pl; q[3] = *pl;
                p += 16; left -= 16;
                spin = 0;
                continue;
            }
            if (left >= 2 && left < 16 && ((*dreq16 | *dreqw) & 0x80)) {
                *(UWORD *)p = *pw;
                p += 2; left -= 2;
                spin = 0;
                continue;
            }
            if ((st = rd(mc, R_STAT)) & ST_INT)
                break;
            if (++spin > SPIN_LIMIT)
                break;
        }
    } else {
        for (;;) {
            if (left >= 16 && (*dreq16 & 0x80)) {
                ULONG *q = (ULONG *)p;
                *pl = q[0]; *pl = q[1]; *pl = q[2]; *pl = q[3];
                p += 16; left -= 16;
                spin = 0;
                continue;
            }
            if (left >= 2 && left < 16 && ((*dreq16 | *dreqw) & 0x80)) {
                *pw = *(UWORD *)p;
                p += 2; left -= 2;
                spin = 0;
                continue;
            }
            if ((st = rd(mc, R_STAT)) & ST_INT)
                break;
            if (++spin > SPIN_LIMIT)
                break;
        }
    }
    if (!(st & ST_INT)) {
        Enable();
        return HFERR_DMA;
    }
    mc->istat = st;
    mc->iseq = rd(mc, R_SEQ);
    {
        /* What the chip moved on the bus: n minus the transfer counter. */
        ULONG rest = rd(mc, R_TCLO) | (ULONG)rd(mc, R_TCMID) << 8;
        UBYTE fifo = rd(mc, R_FFLAGS) & 0x1F;
        ULONG moved;
        if (st & ST_TC)
            rest = 0;                   /* all of it */
        else if (!rest && n == 0x10000)
            rest = n;                   /* 64 KB loaded as 0, none of it moved */
        moved = n - rest;
        if (c->in) {
            /* A last odd byte stays in the chip's FIFO. */
            while (fifo-- && p < c->data + c->actual + moved)
                *p++ = rd(mc, R_FIFO);
        } else if (fifo) {
            moved -= fifo;              /* sent to the FIFO, not taken by the target */
            wr(mc, R_CMD, C_FLUSH);
        }
        c->actual += moved;
    }
    mc->iintr = rd(mc, R_INTR);
    mc->ipending = 1;
    Enable();
    return 0;
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
