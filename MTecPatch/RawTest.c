/* RawTest - raw checks of mtecscsi.device around the 2 GB and 4 GB marks.
 *
 *   RawTest           WARNING: overwrites the tested disk areas
 *
 * Uses unit 0, so run it on a scratch disk only. Every sector holds its own
 * number mixed with a seed, so a host tool can check the image afterwards.
 */
__asm("  .text\n  jmp _start\n");

#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>

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

#define BUFSIZE 0x100000UL

static struct MsgPort port;
static struct IOStdReq io;
static ULONG *buf;
static ULONG failures;

static void fill(ULONG hi, ULONG lo, ULONG len, ULONG seed)
{
    ULONG i, n = len >> 9, sec = (hi << 23) | (lo >> 9);
    for (i = 0; i < n; i++, sec++) {
        ULONG *p = buf + (i << 7), j;
        for (j = 0; j < 128; j++)
            p[j] = sec ^ seed ^ (j << 24);
    }
}

static ULONG check(ULONG hi, ULONG lo, ULONG len, ULONG seed)
{
    ULONG i, n = len >> 9, sec = (hi << 23) | (lo >> 9), bad = 0;
    for (i = 0; i < n; i++, sec++) {
        ULONG *p = buf + (i << 7), j;
        for (j = 0; j < 128; j++)
            if (p[j] != (sec ^ seed ^ (j << 24))) { bad++; break; }
    }
    return bad;
}

static LONG xfer(UWORD cmd, ULONG hi, ULONG lo, ULONG len)
{
    io.io_Command = cmd;
    io.io_Flags = 0;
    io.io_Data = buf;
    io.io_Length = len;
    io.io_Offset = lo;
    io.io_Actual = hi;
    DoIO((struct IORequest *)&io);
    return io.io_Error;
}

static void wr(const char *name, UWORD cmd, ULONG hi, ULONG lo, ULONG len, ULONG seed)
{
    LONG e;
    fill(hi, lo, len, seed);
    e = xfer(cmd, hi, lo, len);
    P("WRITE %-5s %lx:%08lx len %lx err %ld actual %lx\n", (ULONG)name, hi, lo, len, e, io.io_Actual);
    if (e || io.io_Actual != len) failures++;
}

static void rd(const char *name, UWORD cmd, ULONG hi, ULONG lo, ULONG len, ULONG seed)
{
    LONG e;
    ULONG bad;
    memset(buf, 0xA5, len);
    e = xfer(cmd, hi, lo, len);
    bad = check(hi, lo, len, seed);
    P("READ  %-5s %lx:%08lx len %lx err %ld actual %lx badsectors %ld\n", (ULONG)name, hi, lo, len, e, io.io_Actual, bad);
    if (e || bad || io.io_Actual != len) failures++;
}

static void test(void)
{
    struct NSDeviceQueryResult q;
    UWORD *c;
    LONG e;

    memset(&q, 0, sizeof(q));
    io.io_Command = NSCMD_DEVICEQUERY;
    io.io_Flags = 0;
    io.io_Data = &q;
    io.io_Length = sizeof(q);
    e = DoIO((struct IORequest *)&io);
    P("DEVICEQUERY err %ld type %ld size %ld\n", e, q.nsdqr_DeviceType, q.nsdqr_SizeAvailable);
    if (e || q.nsdqr_DeviceType != NSDEVTYPE_TRACKDISK) failures++;
    else {
        ULONG found = 0;
        for (c = q.nsdqr_SupportedCommands; *c; c++)
            if (*c == NSCMD_TD_READ64 || *c == NSCMD_TD_WRITE64) found++;
        if (found != 2) failures++;
    }

    /* Straddles 2 GB: split between CMD_WRITE and WRITE(10). */
    wr("CMD", CMD_WRITE, 0, 0x7FFFF000, 0x2000, 0x11000000);
    rd("CMD", CMD_READ, 0, 0x7FFFF000, 0x2000, 0x11000000);
    rd("TD64", TD_READ64, 0, 0x7FFFF000, 0x2000, 0x11000000);
    rd("NSD", NSCMD_TD_READ64, 0, 0x7FFFF000, 0x2000, 0x11000000);

    /* Between 2 GB and 4 GB with a 32-bit command. */
    wr("CMD", CMD_WRITE, 0, 0xC0000000, BUFSIZE, 0x22000000);
    rd("CMD", CMD_READ, 0, 0xC0000000, BUFSIZE, 0x22000000);
    rd("TD64", TD_READ64, 0, 0xC0000000, BUFSIZE, 0x22000000);

    /* Straddles 4 GB. */
    wr("TD64", TD_WRITE64, 0, 0xFFFFF000, 0x2000, 0x33000000);
    rd("NSD", NSCMD_TD_READ64, 0, 0xFFFFF000, 0x2000, 0x33000000);
    rd("TD64", TD_READ64, 0, 0xFFFFF000, 0x2000, 0x33000000);

    /* Well above 4 GB. */
    wr("NSD", NSCMD_TD_WRITE64, 1, 0x40000000, BUFSIZE, 0x44000000);
    rd("TD64", TD_READ64, 1, 0x40000000, BUFSIZE, 0x44000000);
    rd("NSD", NSCMD_TD_READ64, 1, 0x40000000, BUFSIZE, 0x44000000);

    /* Below 2 GB a 64-bit write must keep the driver's read cache correct. */
    wr("CMD", CMD_WRITE, 0, 0x40000000, 0x10000, 0x55000000);
    rd("CMD", CMD_READ, 0, 0x40000000, 0x10000, 0x55000000);
    wr("TD64", TD_WRITE64, 0, 0x40000000, 0x10000, 0x66000000);
    rd("CMD", CMD_READ, 0, 0x40000000, 0x10000, 0x66000000);
    wr("NSD", NSCMD_TD_WRITE64, 0, 0x40000000, 0x10000, 0x77000000);
    rd("CMD", CMD_READ, 0, 0x40000000, 0x10000, 0x77000000);

    /* Misaligned requests must be refused. */
    e = xfer(TD_READ64, 1, 0x40000100, 0x200);
    P("MISALIGNED err %ld\n", e);
    if (!e) failures++;

    P("RESULT %ld failures\n", failures);
}

LONG start(void)
{
    LONG rc = RETURN_FAIL;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
    if (!DOSBase) return rc;
    port.mp_Node.ln_Type = NT_MSGPORT;
    port.mp_Flags = PA_SIGNAL;
    port.mp_SigBit = AllocSignal(-1);
    port.mp_SigTask = (struct Task *)FindTask(NULL);
    port.mp_MsgList.lh_Head = (struct Node *)&port.mp_MsgList.lh_Tail;
    port.mp_MsgList.lh_TailPred = (struct Node *)&port.mp_MsgList.lh_Head;
    io.io_Message.mn_ReplyPort = &port;
    io.io_Message.mn_Length = sizeof(io);
    buf = (ULONG *)AllocMem(BUFSIZE, MEMF_PUBLIC);
    if (buf) {
        if (!OpenDevice((STRPTR)"mtecscsi.device", 0, (struct IORequest *)&io, 0)) {
            test();
            rc = failures ? RETURN_ERROR : RETURN_OK;
            CloseDevice((struct IORequest *)&io);
        } else {
            P("OpenDevice failed\n");
        }
        FreeMem(buf, BUFSIZE);
    }
    CloseLibrary((struct Library *)DOSBase);
    return rc;
}
