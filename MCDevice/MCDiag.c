/* MCDiag - tests how mastercard.device moves data on the real MasterCard.
 *
 *   MCDiag [UNIT n] [TAKEOVER] [WRITE] [LBA n] [LOG file] [FINDUNIT] [NODMA8] [NOPIO]
 *
 * Every test runs a SCSI command through the chip's FIFO without DMA first
 * (PIO, the reference), then with each of the board's data port modes, and
 * compares data and length:
 *   DMA16  16 bytes per $C039 flag, words at the end (the driver's normal mode)
 *   DMA8   8 bytes per $C839 flag, words at the end
 *   WORD   one word per $D039 flag
 * Each line shows what the data port did: bursts, word moves, bytes through the
 * chip FIFO, DMA chunks, the longest wait, and the flags and chip registers at
 * the end.
 *
 * A speed section times 16 reads of 64 KB per mode and one-block commands,
 * and two read comparisons: DMA32 (32 bytes per flag, faster but only safe
 * if the board holds 32 bytes) and MOVEM (DMA16 read with movem.l). They only report; they don't count as failures.
 *
 * Read-only unless WRITE is given. The buffer tests write the drive's own
 * buffer (WRITE BUFFER / READ BUFFER), not the medium. WRITE adds medium
 * writes at LBA (default: 300 blocks before the end): each block is read,
 * written back unchanged and with a pattern, checked, and restored.
 *
 * TAKEOVER is for a card that still has the M-Tec ROM: it freezes the
 * mtecscsi.device process while it waits, removes its interrupt server, makes
 * its BeginIO fail every request and renames it, so mastercard.device can
 * load. Nothing can use the M-Tec driver until the next reboot.
 *
 * FINDUNIT prints the first disk's unit number and ends, for scripts.
 * NODMA8 and NOPIO leave those modes out; emulators model neither.
 */
__asm("  .text\n  jmp _start\n");

#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/interrupts.h>
#include <exec/devices.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <devices/scsidisk.h>
#include <devices/trackdisk.h>
#include <libraries/configvars.h>
#include <hardware/intbits.h>
#include <devices/timer.h>
#include "mcdiag.h"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *ExpansionBase;
struct Device *TimerBase;

#include "gen/tstubs.h"

LONG start(void);

static const char vers[] __attribute__((used)) = "$VER: MCDiag 0.1 (4.11.2026)";

#define DEV_NAME "mastercard.device"
#define BUFSIZE  (0x10000 + 0x1000 + 16)
#define PIO_MAX  4096    /* longest PIO transfer worth waiting for */

static BPTR logfh;
static ULONG fails, oks, skips;
static struct MsgPort port;
static struct IOStdReq io;
static UBYTE *ref, *got, *save;
static ULONG bsize, blocks;
static UBYTE devtype;
static UBYTE skipmode[MCMODES];
static const char *mname[MCMODES] = { "DMA16", "DMA8", "WORD", "PIO", "DMA32", "MOVEM" };

__attribute__((used)) void *memset(void *d, int c, unsigned long n)
{
    UBYTE *p = d;
    while (n--) *p++ = (UBYTE)c;
    return d;
}

__attribute__((used)) void *memcpy(void *d, const void *s, unsigned long n)
{
    UBYTE *p = d;
    const UBYTE *q = s;
    while (n--) *p++ = *q++;
    return d;
}

static void out(const char *fmt, LONG *args)
{
    VPrintf((STRPTR)fmt, args);
    if (logfh) {
        VFPrintf(logfh, (STRPTR)fmt, args);
        Flush(logfh);
    }
}
#define P(fmt, ...) do { ULONG a_[] = { 0, ##__VA_ARGS__ }; out(fmt, (LONG *)(a_ + 1)); } while (0)

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

/* ---- taking over from the M-Tec driver ---- */

/* BeginIO for the frozen driver: every request fails at once. */
static const UWORD fail_beginio[] = {
    0x137C, 0x00FF, 0x001F,     /* move.b #IOERR_OPENFAIL,io_Error(a1) */
    0x0829, 0x0000, 0x001E,     /* btst #IOB_QUICK,io_Flags(a1) */
    0x660C,                     /* bne.s 1f */
    0x2F0E,                     /* move.l a6,-(sp) */
    0x2C78, 0x0004,             /* movea.l 4.w,a6 */
    0x4EAE, 0xFE86,             /* jsr ReplyMsg(a6) */
    0x2C5F,                     /* movea.l (sp)+,a6 */
    0x4E75                      /* 1: rts */
};

static ULONG drained;

static int takeover(void)
{
    struct Device *mt;
    struct Task *proc;
    struct Interrupt *is = 0;
    struct Node *n;
    UBYTE *mem;
    int tries;

    for (tries = 0; tries < 50; tries++) {
        Forbid();
        mt = (struct Device *)FindName(&SysBase->DeviceList, (STRPTR)"mtecscsi.device");
        if (!mt) {
            Permit();
            P("TAKEOVER: no mtecscsi.device running, nothing to do\n");
            return 1;
        }
        proc = (struct Task *)FindTask((STRPTR)"mtecscsi.device");
        if (!proc || proc->tc_State == TS_WAIT)
            break;
        Permit();
        Delay(2);
    }
    if (tries == 50) {
        P("TAKEOVER: the M-Tec process never waited, giving up\n");
        return 0;
    }
    /* Still in Forbid. */
    mem = (UBYTE *)AllocMem(sizeof fail_beginio + 16, MEMF_PUBLIC);
    if (!mem) {
        Permit();
        P("TAKEOVER: no memory\n");
        return 0;
    }
    if (proc) {
        Remove(&proc->tc_Node);         /* frozen: Signal leaves a removed task alone */
        proc->tc_State = TS_REMOVED;
    }
    for (n = ((struct List *)SysBase->IntVects[INTB_PORTS].iv_Data)->lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (n->ln_Name && streq(n->ln_Name, "mtecscsi.device")) {
            is = (struct Interrupt *)n;
            break;
        }
    if (is)
        RemIntServer(INTB_PORTS, is);
    {
        UWORD k;
        for (k = 0; k < sizeof fail_beginio / 2; k++)
            ((UWORD *)mem)[k] = fail_beginio[k];
        CacheClearU();
        SetFunction((struct Library *)mt, DEV_BEGINIO, (APTR)mem);
        {
            const char *nm = "mtecscsi.off";
            char *d = (char *)mem + sizeof fail_beginio;
            while ((*d++ = *nm++)) ;
            mt->dd_Library.lib_Node.ln_Name = (char *)mem + sizeof fail_beginio;
        }
    }
    if ((ExpansionBase = (struct Library *)OpenLibrary((STRPTR)"expansion.library", 0))) {
        struct ConfigDev *cd = (struct ConfigDev *)FindConfigDev(0, 2192, 33);
        if (cd) {
            /* The frozen driver may have left a command running, a chip
               interrupt pending or data in the board, and nothing serves
               INT2 for the card until mastercard.device loads: reset the
               chip and empty the board with interrupts off. */
            volatile UBYTE *b = (volatile UBYTE *)cd->cd_BoardAddr;
            ULONG k;
            Disable();
            b[0xC000 + (3 << 3)] = 0x02;            /* Reset Chip */
            b[0xC000 + (3 << 3)] = 0x00;            /* NOP */
            (void)b[0xC000 + (5 << 3)];             /* interrupt register */
            for (k = 0; k < 4096 && ((b[0xD039] | b[0xC039]) & 0x80); k++)
                (void)*(volatile UWORD *)(b + 0xF000);
            Enable();
            drained = k;
            cd->cd_Driver = 0;
        }
        CloseLibrary(ExpansionBase);
    }
    Permit();
    P("TAKEOVER: process %s, interrupt server %s, device renamed to mtecscsi.off\n",
      (ULONG)(proc ? "frozen" : "not found"), (ULONG)(is ? "removed" : "not found"));
    P("TAKEOVER: chip reset, %lu words drained from the board\n", drained);
    return 1;
}

/* ---- running commands ---- */

static struct MCDiagReq dr;
static struct timerequest treq;
static ULONG efreq;                     /* EClock ticks per second, 0 without timer */
static ULONG lastus;                    /* time of the last run() */

static ULONG now(void)
{
    struct EClockVal ev;
    if (!efreq)
        return 0;
    ReadEClock(&ev);
    return ev.ev_lo;
}

static ULONG ticks_us(ULONG t)
{
    return efreq ? t / (efreq / 1000) * 1000 + t % (efreq / 1000) * 1000 / (efreq / 1000) : 0;
}

static LONG run(UBYTE mode, const UBYTE *cdb, UWORD cdblen, UBYTE in, UBYTE *data, ULONG len)
{
    UWORD k;
    memset(&dr, 0, sizeof dr);
    for (k = 0; k < cdblen; k++)
        dr.cdb[k] = cdb[k];
    dr.cdblen = cdblen;
    dr.in = in;
    dr.mode = mode;
    dr.data = data;
    dr.len = len;
    io.io_Command = MCCMD_DIAG;
    io.io_Data = &dr;
    io.io_Length = sizeof dr;
    {
        ULONG t0 = now();
        BYTE e = DoIO((struct IORequest *)&io);
        lastus = ticks_us(now() - t0);
        if (e)
            return io.io_Error;
    }
    return dr.err;
}

static void stats(void)
{
    struct MCStats *s = &dr.st;
    P(" b%lu w%lu f%lu c%lu p%lu tc%lx [%02lx %02lx %02lx] st%02lx in%02lx sq%02lx ff%02lx d%lu x%lu t%luus\n",
      s->bursts, s->words, s->fifo, s->chunks, s->maxpolls, s->tcleft,
      (ULONG)s->dreq16, (ULONG)s->dreq8, (ULONG)s->dreqw,
      (ULONG)s->stat, (ULONG)s->intr, (ULONG)s->seq, (ULONG)s->fflags, s->drained, s->stray, lastus);
}

/* The normal mode (DMA16) into ref, then the others into got, compared with
   ref. align puts the data at got+align, with guard bytes around it. */
static const UBYTE order[MCMODES_STD] = { MCMODE_DMA16, MCMODE_DMA8, MCMODE_WORD, MCMODE_PIO };

static void test(const char *name, const UBYTE *cdb, UWORD cdblen, ULONG len, ULONG align)
{
    LONG e;
    ULONG want = 0, k, i;

    P("-- %s\n", (ULONG)name);         /* flushed: shows where a hang happened */

    for (k = 0; k < MCMODES_STD; k++) {
        UBYTE m = order[k];
        UBYTE *g = k ? got + align : ref;
        if ((m == MCMODE_PIO && len > PIO_MAX) || skipmode[m])
            continue;                   /* PIO: one interrupt per byte, too slow */
        if (k)
            memset(got, 0x5A, len + align + 16);
        else
            memset(ref, 0xA5, len + 16);
        e = run(m, cdb, cdblen, 1, g, len);
        if (!k) {
            want = dr.actual;
            if (e || dr.status) {
                if (!e && dr.status == 0x02) {
                    skips++;
                    P("%-18s skipped: the device refused it (status %02lx)\n", (ULONG)name, (ULONG)dr.status);
                } else {
                    fails++;
                    P("%-18s DMA16 ERR %ld %lu/%lu s%02lx", (ULONG)name, e, dr.actual, len, (ULONG)dr.status);
                    stats();
                }
                return;
            }
            if (ref[len] != 0xA5) {
                fails++;
                P("%-18s DMA16 OVER the buffer end was overwritten", (ULONG)name);
            } else {
                oks++;
                P("%-18s DMA16 ok   %5lu/%-5lu s%02lx", (ULONG)name, want, len, (ULONG)dr.status);
            }
            stats();
            continue;
        }
        for (i = 0; i < want && i < dr.actual && g[i] == ref[i]; i++) ;
        if (!e && !dr.status && dr.actual == want && i == want && got[len + align] == 0x5A &&
            (!align || got[align - 1] == 0x5A)) {
            oks++;
            P("%-18s %-5s same %5lu/%-5lu s%02lx", (ULONG)name, (ULONG)mname[m], dr.actual, len, (ULONG)dr.status);
        } else {
            fails++;
            if (e)
                P("%-18s %-5s ERR %ld %lu/%lu s%02lx", (ULONG)name, (ULONG)mname[m], e, dr.actual, len, (ULONG)dr.status);
            else if (dr.actual != want)
                P("%-18s %-5s LEN  %5lu/%-5lu DMA16 %lu", (ULONG)name, (ULONG)mname[m], dr.actual, len, want);
            else if (i != want)
                P("%-18s %-5s DIFF @%lx %02lx!=%02lx", (ULONG)name, (ULONG)mname[m], i, (ULONG)g[i], (ULONG)ref[i]);
            else
                P("%-18s %-5s OVER a guard byte was overwritten", (ULONG)name, (ULONG)mname[m]);
        }
        stats();
    }
}

static void put32(UBYTE *p, ULONG v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void read_test(const char *name, ULONG lba, UWORD n, ULONG align)
{
    UBYTE cdb[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    put32(cdb + 2, lba);
    cdb[7] = n >> 8;
    cdb[8] = n;
    test(name, cdb, 10, n * bsize, align);
}

/* Writes len bytes of a known pattern with mode wm and reads them back with
   mode rm, through the drive's buffer (WRITE/READ BUFFER) or the medium. The
   check needs no reference: the pattern is known. */
static void pattern(UBYTE *p, ULONG len, ULONG seed)
{
    ULONG i;
    for (i = 0; i < len; i++)
        p[i] = (UBYTE)(i * 7 + (i >> 8) + seed * 31 + 1);
}

static int roundtrip(const char *name, UBYTE *wcdb, UBYTE *rcdb, UWORD cdblen, ULONG len, UBYTE wm, UBYTE rm,
                     ULONG seed)
{
    struct MCStats ws, rs;
    ULONG wact, i;
    UBYTE wst;
    LONG e;

    pattern(ref, len, seed);
    e = run(wm, wcdb, cdblen, 0, ref, len);
    wact = dr.actual;
    wst = dr.status;
    ws = dr.st;
    memset(got, 0x5A, len + 16);
    if (!e && !wst)
        e = run(rm, rcdb, cdblen, 1, got, len);
    rs = dr.st;
    for (i = 0; i < len && got[i] == ref[i]; i++) ;
    if (!e && !wst && !dr.status && wact == len && dr.actual == len && i == len && got[len] == 0x5A) {
        oks++;
        P("%-18s %-5s>%-5s ok   %5lu", (ULONG)name, (ULONG)mname[wm], (ULONG)mname[rm], len);
        dr.st = ws;
        stats();
        return 1;
    }
    fails++;
    P("%-18s %-5s>%-5s FAIL e%ld w%lu/s%02lx r%lu/s%02lx len %lu @%lx", (ULONG)name, (ULONG)mname[wm],
      (ULONG)mname[rm], e, wact, (ULONG)wst, dr.actual, (ULONG)dr.status, len, i);
    if (i < len)
        P(" %02lx!=%02lx", (ULONG)got[i], (ULONG)ref[i]);
    P("\n  write:");
    dr.st = ws;
    stats();
    P("  read: ");
    dr.st = rs;
    stats();
    return 0;
}

/* WRITE BUFFER and READ BUFFER (data mode, buffer 0): every write mode with
   every read mode, odd and even lengths. Doesn't touch the medium. */
static void buffer_tests(void)
{
    static const UBYTE lens[] = { 1, 2, 3, 4, 15, 16, 17, 18, 31, 33, 64, 255 };
    UBYTE wcdb[10], rcdb[10];
    ULONG k, wm, rm, cap;
    LONG e;

    memset(rcdb, 0, sizeof rcdb);
    rcdb[0] = 0x3C; rcdb[1] = 0x03; rcdb[8] = 4;    /* READ BUFFER, descriptor */
    memset(ref, 0, 4);
    e = run(MCMODE_DMA16, rcdb, 10, 1, ref, 4);
    if (e || dr.status || dr.actual < 4) {
        skips++;
        P("WRITE/READ BUFFER  skipped: not supported\n");
        return;
    }
    cap = (ULONG)ref[1] << 16 | (ULONG)ref[2] << 8 | ref[3];
    P("WRITE/READ BUFFER  the drive's buffer is %lu bytes\n", cap);
    /* Some drives (BlueSCSI) take WRITE BUFFER but don't give the data back:
       check with 64 bytes in the normal mode first. */
    {
        ULONG f0 = fails, o0 = oks;
        memset(wcdb, 0, sizeof wcdb);
        wcdb[0] = 0x3B; wcdb[1] = 0x02; wcdb[8] = 64;
        memset(rcdb, 0, sizeof rcdb);
        rcdb[0] = 0x3C; rcdb[1] = 0x02; rcdb[8] = 64;
        if (cap < 64 || !roundtrip("BUFFER check", wcdb, rcdb, 10, 64, MCMODE_DMA16, MCMODE_DMA16, 99)) {
            fails = f0;
            oks = o0;
            skips++;
            P("WRITE/READ BUFFER  skipped: the drive doesn't return what was written\n");
            return;
        }
        oks = o0;
    }
    for (k = 0; k < sizeof lens; k++) {
        ULONG len = lens[k];
        if (len > cap)
            break;
        memset(wcdb, 0, sizeof wcdb);
        wcdb[0] = 0x3B; wcdb[1] = 0x02; wcdb[8] = len;  /* WRITE BUFFER, data */
        memset(rcdb, 0, sizeof rcdb);
        rcdb[0] = 0x3C; rcdb[1] = 0x02; rcdb[8] = len;  /* READ BUFFER, data */
        for (wm = 0; wm < MCMODES_STD; wm++)
            for (rm = 0; rm < MCMODES_STD; rm++) {
                if (skipmode[wm] || skipmode[rm])
                    continue;
                char name[16] = "BUFFER ";
                name[7] = '0' + len / 100; name[8] = '0' + len / 10 % 10; name[9] = '0' + len % 10; name[10] = 0;
                roundtrip(name, wcdb, rcdb, 10, len, wm, rm, len + wm * 4 + rm);
            }
    }
}

/* Medium writes, only with WRITE: the original blocks are saved with the
   normal mode, a pattern is written with each mode and read back with DMA16
   and PIO, then the original is written back and checked. */
static void write_tests(ULONG lba)
{
    static const UWORD ns[] = { 1, 8, 129 };
    UBYTE wcdb[10], rcdb[10];
    ULONG k, wm, i, len;
    LONG e;

    P("Medium writes at LBA %lu\n", lba);
    for (k = 0; k < sizeof ns / 2; k++) {
        UWORD n = ns[k];
        char name[16] = "WRITE ";
        len = n * bsize;
        if (len > BUFSIZE - 32 || lba + n > blocks)
            break;
        memset(rcdb, 0, sizeof rcdb);
        rcdb[0] = 0x28; put32(rcdb + 2, lba); rcdb[7] = n >> 8; rcdb[8] = n;
        for (i = 0; i < 10; i++)
            wcdb[i] = rcdb[i];
        wcdb[0] = 0x2A;
        name[6] = '0' + n / 100; name[7] = '0' + n / 10 % 10; name[8] = '0' + n % 10; name[9] = 0;
        if (run(MCMODE_DMA16, rcdb, 10, 1, save, len) || dr.status || dr.actual != len) {
            fails++;
            P("%s blocks: cannot read the original, stopping\n", (ULONG)name);
            return;
        }
        for (wm = 0; wm < MCMODES_STD; wm++) {
            if ((wm == MCMODE_PIO && len > PIO_MAX) || skipmode[wm])
                continue;
            roundtrip(name, wcdb, rcdb, 10, len, wm, MCMODE_DMA16, n + wm);
            if (len <= PIO_MAX && !skipmode[MCMODE_PIO])
                roundtrip(name, wcdb, rcdb, 10, len, wm, MCMODE_PIO, n + wm + 8);
        }
        e = run(MCMODE_DMA16, wcdb, 10, 0, save, len);
        if (!e)
            e = run(MCMODE_DMA16, rcdb, 10, 1, got, len);
        for (i = 0; i < len && got[i] == save[i]; i++) ;
        if (e || i != len) {
            fails++;
            P("%s blocks: RESTORE FAILED at LBA %lu, the original data there is lost\n", (ULONG)name, lba);
            return;
        }
        P("%s blocks: original restored\n", (ULONG)name);
    }
}

/* Speed: 16 reads of 64 KB in each mode, timed by the EClock, each checked
   against a DMA16 read; then the time of one command, from 64 reads of one
   block. DMA32 and MOVEM are comparisons, see the top. */
static void speed_tests(void)
{
    static const UBYTE modes[] = { MCMODE_DMA16, MCMODE_DMA8, MCMODE_WORD, MCMODE_DMA32, MCMODE_MOVEM };
    UBYTE cdb[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    ULONG len = 0x10000, n = len / bsize, k, i, j, t0, dt;

    if (!efreq) {
        P("SPEED              skipped: no EClock\n");
        return;
    }
    cdb[7] = n >> 8;
    cdb[8] = n;
    if (run(MCMODE_DMA16, cdb, 10, 1, ref, len) || dr.status || dr.actual != len) {
        P("SPEED              skipped: the reference read failed\n");
        skips++;
        return;
    }
    for (k = 0; k < sizeof modes; k++) {
        UBYTE m = modes[k];
        ULONG bad = 0, err = 0;
        if (skipmode[m])
            continue;
        t0 = now();
        for (j = 0; j < 16; j++) {
            if (run(m, cdb, 10, 1, got, len) || dr.status || dr.actual != len) {
                err++;
                break;
            }
            if (!j) {
                for (i = 0; i < len && got[i] == ref[i]; i++) ;
                if (i != len)
                    bad = i + 1;
            }
        }
        dt = now() - t0;
        if (err) {
            if (m < MCMODES_STD)
                fails++;                /* the experiments only report */
            P("SPEED 64K x16      %-5s ERR %ld %lu/%lu", (ULONG)mname[m], dr.err, dr.actual, len);
            stats();
        } else {
            if (bad) {
                if (m < MCMODES_STD)
                    fails++;
                P("SPEED 64K x16      %-5s DIFF @%lx %02lx!=%02lx  %lu KB/s", (ULONG)mname[m], bad - 1,
                  (ULONG)got[bad - 1], (ULONG)ref[bad - 1], dt ? 1024 * efreq / dt : 0);
            } else {
                oks++;
                P("SPEED 64K x16      %-5s same %lu KB/s, %lu us per 64 KB", (ULONG)mname[m],
                  dt ? 1024 * efreq / dt : 0, ticks_us(dt) / 16);
            }
            stats();
        }
    }
    cdb[7] = 0;
    cdb[8] = 1;
    t0 = now();
    for (j = 0; j < 64; j++)
        if (run(MCMODE_DMA16, cdb, 10, 1, got, bsize) || dr.status)
            break;
    dt = now() - t0;
    P("SPEED 1 block x64  DMA16 %lu us per command\n", ticks_us(dt) / 64);
}

static int open_unit(ULONG unit)
{
    io.io_Message.mn_ReplyPort = &port;
    io.io_Message.mn_Length = sizeof io;
    return !OpenDevice((STRPTR)DEV_NAME, unit, (struct IORequest *)&io, 0);
}

static void unit_tests(ULONG unit, int write, ULONG wlba)
{
    static const UBYTE inq36[6] = { 0x12, 0, 0, 0, 36, 0 };
    static const UBYTE inq37[6] = { 0x12, 0, 0, 0, 37, 0 };
    static const UBYTE inq255[6] = { 0x12, 0, 0, 0, 255, 0 };
    static const UBYTE sense18[6] = { 0x03, 0, 0, 0, 18, 0 };
    static const UBYTE msense[6] = { 0x1A, 0x08, 0x3F, 0, 255, 0 };
    static const UBYTE rcap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    UBYTE inq[36];
    ULONG k;

    P("Unit %lu open, INQUIRY with DMA16\n", unit);
    if (run(MCMODE_DMA16, inq36, 6, 1, inq, 36) || dr.status) {
        P("Unit %lu: INQUIRY failed, err %ld status %02lx\n", unit, dr.err, (ULONG)dr.status);
        fails++;
        return;
    }
    devtype = inq[0] & 0x1F;
    {
        char v[9], p[17], r[5];
        for (k = 0; k < 8; k++) v[k] = inq[8 + k];
        for (k = 0; k < 16; k++) p[k] = inq[16 + k];
        for (k = 0; k < 4; k++) r[k] = inq[32 + k];
        v[8] = p[16] = r[4] = 0;
        P("\n== Unit %lu: type %lu  %s %s %s\n", unit, (ULONG)devtype, (ULONG)v, (ULONG)p, (ULONG)r);
    }
    test("INQUIRY 36", inq36, 6, 36, 0);
    test("INQUIRY 37", inq37, 6, 37, 0);
    test("INQUIRY 255", inq255, 6, 255, 0);
    test("REQUEST SENSE 18", sense18, 6, 18, 0);
    test("MODE SENSE 255", msense, 6, 255, 0);
    bsize = 0;
    blocks = 0;
    {
        UBYTE c[8];
        if (!run(MCMODE_DMA16, rcap, 10, 1, c, 8) && !dr.status && dr.actual == 8) {
            blocks = ((ULONG)c[0] << 24 | (ULONG)c[1] << 16 | (ULONG)c[2] << 8 | c[3]) + 1;
            bsize = (ULONG)c[4] << 24 | (ULONG)c[5] << 16 | (ULONG)c[6] << 8 | c[7];
        }
    }
    if (!bsize || bsize > 4096 || (bsize & 511)) {
        P("No readable medium (block size %lu), skipping the reads\n", bsize);
        skips++;
    } else {
        UWORD n64 = 0x10000 / bsize;
        P("Medium: %lu blocks of %lu bytes\n", blocks, bsize);
        read_test("READ 1", 0, 1, 0);
        read_test("READ 8", 0, 8, 0);
        read_test("READ 64K", 0, n64, 0);
        read_test("READ 64K+1 block", 0, n64 + 1, 0);
        read_test("READ last block", blocks - 1, 1, 0);
        read_test("READ 8 buf+1", 0, 8, 1);
        read_test("READ 8 buf+2", 0, 8, 2);
        read_test("READ 8 buf+3", 0, 8, 3);
        for (k = 1; k <= 8; k++)
            read_test("READ 64K spread", (blocks - n64) / 9 * k, n64, 0);
        speed_tests();
    }
    buffer_tests();
    if (write && bsize && blocks > 600)
        write_tests(wlba ? wlba : blocks - 300);
}

LONG start(void)
{
    LONG rc = RETURN_FAIL;
    LONG args[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    struct RDArgs *ra;
    ULONG unit, first, last;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
    if (!DOSBase)
        return rc;
    ra = (struct RDArgs *)ReadArgs((STRPTR)"UNIT/N,TAKEOVER/S,WRITE/S,LBA/N,LOG/K,FINDUNIT/S,NODMA8/S,NOPIO/S", args, 0);
    if (!ra) {
        PrintFault(IoErr(), (STRPTR)"MCDiag");
        goto done;
    }
    port.mp_Node.ln_Type = NT_MSGPORT;
    port.mp_Flags = PA_SIGNAL;
    port.mp_SigBit = SIGB_SINGLE;
    port.mp_SigTask = (struct Task *)FindTask(0);
    port.mp_MsgList.lh_Head = (struct Node *)&port.mp_MsgList.lh_Tail;
    port.mp_MsgList.lh_TailPred = (struct Node *)&port.mp_MsgList.lh_Head;

    skipmode[MCMODE_DMA8] = args[6] != 0;
    skipmode[MCMODE_PIO] = args[7] != 0;
    if (args[5]) {                      /* FINDUNIT */
        for (unit = 0; unit < 7; unit++)
            if (open_unit(unit)) {
                CloseDevice((struct IORequest *)&io);
                P("%lu\n", unit);
                rc = RETURN_OK;
                goto done;
            }
        P("0\n");
        rc = RETURN_WARN;
        goto done;
    }
    if (args[4]) {
        logfh = Open((STRPTR)args[4], MODE_NEWFILE);
        if (!logfh)
            P("Cannot open the log %s\n", args[4]);
    }
    P("MCDiag 0.1 - data port tests for mastercard.device\n");
    if (args[1] && !takeover())
        goto done;
    treq.tr_node.io_Message.mn_ReplyPort = &port;
    treq.tr_node.io_Message.mn_Length = sizeof treq;
    if (!OpenDevice((STRPTR)"timer.device", UNIT_ECLOCK, (struct IORequest *)&treq, 0)) {
        struct EClockVal ev;
        TimerBase = treq.tr_node.io_Device;
        efreq = ReadEClock(&ev);
    }
    ref = (UBYTE *)AllocMem(BUFSIZE, MEMF_PUBLIC);
    got = (UBYTE *)AllocMem(BUFSIZE, MEMF_PUBLIC);
    save = (UBYTE *)AllocMem(BUFSIZE, MEMF_PUBLIC);
    if (!ref || !got || !save) {
        P("No memory\n");
        goto done;
    }
    first = args[0] ? *(ULONG *)args[0] : 0;
    last = args[0] ? first : 6;
    for (unit = first; unit <= last; unit++) {
        P("Opening unit %lu%s\n", unit,
          (ULONG)(unit == first ? " (loads the driver: chip and bus reset, 2-3 s)" : ""));
        if (!open_unit(unit)) {
            if (args[0] || io.io_Error != TDERR_BadUnitNum)
                if (io.io_Error != HFERR_SelTimeout)
                    P("Unit %lu: open failed, error %ld\n", unit, (LONG)io.io_Error);
            continue;
        }
        unit_tests(unit, args[2] != 0, args[3] ? *(ULONG *)args[3] : 0);
        CloseDevice((struct IORequest *)&io);
    }
    P("\nRESULT %lu ok, %lu failed, %lu skipped\n", oks, fails, skips);
    rc = fails ? RETURN_ERROR : RETURN_OK;
done:
    if (ref) FreeMem(ref, BUFSIZE);
    if (got) FreeMem(got, BUFSIZE);
    if (save) FreeMem(save, BUFSIZE);
    if (TimerBase) CloseDevice((struct IORequest *)&treq);
    if (logfh) Close(logfh);
    if (ra) FreeArgs(ra);
    CloseLibrary((struct Library *)DOSBase);
    return rc;
}
