/* MTecSync - turns on synchronous SCSI for mtecscsi.device 2.3 (or 2.31).
 *
 *   MTecSync RATE 5|6|8|10   negotiate about this many MB/s
 *   MTecSync OFF             go back to asynchronous transfers
 *
 * The driver never touches the 53CF94 sync registers. A sync agreement is a
 * state kept by the target, so this runs one TEST UNIT READY by polling the
 * chip with interrupts off, sends SDTR with it, and loads the agreed values.
 * The chip keeps them, so the driver then transfers in sync mode unchanged.
 * The chip has one register set, so more than one device on the bus is refused.
 * A bus reset drops the agreement on the target; a small hook clears the
 * registers when the driver sees one. Undone by a reboot.
 */
__asm("  .text\n  jmp _start\n");

#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/devices.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <devices/scsidisk.h>

#define DEV_NAME "mtecscsi.device"

/* Offsets into the driver code and device base, from the v2.3 ROM. */
#define CODE_NAME   0x5D8
#define CODE_HOOK   0x21CE      /* move.b #$44,$18(a6) */
#define CODE_POLL   0x242A      /* movea.l $1a8(a1),a6 */
#define DEV_CHIP    0x1A8
#define DEV_TARGET  0x192       /* current target, -1 when idle */
#define DEV_ACTIVE  0x19E
#define DEV_QUEUE   0xCC

/* 53CF94 registers, 8 bytes apart. */
#define R_TCLO  0x00
#define R_TCMID 0x08
#define R_FIFO  0x10
#define R_CMD   0x18
#define R_STAT  0x20
#define R_DEST  0x20
#define R_INTR  0x28
#define R_SEQ   0x30
#define R_PERIOD 0x30
#define R_FLAGS 0x38
#define R_OFFSET 0x38

#define C_FLUSH   0x01
#define C_BUSRESET 0x03
#define C_TI      0x10
#define C_ICCS    0x11
#define C_MSGOK   0x12
#define C_SELATNS 0x43
#define C_ENSEL   0x44
#define C_DISSEL  0x45

#define INT_DONE 0x08
#define INT_BUS  0x10
#define INT_DISC 0x20

#define PH_CMD    2
#define PH_STATUS 3
#define PH_MSGOUT 6
#define PH_MSGIN  7

#define MAX_OFFSET 15
#define WAIT 3000000UL

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

#include "gen/stubs.h"

LONG start(void);

static const char vers[] __attribute__((used)) = "$VER: MTecSync 1.0 (31.10.2026)";

#define P(fmt, ...) do { ULONG a_[] = { 0, ##__VA_ARGS__ }; VPrintf((STRPTR)fmt, (LONG *)(a_ + 1)); } while (0)

__attribute__((used)) void *memset(void *d, int c, unsigned long n)
{
    UBYTE *p = d;
    while (n--) *p++ = (UBYTE)c;
    return d;
}

static volatile UBYTE *chip;
static UBYTE stat_reg, seq_reg, int_reg;

static BOOL wait_int(void)
{
    ULONG i;

    for (i = 0; i < WAIT; i++)
        if (chip[R_STAT] & 0x80) {
            stat_reg = chip[R_STAT];
            seq_reg = chip[R_SEQ];
            int_reg = chip[R_INTR];
            return TRUE;
        }
    return FALSE;
}

static UBYTE phase(void)
{
    return stat_reg & 7;
}

/* Sends the agreement request. Returns 0 when the exchange ended cleanly, with
   *period and *offset set to the agreement, or the failed step. */
static int exchange(UBYTE target, UBYTE ask_period, UBYTE ask_offset, UBYTE *period, UBYTE *offset)
{
    static const UBYTE sdtr[3] = { 0x01, 0x03, 0x01 };
    UBYTE msg[5];
    int i, n = 0;

    *period = 0;
    *offset = 0;
    chip[R_DEST] = target;
    chip[R_CMD] = C_FLUSH;
    chip[R_FIFO] = 0x80;                    /* IDENTIFY, no disconnect */
    chip[R_CMD] = C_SELATNS;
    if (!wait_int()) return 1;
    if (int_reg & INT_DISC) return 2;       /* nobody answered */
    if (phase() != PH_MSGOUT) return 3;

    chip[R_CMD] = C_FLUSH;
    for (i = 0; i < 3; i++) chip[R_FIFO] = sdtr[i];
    chip[R_FIFO] = ask_period;
    chip[R_FIFO] = ask_offset;
    chip[R_CMD] = C_TI;
    if (!wait_int()) return 4;
    if (int_reg & INT_DISC) return 5;

    while (phase() == PH_MSGIN) {
        chip[R_CMD] = C_FLUSH;
        chip[R_CMD] = C_TI;
        if (!wait_int()) return 6;
        if (int_reg & INT_DISC) return 7;
        if (n < 5) msg[n] = chip[R_FIFO];
        n++;
        chip[R_CMD] = C_MSGOK;
        if (!wait_int()) return 8;
        if (int_reg & INT_DISC) return 9;
        if (n > 5) return 10;
    }
    if (n == 5 && msg[0] == 1 && msg[1] == 3 && msg[2] == 1) {
        *period = msg[3];
        *offset = msg[4];
    }

    if (phase() != PH_CMD) return 11;
    chip[R_CMD] = C_FLUSH;
    for (i = 0; i < 6; i++) chip[R_FIFO] = 0;   /* TEST UNIT READY */
    chip[R_CMD] = C_TI;
    if (!wait_int()) return 12;
    if (phase() != PH_STATUS) return 13;

    chip[R_CMD] = C_ICCS;
    if (!wait_int()) return 14;
    chip[R_CMD] = C_MSGOK;
    if (!wait_int()) return 15;
    if (!(int_reg & INT_DISC)) return 16;
    return 0;
}

static BOOL driver_idle(UBYTE *dev)
{
    struct Node *head = *(struct Node **)(dev + DEV_QUEUE);

    return *(WORD *)(dev + DEV_TARGET) == -1 && dev[DEV_ACTIVE] == 0 &&
           head && !head->ln_Succ && !(chip[R_STAT] & 0x80);
}

/* Returns TRUE and sets the registers when the target agreed to sync. */
static BOOL negotiate(UBYTE *dev, UBYTE target, UBYTE ask_period, UBYTE ask_offset,
                      UBYTE *period, UBYTE *offset, int *step)
{
    BOOL idle = FALSE;
    int tries;

    for (tries = 0; tries < 250 && !idle; tries++) {
        Disable();
        idle = driver_idle(dev);
        if (idle) break;
        Enable();
        Delay(2);
    }
    if (!idle) {
        *step = 100;
        return FALSE;
    }
    chip[R_CMD] = C_DISSEL;
    wait_int();
    chip[R_PERIOD] = 0;
    chip[R_OFFSET] = 0;
    *step = exchange(target, ask_period, ask_offset, period, offset);
    if (*step) {
        UBYTE s = stat_reg, q = seq_reg, n = int_reg;
        chip[R_CMD] = C_BUSRESET;
        wait_int();
        stat_reg = s;
        seq_reg = q;
        int_reg = n;
        *period = 0;
        *offset = 0;
    } else if (*offset) {
        UBYTE clocks = (*period * 4 + 24) / 25;
        if (clocks < 4) clocks = 4;
        chip[R_PERIOD] = clocks;
        chip[R_OFFSET] = *offset;
    }
    chip[R_CMD] = C_FLUSH;
    chip[R_CMD] = C_ENSEL;
    Enable();
    return *step == 0 && *offset != 0;
}

/* Clears the sync registers when the interrupt that led here was a bus reset. */
static const UWORD stub[] = {
    0x0800, 0x0007,             /* btst #7,d0 */
    0x670C,                     /* beq.s +12 */
    0x1D7C, 0x0000, 0x0038,     /* move.b #0,$38(a6) */
    0x1D7C, 0x0000, 0x0030,     /* move.b #0,$30(a6) */
    0x1D7C, 0x0044, 0x0018,     /* move.b #$44,$18(a6) */
    0x4E75                      /* rts */
};

static BOOL install_hook(UBYTE *code)
{
    UWORD *h = (UWORD *)(code + CODE_HOOK);
    UWORD *s;
    ULONG i;

    if (h[0] == 0x4EB9) return TRUE;
    s = (UWORD *)AllocMem(sizeof(stub), MEMF_PUBLIC);
    if (!s) return FALSE;
    for (i = 0; i < sizeof(stub) / 2; i++) s[i] = stub[i];
    Disable();
    h[1] = (ULONG)s >> 16;
    h[2] = (UWORD)(ULONG)s;
    h[0] = 0x4EB9;
    CacheClearU();
    Enable();
    return TRUE;
}

static BOOL code_ok(UBYTE *code)
{
    UWORD *h = (UWORD *)(code + CODE_HOOK), *p = (UWORD *)(code + CODE_POLL);

    if (TypeOfMem(code) == 0) {
        P("The driver code is not in RAM\n");
        return FALSE;
    }
    if (!(h[0] == 0x4EB9 || (h[0] == 0x1D7C && h[1] == 0x0044 && h[2] == 0x0018)) ||
        p[0] != 0x2C69 || p[1] != 0x01A8) {
        P("The driver code does not match version 2.3\n");
        return FALSE;
    }
    return TRUE;
}

/* The chip has one set of sync registers, so there must be exactly one device. */
static int find_target(void)
{
    static struct MsgPort port;
    static struct IOStdReq io;
    int unit, found = -1, count = 0;

    port.mp_Node.ln_Type = NT_MSGPORT;
    port.mp_Flags = PA_SIGNAL;
    port.mp_SigBit = AllocSignal(-1);
    port.mp_SigTask = (struct Task *)FindTask(NULL);
    port.mp_MsgList.lh_Head = (struct Node *)&port.mp_MsgList.lh_Tail;
    port.mp_MsgList.lh_TailPred = (struct Node *)&port.mp_MsgList.lh_Head;
    io.io_Message.mn_ReplyPort = &port;
    io.io_Message.mn_Length = sizeof(io);
    for (unit = 0; unit < 7; unit++) {
        struct SCSICmd c;
        UBYTE cdb[6], sense[18];

        if (OpenDevice((STRPTR)DEV_NAME, unit, (struct IORequest *)&io, 0))
            continue;
        memset(&c, 0, sizeof(c));
        memset(cdb, 0, sizeof(cdb));
        c.scsi_Command = cdb;
        c.scsi_CmdLength = 6;
        c.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
        c.scsi_SenseData = sense;
        c.scsi_SenseLength = sizeof(sense);
        io.io_Command = HD_SCSICMD;
        io.io_Flags = 0;
        io.io_Data = &c;
        io.io_Length = sizeof(c);
        DoIO((struct IORequest *)&io);
        if (io.io_Error != 44) {
            found = unit;
            count++;
        }
        CloseDevice((struct IORequest *)&io);
    }
    FreeSignal(port.mp_SigBit);
    if (count > 1) {
        P("%ld devices on the bus, sync needs exactly one\n", (LONG)count);
        return -1;
    }
    if (!count) P("No device found\n");
    return found;
}

static LONG run(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rd = (struct RDArgs *)ReadArgs((STRPTR)"RATE/K/N,OFF/S", args, NULL);
    struct Library *dev;
    UBYTE *code;
    UBYTE ask = 0, off = MAX_OFFSET, period, offset;
    int target, step;
    LONG rc = RETURN_ERROR;

    if (!rd) {
        PrintFault(IoErr(), (STRPTR)"MTecSync");
        return rc;
    }
    if (args[1]) off = 0;
    if (args[1]) ask = 50;
    else if (args[0])
        switch (*(LONG *)args[0]) {
        case 5: ask = 50; break;
        case 6: ask = 37; break;
        case 8: ask = 31; break;
        case 10: ask = 25; break;
        }
    if (!ask) {
        P("Use RATE 5, RATE 6, RATE 8, RATE 10 or OFF\n");
        FreeArgs(rd);
        return rc;
    }
    Forbid();
    dev = (struct Library *)FindName(&SysBase->DeviceList, (STRPTR)DEV_NAME);
    Permit();
    if (!dev || dev->lib_Version != 2 || (dev->lib_Revision != 3 && dev->lib_Revision < 31)) {
        P(DEV_NAME " 2.3 is not running\n");
        FreeArgs(rd);
        return rc;
    }
    code = (UBYTE *)dev->lib_Node.ln_Name - CODE_NAME;
    chip = *(volatile UBYTE **)((UBYTE *)dev + DEV_CHIP);
    if (code_ok(code) && (target = find_target()) >= 0 && install_hook(code)) {
        BOOL on = negotiate((UBYTE *)dev, target, ask, off, &period, &offset, &step);
        if (step == 100) {
            P("The driver stayed busy\n");
        } else if (step) {
            P("Unit %ld: negotiation failed at step %ld (status %02lx, sequence %02lx, interrupt %02lx). Bus was reset, using async\n",
              (LONG)target, (LONG)step, (ULONG)stat_reg, (ULONG)seq_reg, (ULONG)int_reg);
        } else if (on) {
            P("Unit %ld: sync on, asked factor %ld, agreed factor %ld offset %ld, about %ld KB/s on the bus\n",
              (LONG)target, (LONG)ask, (LONG)period, (LONG)offset, 250000L / period);
            rc = RETURN_OK;
        } else {
            P("Unit %ld: async%s\n", (LONG)target, (ULONG)(off ? ", the device refused sync" : ""));
            rc = off ? RETURN_WARN : RETURN_OK;
        }
    }
    FreeArgs(rd);
    return rc;
}

LONG start(void)
{
    LONG rc = RETURN_FAIL;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
    if (DOSBase) {
        if (((struct Process *)FindTask(NULL))->pr_CLI) rc = run();
        CloseLibrary((struct Library *)DOSBase);
    }
    return rc;
}
