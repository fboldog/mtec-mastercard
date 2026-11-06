/* mcdiag.h - the private test command of mastercard.device, for MCDiag.
 *
 * MCCMD_DIAG runs one SCSI command with a chosen way of moving the data
 * phase, and reports what the board's data port flags did. io_Data points to
 * a struct MCDiagReq and io_Length is its size. io_Error is 0; the result is
 * in the request.
 */
#ifndef MCDIAG_H
#define MCDIAG_H

#include <exec/types.h>

#define MCCMD_DIAG    0x7FF1
#define MCCMD_SYNC    0x7FF3     /* private: negotiate sync, or reset to async */

#define MCMODE_DMA16  0     /* 16 bytes per $C039, words at the end: the normal mode */
#define MCMODE_DMA8   1     /* 8 bytes per $C839, words at the end */
#define MCMODE_WORD   2     /* one word per $D039 */
#define MCMODE_PIO    3     /* the chip's FIFO, no DMA: the reference */
#define MCMODE_DMA32  4     /* experiment, reads: 32 bytes per $C039 */
#define MCMODE_MOVEM  5     /* comparison, reads: DMA16 with two movem.l from $F000 */
#define MCMODE_ROM16  6     /* experiment: original ROM loop, trusts the chip transfer count */
#define MCMODE_EXT24  7     /* experiment: 24-bit CF94 count, up to a 512 KiB DMA phase */
#define MCMODES       8
#define MCMODES_STD   4     /* the modes every test runs */

struct MCStats {
    ULONG bursts;           /* 16- or 8-byte moves */
    ULONG words;            /* word moves */
    ULONG fifo;             /* bytes taken from or left in the chip's FIFO */
    ULONG chunks;           /* DMA or PIO commands issued */
    ULONG pads;             /* synchronous overrun phases discarded by Transfer Pad */
    ULONG maxpolls;         /* longest wait for a flag, in polls */
    ULONG tcleft;           /* transfer counter at the end of the last chunk */
    UBYTE dreq16, dreq8, dreqw;     /* the flags after the last chunk */
    UBYTE fflags, stat, intr, seq;  /* chip registers after the last chunk */
    UBYTE stat_live;        /* status after reading INTR reopens the phase latch */
    ULONG drained;          /* words read after the chip finished */
    ULONG stray;            /* board requests the interrupt server cleared, so far */
    ULONG irqs;             /* IRQXFER: server entries during the data phase */
    ULONG sleeps;           /* waits that needed the timer: the short poll ran out */
};

struct MCDiagReq {
    UBYTE cdb[16];
    UWORD cdblen;
    UBYTE in;               /* data phase from the target */
    UBYTE mode;             /* MCMODE_ */
    UBYTE *data;
    ULONG len;
    ULONG actual;           /* bytes moved */
    LONG err;               /* 0 or HFERR_ */
    UBYTE status;           /* SCSI status, 0xFF if none */
    UBYTE pad[3];
    struct MCStats st;
};

struct MCSyncReq {
    UBYTE ask_period;       /* SCSI period factor (50 = 5 MB/s), 0 resets async */
    UBYTE ask_offset;
    UBYTE period;           /* target's reply */
    UBYTE offset;
    UBYTE chip_period;      /* value programmed into the CF94 */
    UBYTE step;             /* negotiation step which failed, 0 on success */
    UBYTE stat, intr, seq;  /* final/failing chip registers */
    UBYTE pad[3];
    LONG err;
};

#endif
