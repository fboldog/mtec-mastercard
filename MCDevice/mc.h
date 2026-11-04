/* mastercard.device - a new driver for the M-Tec MasterCard SCSI option of
 * the T1230 (Viper) A1200 accelerator. Shared definitions.
 *
 * Board (64 KB Zorro II space, manufacturer 2192, product 33):
 *   $0000-$7FFF  boot ROM, even bytes
 *   $C000-$DFFF  NCR 53CF94 registers, even bytes, register n at +n*8
 *   $C039        bit 7: the data port can move 16 bytes
 *   $C839        bit 7: the data port can move 8 bytes (not emulated)
 *   $D039        bit 7: the data port can move a word (end of a transfer)
 *   $F000        data port for the chip's DMA, any width
 * The chip's interrupt reaches INT2 (PORTS).
 */
#ifndef MC_H
#define MC_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <exec/interrupts.h>
#include <exec/lists.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include "mcdiag.h"

static inline void NewList(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = 0;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

#define MC_MANUFACTURER 2192
#define MC_PRODUCT      33

#define MC_REGS     0xC000
#define MC_DREQ16   0xC039
#define MC_DREQ8    0xC839
#define MC_DREQW    0xD039
#define MC_PORT     0xF000

/* 53CF94 registers: read / write */
#define R_TCLO     0x0
#define R_TCMID    0x1
#define R_FIFO     0x2
#define R_CMD      0x3
#define R_STAT     0x4      /* write: destination ID */
#define R_DESTID   0x4
#define R_INTR     0x5      /* write: select time-out */
#define R_TIMEOUT  0x5
#define R_SEQ      0x6      /* write: sync period */
#define R_SYNCPER  0x6
#define R_FFLAGS   0x7      /* write: sync offset */
#define R_SYNCOFF  0x7
#define R_CFG1     0x8
#define R_CLKCONV  0x9
#define R_CFG2     0xB
#define R_CFG3     0xC
#define R_CFG4     0xD
#define R_TCHI     0xE

/* Commands */
#define C_NOP      0x00
#define C_FLUSH    0x01
#define C_RESET    0x02
#define C_BUSRESET 0x03
#define C_TI       0x10
#define C_ICCS     0x11
#define C_MSGACC   0x12
#define C_SETATN   0x1A
#define C_SELATN   0x42
#define C_DMA      0x80

/* Status register */
#define ST_INT     0x80
#define ST_GE      0x40
#define ST_PE      0x20
#define ST_TC      0x10
#define ST_PHASE   0x07
#define PH_DATAOUT 0
#define PH_DATAIN  1
#define PH_CMD     2
#define PH_STATUS  3
#define PH_MSGOUT  6
#define PH_MSGIN   7

/* Interrupt register */
#define IN_RESET   0x80
#define IN_ILLEGAL 0x40
#define IN_DISC    0x20
#define IN_BS      0x10
#define IN_FC      0x08

#define OWN_ID     7
#define MAX_TARGET 7
#define MAX_LUN    8

struct MCUnit {
    struct Unit u;
    UBYTE target, lun;
    UBYTE type;             /* peripheral device type from INQUIRY */
    UBYTE removable;
    UBYTE shift;            /* log2 of the block size, 0 until known */
    UBYTE pad;
    ULONG blocks;           /* last LBA + 1, 0 if unknown or over 4G */
    ULONG changes;          /* TD_CHANGENUM */
    struct List changeints;
};

/* A data phase run by the interrupt server (IRQXFER builds). */
struct Xfer {
    UBYTE *p;               /* next byte */
    ULONG left;             /* bytes still to move in this chunk */
    ULONG n;                /* the chunk */
    ULONG moved;            /* what the chip moved, set at the end */
    UBYTE in, active, pad[2];
    struct MCStats st;
};

struct MC {
    struct Library lib;
    UWORD pad;
    BPTR seglist;
    volatile UBYTE *board;
    struct Task *task;
    struct MsgPort *port;       /* the task's request port */
    struct Task *starter;
    ULONG irqsig;
    volatile UBYTE istat, iseq, iintr, ipending;
    struct Interrupt irq;
    struct MsgPort tport;
    struct timerequest treq;
    UBYTE timer_open;
    UBYTE pad2;
    struct MCUnit *units[MAX_TARGET * MAX_LUN];
    UBYTE sense[32];
    ULONG stray;                /* board requests nobody was moving */
    ULONG sleeps;               /* waits that needed the timer */
    struct Xfer x;
};

/* One SCSI command, as esp_exec runs it. */
struct Cmd {
    UBYTE *cdb;
    UWORD cdblen;
    UBYTE in;               /* data phase is from the target */
    UBYTE status;           /* SCSI status byte */
    UBYTE *data;
    ULONG len;
    ULONG actual;
    UBYTE mode;             /* MCMODE_, 0 is the normal DMA */
    struct MCStats *st;     /* counters for MCDiag, or 0 */
};

void esp_init(struct MC *mc);
void esp_busreset(struct MC *mc);
LONG esp_exec(struct MC *mc, UBYTE target, UBYTE lun, struct Cmd *c);
void mc_delay(struct MC *mc, ULONG micros);
LONG mc_irq(struct MC *mc __asm("a1"));

extern struct ExecBase *SysBase;
extern struct Library *ExpansionBase;

#endif
