/* CmdTest - checks the added commands and error returns of mtecscsi.device.
 *
 *   CmdTest UNIT n     needs a disk with no medium change during the run
 *
 * Only reads, plus TD_EJECT with length 0, which means load, not eject.
 */
__asm("  .text\n  jmp _start\n");

#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/newstyle.h>
#ifndef NSCMD_ETD_READ64
#define NSCMD_ETD_READ64   0xE000
#define NSCMD_ETD_WRITE64  0xE001
#define NSCMD_ETD_SEEK64   0xE002
#define NSCMD_ETD_FORMAT64 0xE003
#endif

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

#include "gen/stubs.h"

LONG start(void);

#define P(fmt, ...) do { ULONG a_[] = { 0, ##__VA_ARGS__ }; VPrintf((STRPTR)fmt, (LONG *)(a_ + 1)); } while (0)

__attribute__((used)) void *memset(void *d, int c, unsigned long n)
{
    UBYTE *p = d;
    while (n--) *p++ = (UBYTE)c;
    return d;
}

static struct MsgPort port;
static struct IOExtTD io;
static ULONG failures;
static UBYTE buf[512];

static LONG send(UWORD cmd, void *data, ULONG len, ULONG off, ULONG hi, ULONG count)
{
    io.iotd_Req.io_Command = cmd;
    io.iotd_Req.io_Flags = 0;
    io.iotd_Req.io_Data = data;
    io.iotd_Req.io_Length = len;
    io.iotd_Req.io_Offset = off;
    io.iotd_Req.io_Actual = hi;
    io.iotd_Count = count;
    DoIO((struct IORequest *)&io);
    return io.iotd_Req.io_Error;
}

static void check(const char *name, LONG got, LONG want)
{
    P("%-34s error %ld, want %ld: %s\n", (ULONG)name, got, want, (ULONG)(got == want ? "ok" : "WRONG"));
    if (got != want) failures++;
}

/* READ(10) past the end of the disk gives CHECK CONDITION. */
static void bad_status(const char *name, UBYTE flags)
{
    struct SCSICmd c;
    UBYTE cdb[10], sense[18];
    LONG e;

    memset(&c, 0, sizeof(c));
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;
    cdb[2] = 0xFF; cdb[3] = 0xFF; cdb[4] = 0xFF;
    cdb[8] = 1;
    c.scsi_Data = (UWORD *)buf;
    c.scsi_Length = 512;
    c.scsi_Command = cdb;
    c.scsi_CmdLength = 10;
    c.scsi_Flags = SCSIF_READ | flags;
    c.scsi_SenseData = sense;
    c.scsi_SenseLength = sizeof(sense);
    e = send(HD_SCSICMD, &c, sizeof(c), 0, 0, 0);
    check(name, e, HFERR_BadStatus);
    P("  scsi_Status %ld\n", (LONG)c.scsi_Status);
}

static void timeout(void)
{
    struct SCSICmd c;
    UBYTE cdb[6];
    LONG e, unit;

    /* An empty unit: the lowest with a selection timeout. */
    for (unit = 0; unit < 7; unit++) {
        struct IOExtTD t;
        memset(&t, 0, sizeof(t));
        t.iotd_Req.io_Message.mn_ReplyPort = &port;
        t.iotd_Req.io_Message.mn_Length = sizeof(t);
        if (OpenDevice((STRPTR)"mtecscsi.device", unit, (struct IORequest *)&t, 0))
            continue;
        memset(&c, 0, sizeof(c));
        memset(cdb, 0, sizeof(cdb));
        c.scsi_Command = cdb;
        c.scsi_CmdLength = 6;
        c.scsi_Flags = SCSIF_READ;
        c.scsi_Status = 0xEE;
        t.iotd_Req.io_Command = HD_SCSICMD;
        t.iotd_Req.io_Data = &c;
        t.iotd_Req.io_Length = sizeof(c);
        DoIO((struct IORequest *)&t);
        e = t.iotd_Req.io_Error;
        CloseDevice((struct IORequest *)&t);
        if (e == HFERR_SelTimeout) {
            check("HD_SCSICMD empty unit", e, HFERR_SelTimeout);
            P("  scsi_Status %ld\n", (LONG)c.scsi_Status);
            return;
        }
    }
    P("No empty unit to test a selection timeout\n");
}


static void test(void)
{
    static struct Interrupt irq;
    struct NSDeviceQueryResult q;
    static const UWORD want[] = { CMD_STOP, CMD_START, TD_EJECT, ETD_READ, NSCMD_ETD_READ64, TD_GETGEOMETRY, 0 };
    ULONG change;
    int i;

    send(TD_CHANGENUM, NULL, 0, 0, 0, 0);
    change = io.iotd_Req.io_Actual;
    P("Change count %lu\n", change);

    check("ETD_READ", send(ETD_READ, buf, 512, 0, 0, change), 0);
    if (change)
        check("ETD_READ old count", send(ETD_READ, buf, 512, 0, 0, change - 1), TDERR_DiskChanged);
    else
        P("ETD_READ old count: change count is 0, cannot test\n");
    check("NSCMD_ETD_READ64", send(NSCMD_ETD_READ64, buf, 512, 0x40000000, 1, change), 0);
    check("NSCMD_ETD_SEEK64", send(NSCMD_ETD_SEEK64, NULL, 0, 0x40000000, 1, change), 0);
    check("ETD_SEEK", send(ETD_SEEK, NULL, 0, 0x100000, 0, change), 0);
    check("ETD_MOTOR", send(ETD_MOTOR, NULL, 1, 0, 0, change), 0);
    check("ETD_UPDATE", send(ETD_UPDATE, NULL, 0, 0, 0, change), 0);
    check("ETD_CLEAR", send(ETD_CLEAR, NULL, 0, 0, 0, change), 0);
    check("TD_EJECT load", send(TD_EJECT, NULL, 0, 0, 0, 0), 0);
    check("TD_RAWREAD stays unsupported", send(TD_RAWREAD, buf, 512, 0, 0, 0), IOERR_NOCMD);

    check("CMD_STOP", send(CMD_STOP, NULL, 0, 0, 0, 0), 0);
    check("CMD_START", send(CMD_START, NULL, 0, 0, 0, 0), 0);
    check("read after START", send(CMD_READ, buf, 512, 0, 0, 0), 0);

    irq.is_Node.ln_Type = NT_INTERRUPT;
    check("TD_REMOVE set", send(TD_REMOVE, &irq, 0, 0, 0, 0), 0);
    check("TD_REMOVE NULL", send(TD_REMOVE, NULL, 0, 0, 0, 0), 0);
    check("TD_REMOVE NULL, nothing set", send(TD_REMOVE, NULL, 0, 0, 0, 0), 0);

    bad_status("HD_SCSICMD bad, no sense", 0);
    bad_status("HD_SCSICMD bad, autosense", SCSIF_AUTOSENSE);
    timeout();

    memset(&q, 0, sizeof(q));
    if (!send(NSCMD_DEVICEQUERY, &q, sizeof(q), 0, 0, 0)) {
        for (i = 0; want[i]; i++) {
            UWORD *c;
            for (c = q.nsdqr_SupportedCommands; *c && *c != want[i]; c++)
                ;
            P("Query lists %04lx: %s\n", (ULONG)want[i], (ULONG)(*c ? "ok" : "WRONG"));
            if (!*c) failures++;
        }
    }
    P("RESULT CmdTest: %ld failures\n", (LONG)failures);
}

LONG start(void)
{
    LONG rc = RETURN_FAIL, args[1] = { 0 };
    struct RDArgs *rd;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
    if (!DOSBase) return rc;
    rd = (struct RDArgs *)ReadArgs((STRPTR)"UNIT/K/N/A", args, NULL);
    if (rd) {
        port.mp_Node.ln_Type = NT_MSGPORT;
        port.mp_Flags = PA_SIGNAL;
        port.mp_SigBit = AllocSignal(-1);
        port.mp_SigTask = (struct Task *)FindTask(NULL);
        port.mp_MsgList.lh_Head = (struct Node *)&port.mp_MsgList.lh_Tail;
        port.mp_MsgList.lh_TailPred = (struct Node *)&port.mp_MsgList.lh_Head;
        io.iotd_Req.io_Message.mn_ReplyPort = &port;
        io.iotd_Req.io_Message.mn_Length = sizeof(io);
        if (!OpenDevice((STRPTR)"mtecscsi.device", *(LONG *)args[0], (struct IORequest *)&io, 0)) {
            test();
            CloseDevice((struct IORequest *)&io);
            rc = failures ? RETURN_ERROR : RETURN_OK;
        }
        FreeArgs(rd);
    }
    CloseLibrary((struct Library *)DOSBase);
    return rc;
}
