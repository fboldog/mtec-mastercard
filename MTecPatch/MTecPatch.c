/* MTecPatch - large disk support for mtecscsi.device 2.3, loaded into RAM.
 *
 *   MTecPatch          install the patch (undone by a reboot)
 *   MTecPatch STATUS   show what it has handled so far
 *
 * The driver treats the 32-bit byte offset as signed, so CMD_READ and
 * CMD_WRITE break at 2 GB, and it has neither TD64 nor NSD. This wraps its
 * BeginIO. Requests that touch 2 GB or above, and all 64-bit commands, go to
 * a small task that issues READ(10)/WRITE(10) through HD_SCSICMD. Everything
 * below 2 GB stays on CMD_READ/CMD_WRITE, so the driver's read cache stays
 * correct and it still retries. HD_SCSICMD bypasses that cache, which is why
 * the two ranges are kept apart: nothing at or above 2 GB is ever cached.
 * TD_GETGEOMETRY and TD_GETNUMTRACKS, also missing, are built from READ
 * CAPACITY and INQUIRY. The disk has no real geometry, so it reports 255 heads
 * and 63 sectors per track, with the true sector count in dg_TotalSectors.
 * Also added: TD_EJECT, the ETD_ commands, TD_REMOVE with NULL, which the driver
 * would store and later Cause(), and a proper HFERR_BadStatus for HD_SCSICMD
 * without autosense, where the driver returns the raw SCSI status byte.
 *
 * Built with ROM_BUILD, the same code becomes a blob for the card's boot ROM
 * (see mkrom.py, or MTecROM for the build from source). The blob is copied to RAM right after the driver, without
 * relocation, so it is built PC-relative and keeps its variables in the code.
 * The device's init routine points at rom_entry, which runs the driver's own
 * init and then installs the patch before anything can boot.
 */
#ifndef ROM_BUILD
/* AmigaOS enters at the first byte of the first hunk. */
__asm("  .text\n  jmp _start\n");
#else
#define STR_(x) #x
#define STR(x) STR_(x)
/* Called as the device's init routine: d0 = device, a0 = segment, a6 = SysBase.
   For mkrom.py, ORIG_INIT is the distance from here back to the driver's init
   routine. The source build (MTecROM) assembles this with the driver, so it
   calls the driver's init by name. */
#ifdef ORIG_INIT
#define CALL_INIT "  jsr _rom_entry-" STR(ORIG_INIT) "(pc)\n"
#else
#define CALL_INIT "  jsr _mtec_init(pc)\n"
#endif
__asm(
"  .text\n"
"  .globl _rom_entry\n"
"_rom_entry:\n"
"  lea _SysBase(pc),a1\n"
"  move.l a6,(a1)\n"
CALL_INIT
"  tst.l d0\n"
"  beq.s 1f\n"
"  move.l d0,-(sp)\n"
"  move.l d0,-(sp)\n"
"  jsr _rom_install(pc)\n"
"  addq.l #4,sp\n"
"  move.l (sp)+,d0\n"
"1: rts\n"
);
#endif
/* Variables live in the code, so the ROM build reaches them PC-relative. */
__asm(
"  .text\n"
"  .even\n"
"  .globl _SysBase\n"
"_SysBase: .long 0\n"
"  .globl _pt\n"
"_pt: .long 0\n"
"  .globl _old_beginio\n"
"_old_beginio: .long 0\n"
);

#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/ports.h>
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

#define DEV_NAME   "mtecscsi.device"
#ifdef ROM_BUILD
#define PATCHED_REV 40          /* shown as 2.40 */
#else
#define PATCHED_REV 32          /* shown as 2.32 */
#endif
#define PORT_NAME  "MTecPatch"
#define LIMIT      0x80000000UL
#define MAX_BLOCKS 0x8000UL
#define TRIES      3
#define STACK      4096
#define HFERR_BadStatus 45
#define HEADS      255
#define DEV_CHANGE_LIST 0xC0    /* driver's list of change interrupts */
#define DEV_CHANGE_NUM  0xD8    /* driver's change counter per unit */
#define REMOVE_NODE     0x34    /* size of its TD_REMOVE list node */
#define TRACK_SECS 63

extern struct ExecBase *SysBase;
#ifdef ROM_BUILD
extern struct DosLibrary *DOSBase;  /* never used */
#else
struct DosLibrary *DOSBase;
#endif

#include "gen/stubs.h"

LONG start(void);

static const char vers[] __attribute__((used)) = "$VER: MTecPatch 1.3 (2.11.2026)";

#define P(fmt, ...) do { ULONG a_[] = { 0, ##__VA_ARGS__ }; VPrintf((STRPTR)fmt, (LONG *)(a_ + 1)); } while (0)

__attribute__((used)) void *memset(void *d, int c, unsigned long n)
{
    UBYTE *p = d;
    while (n--) *p++ = (UBYTE)c;
    return d;
}

struct Patch {
    struct MsgPort port;        /* public, so a second run can find it */
    struct MsgPort reply;
    struct Task task;
    struct Task *parent;
    ULONG passed, high32, td64, nsd, query, geometry, other;
    ULONG scsi_ops, std_ops, retries, errors;
    UBYTE shift[256];           /* log2 of the block size, 0 until asked */
    UBYTE stack[STACK];
};

#ifdef ROM_BUILD
static const char patched_id[] = "mtecscsi.device 2.40 (2.3 with MTecPatch 1.3 in ROM)\r\n";
#else
static const char patched_id[] = "mtecscsi.device 2.32 (2.3 with MTecPatch 1.3)\r\n";
#endif

extern struct Patch *pt;
extern void *old_beginio;

static const UWORD supported[] = {
    CMD_RESET, CMD_READ, CMD_WRITE, CMD_UPDATE, CMD_CLEAR, CMD_STOP, CMD_START,
    CMD_FLUSH, TD_MOTOR, TD_SEEK, TD_FORMAT, TD_REMOVE, TD_CHANGENUM,
    TD_CHANGESTATE, TD_PROTSTATUS, TD_GETDRIVETYPE, TD_ADDCHANGEINT,
    TD_REMCHANGEINT, TD_GETGEOMETRY, TD_GETNUMTRACKS, TD_EJECT, HD_SCSICMD,
    TD_READ64, TD_WRITE64, TD_SEEK64, TD_FORMAT64,
    ETD_READ, ETD_WRITE, ETD_MOTOR, ETD_SEEK, ETD_FORMAT, ETD_UPDATE, ETD_CLEAR,
    NSCMD_DEVICEQUERY, NSCMD_TD_READ64, NSCMD_TD_WRITE64,
    NSCMD_TD_SEEK64, NSCMD_TD_FORMAT64,
    NSCMD_ETD_READ64, NSCMD_ETD_WRITE64, NSCMD_ETD_SEEK64, NSCMD_ETD_FORMAT64, 0
};

/* Runs in the caller's context. Returns 1 if the request now belongs to us. */
__attribute__((used)) ULONG route(struct IOStdReq *io)
{
    ULONG o = io->io_Offset;

    /* The patch task's own requests go straight to the driver: sent back to
       the task, they would wait for it forever. */
    if (io->io_Message.mn_ReplyPort == &pt->reply)
        return 0;

    switch (io->io_Command) {
    case CMD_READ: case CMD_WRITE: case TD_FORMAT:
        if (o < LIMIT && io->io_Length <= LIMIT - o) {
            pt->passed++;
            return 0;
        }
        pt->high32++;
        break;
    case TD_SEEK:
        if (o < LIMIT) {
            pt->passed++;
            return 0;
        }
        pt->high32++;
        break;
    case TD_READ64: case TD_WRITE64: case TD_SEEK64: case TD_FORMAT64:
        pt->td64++;
        break;
    case NSCMD_TD_READ64: case NSCMD_TD_WRITE64:
    case NSCMD_TD_SEEK64: case NSCMD_TD_FORMAT64:
        pt->nsd++;
        break;
    case NSCMD_DEVICEQUERY:
        pt->query++;
        break;
    case TD_GETGEOMETRY: case TD_GETNUMTRACKS:
        pt->geometry++;
        break;
    case TD_EJECT:
    case ETD_READ: case ETD_WRITE: case ETD_MOTOR: case ETD_SEEK:
    case ETD_FORMAT: case ETD_UPDATE: case ETD_CLEAR:
    case NSCMD_ETD_READ64: case NSCMD_ETD_WRITE64:
    case NSCMD_ETD_SEEK64: case NSCMD_ETD_FORMAT64:
        pt->other++;
        break;
    case TD_REMOVE:
        if (io->io_Data) {
            pt->passed++;
            return 0;
        }
        pt->other++;
        break;
    case HD_SCSICMD:
        if (((struct SCSICmd *)io->io_Data)->scsi_Flags & SCSIF_OLDAUTOSENSE) {
            pt->passed++;
            return 0;
        }
        pt->other++;
        break;
    default:
        return 0;
    }
    io->io_Flags &= ~IOF_QUICK;
    io->io_Error = 0;
    PutMsg(&pt->port, &io->io_Message);
    return 1;
}

/* a1 = request, a6 = device. */
__asm(
"  .text\n"
"_new_beginio:\n"
"  move.l a1,-(sp)\n"
"  move.l a1,-(sp)\n"
"  jsr _route(pc)\n"
"  addq.l #4,sp\n"
"  move.l (sp)+,a1\n"
"  tst.l d0\n"
"  bne.s 1f\n"
"  move.l _old_beginio(pc),a0\n"
"  jmp (a0)\n"
"1: rts\n"
);
extern void new_beginio(void);

static LONG scsi(struct IOStdReq *r, UBYTE *cdb, UWORD cdblen, void *data, ULONG len, UBYTE flags)
{
    struct SCSICmd c;
    UBYTE sense[18];

    memset(&c, 0, sizeof(c));
    c.scsi_Data = data;
    c.scsi_Length = len;
    c.scsi_Command = cdb;
    c.scsi_CmdLength = cdblen;
    c.scsi_Flags = flags | SCSIF_AUTOSENSE;
    c.scsi_SenseData = sense;
    c.scsi_SenseLength = sizeof(sense);
    r->io_Command = HD_SCSICMD;
    r->io_Data = &c;
    r->io_Length = sizeof(c);
    r->io_Flags = 0;
    DoIO((struct IORequest *)r);
    if (r->io_Error)
        return r->io_Error;
    if (c.scsi_Status || c.scsi_Actual < len)
        return HFERR_BadStatus;
    return 0;
}

/* The driver may store more than the 8 bytes asked for, so cap is 16 longs. */
static LONG read_capacity(struct IOStdReq *r, ULONG *cap)
{
    UBYTE cdb[10];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    return scsi(r, cdb, 10, cap, 8, SCSIF_READ);
}

static UBYTE block_shift(struct IOStdReq *r)
{
    UBYTE *s = &pt->shift[(ULONG)r->io_Unit & 0xFF];

    if (!*s) {
        ULONG cap[16];
        UBYTE n = 9, i;
        if (!read_capacity(r, cap))
            for (i = 9; i < 16; i++)
                if (cap[1] == 1UL << i) n = i;
        *s = n;
    }
    return *s;
}

/* One transfer that lies wholly at or above 2 GB. */
static LONG rw10(struct IOStdReq *r, ULONG lba, ULONG blocks, UBYTE shift, UBYTE *data, BOOL wr)
{
    UBYTE cdb[10];
    int i;
    LONG err = 0;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = wr ? 0x2A : 0x28;
    cdb[2] = lba >> 24; cdb[3] = lba >> 16; cdb[4] = lba >> 8; cdb[5] = lba;
    cdb[7] = blocks >> 8; cdb[8] = blocks;
    for (i = 0; i < TRIES; i++) {
        err = scsi(r, cdb, 10, data, blocks << shift, wr ? SCSIF_WRITE : SCSIF_READ);
        pt->scsi_ops++;
        if (!err) break;
        pt->retries++;
    }
    return err;
}

static void transfer(struct IOStdReq *io, struct IOStdReq *r, ULONG hi, ULONG lo, BOOL wr)
{
    ULONG left = io->io_Length, done = 0;
    UBYTE *data = io->io_Data;
    UBYTE shift = block_shift(r);
    ULONG mask = (1UL << shift) - 1;
    LONG err = 0;

    if ((lo | left) & mask) {
        io->io_Error = IOERR_BADLENGTH;
        io->io_Actual = 0;
        pt->errors++;
        return;
    }
    while (left && !err) {
        ULONG n;
        if (!hi && lo < LIMIT) {
            n = LIMIT - lo;
            if (n > left) n = left;
            r->io_Command = wr ? CMD_WRITE : CMD_READ;
            r->io_Offset = lo;
            r->io_Length = n;
            r->io_Data = data;
            r->io_Flags = 0;
            DoIO((struct IORequest *)r);
            err = r->io_Error;
            pt->std_ops++;
        } else {
            ULONG lba, blocks = left >> shift;
            if (hi >> shift) {
                err = IOERR_BADADDRESS;
                break;
            }
            lba = (hi << (32 - shift)) | (lo >> shift);
            if (blocks > MAX_BLOCKS) blocks = MAX_BLOCKS;
            n = blocks << shift;
            err = rw10(r, lba, blocks, shift, data, wr);
        }
        if (err) break;
        data += n;
        done += n;
        left -= n;
        lo += n;
        if (lo < n) hi++;
    }
    io->io_Error = err;
    io->io_Actual = done;
    if (err) pt->errors++;
}

static void query(struct IOStdReq *io)
{
    struct NSDeviceQueryResult *q = (struct NSDeviceQueryResult *)io->io_Data;

    if (!q || io->io_Length < sizeof(*q)) {
        io->io_Error = IOERR_BADLENGTH;
        return;
    }
    q->nsdqr_DevQueryFormat = 0;
    q->nsdqr_SizeAvailable = sizeof(*q);
    q->nsdqr_DeviceType = NSDEVTYPE_TRACKDISK;
    q->nsdqr_DeviceSubType = 0;
    q->nsdqr_SupportedCommands = (UWORD *)supported;
    io->io_Actual = sizeof(*q);
}

static void geometry(struct IOStdReq *io, struct IOStdReq *r)
{
    struct DriveGeometry *g = (struct DriveGeometry *)io->io_Data;
    ULONG cap[16], total;
    UBYTE inq[64], cdb[6];
    BOOL full = io->io_Command == TD_GETGEOMETRY;
    LONG err;

    if (full && (!g || io->io_Length < sizeof(*g))) {
        io->io_Error = IOERR_BADLENGTH;
        pt->errors++;
        return;
    }
    err = read_capacity(r, cap);
    if (err) {
        io->io_Error = err;
        pt->errors++;
        return;
    }
    total = cap[0] == 0xFFFFFFFFUL ? cap[0] : cap[0] + 1;
    if (!full) {
        io->io_Actual = total / TRACK_SECS;
        return;
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12;
    cdb[4] = 16;
    err = scsi(r, cdb, 6, inq, 16, SCSIF_READ);
    if (err) {
        io->io_Error = err;
        pt->errors++;
        return;
    }
    g->dg_SectorSize = cap[1];
    g->dg_TotalSectors = total;
    g->dg_Cylinders = total / (HEADS * TRACK_SECS);
    g->dg_CylSectors = HEADS * TRACK_SECS;
    g->dg_Heads = HEADS;
    g->dg_TrackSectors = TRACK_SECS;
    g->dg_BufMemType = MEMF_PUBLIC;
    g->dg_DeviceType = inq[0] & 0x1F;
    g->dg_Flags = inq[1] & 0x80 ? DGF_REMOVABLE : 0;
    g->dg_Reserved = 0;
    io->io_Actual = sizeof(*g);
}

/* Runs a command on the driver as it is and copies the result back. */
static void pass_on(struct IOStdReq *io, struct IOStdReq *r, UWORD cmd)
{
    r->io_Command = cmd;
    r->io_Flags = 0;
    r->io_Data = io->io_Data;
    r->io_Length = io->io_Length;
    r->io_Offset = io->io_Offset;
    r->io_Actual = 0;
    DoIO((struct IORequest *)r);
    io->io_Error = r->io_Error;
    io->io_Actual = r->io_Actual;
}

/* The driver would keep a NULL and Cause() it on the next change, so this takes
   out the handler set with TD_REMOVE instead. Requests that came through
   TD_ADDCHANGEINT stay, they share the list but are real IORequests. */
static void remove_handler(struct IOStdReq *io)
{
    struct Node *n, *next;

    Forbid();
    Disable();
    for (n = *(struct Node **)((UBYTE *)io->io_Device + DEV_CHANGE_LIST); (next = n->ln_Succ); n = next) {
        struct IOStdReq *e = (struct IOStdReq *)n;
        if (e->io_Unit == io->io_Unit && e->io_Command != TD_ADDCHANGEINT) {
            n->ln_Pred->ln_Succ = next;
            next->ln_Pred = n->ln_Pred;
            Enable();
            FreeMem(n, REMOVE_NODE);
            Disable();
        }
    }
    Enable();
    Permit();
}

/* Without autosense the driver leaves the raw SCSI status in io_Error, and a
   selection timeout in scsi_Status. Autosense requests never come here. */
static void scsi_plain(struct IOStdReq *io, struct IOStdReq *r)
{
    struct SCSICmd *c = (struct SCSICmd *)io->io_Data;

    pass_on(io, r, HD_SCSICMD);
    if (io->io_Error == HFERR_SelTimeout)
        c->scsi_Status = 0;
    else if (io->io_Error > 0 && c->scsi_Status)
        io->io_Error = HFERR_BadStatus;
}

static void eject(struct IOStdReq *io, struct IOStdReq *r)
{
    UBYTE cdb[6];
    LONG err;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x1B;              /* START STOP UNIT */
    cdb[4] = io->io_Length ? 2 : 3;
    err = scsi(r, cdb, 6, NULL, 0, SCSIF_READ);
    if (err) {
        io->io_Error = err;
        pt->errors++;
    }
}

/* An ETD_ request fails if the medium changed since the caller's count. */
static BOOL count_ok(struct IOStdReq *io)
{
    ULONG unit = (ULONG)io->io_Unit & 0xFF;
    ULONG now = ((ULONG *)((UBYTE *)io->io_Device + DEV_CHANGE_NUM))[unit];

    if (((struct IOExtTD *)io)->iotd_Count >= now)
        return TRUE;
    io->io_Error = TDERR_DiskChanged;
    pt->errors++;
    return FALSE;
}

static void handle(struct IOStdReq *io)
{
    struct IOStdReq r;
    ULONG hi = 0;
    BOOL wr = FALSE;

    memset(&r, 0, sizeof(r));
    r.io_Message.mn_ReplyPort = &pt->reply;
    r.io_Message.mn_Length = sizeof(r);
    r.io_Device = io->io_Device;
    r.io_Unit = io->io_Unit;

    switch (io->io_Command) {
    case ETD_READ: case ETD_WRITE: case ETD_MOTOR: case ETD_SEEK:
    case ETD_FORMAT: case ETD_UPDATE: case ETD_CLEAR:
    case NSCMD_ETD_READ64: case NSCMD_ETD_WRITE64:
    case NSCMD_ETD_SEEK64: case NSCMD_ETD_FORMAT64:
        if (!count_ok(io)) return;
        break;
    }
    switch (io->io_Command) {
    case TD_READ64: case NSCMD_TD_READ64: case NSCMD_ETD_READ64:
    case TD_WRITE64: case NSCMD_TD_WRITE64: case NSCMD_ETD_WRITE64:
    case TD_FORMAT64: case NSCMD_TD_FORMAT64: case NSCMD_ETD_FORMAT64:
    case TD_SEEK64: case NSCMD_TD_SEEK64: case NSCMD_ETD_SEEK64:
        hi = io->io_Actual;
        break;
    }
    switch (io->io_Command) {
    case TD_EJECT:
        eject(io, &r);
        return;
    case TD_REMOVE:
        remove_handler(io);
        return;
    case HD_SCSICMD:
        scsi_plain(io, &r);
        return;
    case ETD_MOTOR:
        pass_on(io, &r, TD_MOTOR);
        return;
    case ETD_UPDATE:
        pass_on(io, &r, CMD_UPDATE);
        return;
    case ETD_CLEAR:
        pass_on(io, &r, CMD_CLEAR);
        return;
    case NSCMD_DEVICEQUERY:
        query(io);
        return;
    case TD_GETGEOMETRY: case TD_GETNUMTRACKS:
        geometry(io, &r);
        return;
    case TD_SEEK: case TD_SEEK64: case NSCMD_TD_SEEK64:
    case ETD_SEEK: case NSCMD_ETD_SEEK64:
        return;                 /* no head to move from here */
    case CMD_WRITE: case TD_FORMAT: case TD_WRITE64: case TD_FORMAT64:
    case NSCMD_TD_WRITE64: case NSCMD_TD_FORMAT64:
    case ETD_WRITE: case ETD_FORMAT: case NSCMD_ETD_WRITE64: case NSCMD_ETD_FORMAT64:
        wr = TRUE;
        break;
    }
    transfer(io, &r, hi, io->io_Offset, wr);
}

static void task_main(void)
{
    struct Patch *p = pt;
    struct Message *m;

    p->reply.mp_Node.ln_Type = NT_MSGPORT;
    p->reply.mp_Flags = PA_SIGNAL;
    p->reply.mp_SigBit = AllocSignal(-1);
    p->reply.mp_SigTask = &p->task;
    p->reply.mp_MsgList.lh_Head = (struct Node *)&p->reply.mp_MsgList.lh_Tail;
    p->reply.mp_MsgList.lh_TailPred = (struct Node *)&p->reply.mp_MsgList.lh_Head;

    p->port.mp_Node.ln_Name = (char *)PORT_NAME;
    p->port.mp_Node.ln_Pri = 0;
    p->port.mp_Flags = PA_SIGNAL;
    p->port.mp_SigBit = AllocSignal(-1);
    p->port.mp_SigTask = &p->task;
    AddPort(&p->port);
    Signal(p->parent, SIGF_SINGLE);

    for (;;) {
        WaitPort(&p->port);
        while ((m = (struct Message *)GetMsg(&p->port))) {
            handle((struct IOStdReq *)m);
            ReplyMsg(m);
        }
    }
}

#ifndef ROM_BUILD
static void status(struct Patch *p)
{
    P("Requests left to the driver: %lu\n", p->passed);
    P("32-bit requests at 2 GB+:    %lu\n", p->high32);
    P("TD64 requests:               %lu\n", p->td64);
    P("NSD requests:                %lu\n", p->nsd);
    P("NSD device queries:          %lu\n", p->query);
    P("Geometry requests:           %lu\n", p->geometry);
    P("Other added commands:        %lu\n", p->other);
    P("Sent as READ/WRITE(10):      %lu\n", p->scsi_ops);
    P("Sent as CMD_READ/CMD_WRITE:  %lu\n", p->std_ops);
    P("Retries:                     %lu\n", p->retries);
    P("Failed requests:             %lu\n", p->errors);
}

#endif

/* Starts the task, then sends the device's requests through route(). */
static struct Patch *start_patch(struct Library *dev)
{
    struct Task *t;
    struct Patch *p = (struct Patch *)AllocMem(sizeof(*p), MEMF_PUBLIC | MEMF_CLEAR);

    if (!p)
        return NULL;
    pt = p;
    p->parent = (struct Task *)FindTask(NULL);
    t = &p->task;
    t->tc_Node.ln_Type = NT_TASK;
    t->tc_Node.ln_Pri = 5;
    t->tc_Node.ln_Name = (char *)"MTecPatch";
    t->tc_SPLower = p->stack;
    t->tc_SPUpper = p->stack + STACK;
    t->tc_SPReg = t->tc_SPUpper;
    t->tc_MemEntry.lh_Head = (struct Node *)&t->tc_MemEntry.lh_Tail;
    t->tc_MemEntry.lh_TailPred = (struct Node *)&t->tc_MemEntry.lh_Head;
    AddTask(t, task_main, NULL);
    Wait(SIGF_SINGLE);

    old_beginio = (void *)SetFunction(dev, DEV_BEGINIO, new_beginio);
    dev->lib_Revision = PATCHED_REV;
    dev->lib_IdString = (APTR)patched_id;
    return p;
}

#ifdef ROM_BUILD
__attribute__((used)) void rom_install(struct Library *dev)
{
    if (dev->lib_Version == 2 && dev->lib_Revision == 3)
        start_patch(dev);
}
#else
static LONG install(void)
{
    struct Library *dev;
    struct Patch *p;

    Forbid();
    dev = (struct Library *)FindName(&SysBase->DeviceList, (STRPTR)DEV_NAME);
    Permit();
    if (!dev) {
        P(DEV_NAME " is not running\n");
        return RETURN_FAIL;
    }
    if (dev->lib_Version != 2 || dev->lib_Revision != 3) {
        P("Written for " DEV_NAME " 2.3, this is %ld.%ld\n", dev->lib_Version, dev->lib_Revision);
        return RETURN_FAIL;
    }
    p = start_patch(dev);
    if (!p) {
        P("Out of memory\n");
        return RETURN_FAIL;
    }

    /* Keep this code loaded after the program ends. */
    ((struct CommandLineInterface *)BADDR(((struct Process *)p->parent)->pr_CLI))->cli_Module = 0;
    P("MTecPatch 1.3 installed, " DEV_NAME " 2.3 is now %ld.%ld\n", dev->lib_Version, dev->lib_Revision);
    return RETURN_OK;
}

static LONG run(void)
{
    LONG args[1] = { 0 };
    struct RDArgs *rd = (struct RDArgs *)ReadArgs((STRPTR)"STATUS/S", args, NULL);
    struct Patch *p;
    LONG rc = RETURN_OK;

    if (!rd) {
        PrintFault(IoErr(), (STRPTR)"MTecPatch");
        return RETURN_ERROR;
    }
    Forbid();
    p = (struct Patch *)FindPort((STRPTR)PORT_NAME);
    Permit();
    if (args[0]) {
        if (p) status(p);
        else { P("MTecPatch is not installed\n"); rc = RETURN_WARN; }
    } else if (p) {
        P("MTecPatch is already installed\n");
    } else {
        rc = install();
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
#endif
