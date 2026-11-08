/* dev.c - mastercard.device: the exec device, its task and the commands.
 *
 * Unit numbers are target + 10 * LUN, as with scsi.device. Requests that touch
 * the bus go to the driver's task, one at a time; the rest are done at once
 * in BeginIO. Offsets and lengths must be whole blocks.
 *
 * Commands: CMD_READ, CMD_WRITE, TD_FORMAT, TD_SEEK, their ETD forms (which
 * fail with TDERR_DiskChanged after a medium change) and their 64-bit forms
 * (TD64, NSD and NSD ETD), HD_SCSICMD with autosense, TD_GETGEOMETRY, TD_EJECT,
 * TD_CHANGESTATE, TD_CHANGENUM, TD_PROTSTATUS, TD_ADDCHANGEINT,
 * TD_REMCHANGEINT, TD_REMOVE, TD_GETNUMTRACKS, CMD_START, CMD_STOP,
 * NSCMD_DEVICEQUERY, and no-ops for CMD_UPDATE, CMD_CLEAR, TD_MOTOR and
 * CMD_RESET.
 */
#include "mc.h"

/* A device loaded from disk starts here if someone runs it. */
__asm("  .text\n  moveq #-1,d0\n  rts\n");

#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <exec/resident.h>
#include <exec/initializers.h>
#include <libraries/configvars.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/newstyle.h>
#include <hardware/intbits.h>

#ifndef NSCMD_ETD_READ64
#define NSCMD_ETD_READ64   0xE000
#define NSCMD_ETD_WRITE64  0xE001
#define NSCMD_ETD_SEEK64   0xE002
#define NSCMD_ETD_FORMAT64 0xE003
#endif

#define DEV_NAME   "mastercard.device"
#define VERSION    0
#define REVISION   1
#define TASK_STACK 4096
#define MAX_XFER   0x100000UL   /* bytes per SCSI command; data phases are capped separately */
#define MC_PROBE   0x7FF0       /* private: open a unit */
#define MC_MOUNT   0x7FF2       /* private, ROM build: add the partitions at start-up */
#define TRIES      3

struct ExecBase *SysBase;
struct Library *ExpansionBase;

#include "gen/stubs.h"

extern void mc_irq_entry(void);

static const char dev_name[] = DEV_NAME;
static const char dev_id[] = "mastercard.device 0.1 (4.11.2026) M-Tec MasterCard SCSI";
static const char timer_name[] = "timer.device";
static const char exp_name[] = "expansion.library";

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

/* ---- SCSI helpers, task context ---- */

static LONG scsi(struct MC *mc, struct MCUnit *u, UBYTE *cdb, UWORD cdblen, void *data, ULONG len,
                 UBYTE in, ULONG *actual, UBYTE *status);

#if MC_AUTO_SYNC
/* Negotiate once, lazily, so probing and capacity discovery remain on the
   conservative async path. A reject is a valid async result. A transport
   failure resets the bus; remember that attempt for this unit so one failing
   target cannot cause a reset loop. Other units may retry after that reset. */
static void auto_sync(struct MC *mc, struct MCUnit *u)
{
    struct MCSyncReq r;
    LONG err;

    if (u->sync_tried)
        return;
    memset(&r, 0, sizeof r);
    r.ask_period = MC_AUTO_SYNC_PERIOD;
    r.ask_offset = MC_AUTO_SYNC_OFFSET;
    err = esp_sync(mc, u->target, u->lun, &r);
    if (!err) {
        u->sync_period = r.chip_period;
        u->sync_offset = r.offset;
    }
    u->sync_tried = 1;
}
#endif

/* REQUEST SENSE into mc->sense; returns the sense key or -1. */
static LONG sense(struct MC *mc, struct MCUnit *u)
{
    UBYTE cdb[6] = { 0x03, 0, 0, 0, sizeof mc->sense, 0 };
    struct Cmd c;
    memset(&c, 0, sizeof c);
    c.cdb = cdb;
    c.cdblen = 6;
    c.in = 1;
    c.data = mc->sense;
    c.len = sizeof mc->sense;
    c.sync_period = u->sync_period;
    c.sync_offset = u->sync_offset;
    memset(mc->sense, 0, sizeof mc->sense);
    if (esp_exec(mc, u->target, u->lun, &c) || c.status)
        return -1;
    if (c.actual >= 3 && mc->sense[2] == 0x06) {    /* UNIT ATTENTION: maybe a new medium */
        struct Node *n;
        u->changes++;
        u->shift = 0;
        for (n = u->changeints.lh_Head; n->ln_Succ; n = n->ln_Succ) {
            struct IOStdReq *r = (struct IOStdReq *)n;
            if (r->io_Data)
                Cause((struct Interrupt *)r->io_Data);
        }
        if (u->remove)
            Cause(u->remove);
    }
    return c.actual >= 3 ? (mc->sense[2] & 0x0F) : -1;
}

/* One command with retries after UNIT ATTENTION and NOT READY. */
static LONG scsi(struct MC *mc, struct MCUnit *u, UBYTE *cdb, UWORD cdblen, void *data, ULONG len,
                 UBYTE in, ULONG *actual, UBYTE *status)
{
    int t;
    LONG err = 0;
    for (t = 0; t < TRIES; t++) {
        struct Cmd c;
        memset(&c, 0, sizeof c);
        c.cdb = cdb;
        c.cdblen = cdblen;
        c.in = in;
        c.data = data;
        c.len = len;
        if ((u->sync_offset && len >= MC_SYNC_EXT_MIN) ||
            (in && len >= MC_EXT_READ_MIN))
            c.mode = MCMODE_EXT24;
        c.sync_period = u->sync_period;
        c.sync_offset = u->sync_offset;
        err = esp_exec(mc, u->target, u->lun, &c);
        if (actual)
            *actual = c.actual;
        if (status)
            *status = c.status;
        if (err)
            return err;
        if (!c.status)
            return 0;
        if (c.status != 0x02)
            return HFERR_BadStatus;
        {
            LONG key = sense(mc, u);
            if (key != 0x06 && key != 0x02)
                return HFERR_BadStatus;
            if (key == 0x02)
                mc_delay(mc, 500000);
        }
    }
    return err ? err : HFERR_BadStatus;
}

/* Write protection, for TD_PROTSTATUS: CD-ROM and WORM drives are
   read-only, disks report it in the MODE SENSE header. */
static void read_wprot(struct MC *mc, struct MCUnit *u)
{
    UBYTE cdb[6] = { 0x1A, 0, 0x3F, 0, 4, 0 }, hdr[4] = { 0, 0, 0, 0 };
    ULONG actual = 0;
    if (u->type == 0x05 || u->type == 0x04)
        u->wprot = 1;
    else
        u->wprot = !scsi(mc, u, cdb, 6, hdr, 4, 1, &actual, 0) && actual >= 3 && (hdr[2] & 0x80);
}

static LONG read_capacity(struct MC *mc, struct MCUnit *u)
{
    UBYTE cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    ULONG cap[2], bs;
    UBYTE shift = 0;
    LONG err = scsi(mc, u, cdb, 10, cap, 8, 1, 0, 0);
    if (err)
        return err;
    bs = cap[1];
    while (shift < 16 && (1UL << shift) != bs)
        shift++;
    if (shift < 8 || shift > 15)
        return HFERR_BadStatus;         /* odd block size */
    u->shift = shift;
    u->blocks = cap[0] == 0xFFFFFFFFUL ? 0 : cap[0] + 1;
    read_wprot(mc, u);                  /* a new medium may be protected differently */
    return 0;
}

static LONG need_shift(struct MC *mc, struct MCUnit *u)
{
    return u->shift ? 0 : read_capacity(mc, u);
}

static void probe(struct MC *mc, struct IOStdReq *ior)
{
    struct MCUnit tmp, *u;
    UBYTE inq[36];
    UBYTE cdb[6] = { 0x12, 0, 0, 0, sizeof inq, 0 };
    UWORD idx = ior->io_Offset;
    LONG err;

    memset(&tmp, 0, sizeof tmp);
    tmp.target = idx % MAX_TARGET;
    tmp.lun = idx / MAX_TARGET;
    NewList(&tmp.changeints);
    err = scsi(mc, &tmp, cdb, 6, inq, sizeof inq, 1, 0, 0);
    if (!err && (inq[0] & 0xE0))
        err = TDERR_BadUnitNum;         /* no device on this LUN */
    if (err) {
        ior->io_Error = err;
        return;
    }
    u = (struct MCUnit *)AllocMem(sizeof *u, MEMF_PUBLIC | MEMF_CLEAR);
    if (!u) {
        ior->io_Error = IOERR_OPENFAIL;
        return;
    }
    u->target = tmp.target;
    u->lun = tmp.lun;
    u->type = inq[0] & 0x1F;
    u->removable = inq[1] >> 7;
    u->changes = 1;                     /* an ETD count of 0 is always out of date */
    NewList(&u->changeints);
    if (u->type == 0x00 || u->type == 0x07 || u->type == 0x0E)
        read_capacity(mc, u);           /* may fail without a medium */
    mc->units[idx] = u;
}

/* ---- commands, task context ---- */

/* Reads and writes. The offset is in bytes, hi:lo, so the 64-bit commands
   reach past 4 GB. The block number is 32 bits, so READ(10) and WRITE(10)
   cover 2 TB with 512-byte blocks. */
static void rw(struct MC *mc, struct MCUnit *u, struct IOStdReq *ior, ULONG hi, ULONG lo, UBYTE write)
{
    UBYTE *data = ior->io_Data;
    ULONG len = ior->io_Length, done = 0, bs, lba;
    UBYTE sh;
    LONG err;

    ior->io_Actual = 0;
    if ((err = need_shift(mc, u))) {
        ior->io_Error = err;
        return;
    }
    sh = u->shift;
    bs = 1UL << sh;
    if ((lo | len) & (bs - 1)) {
        ior->io_Error = IOERR_BADLENGTH;
        return;
    }
    lba = hi << (32 - sh) | lo >> sh;
    if (hi >> sh || lba + (len >> sh) < lba) {
        ior->io_Error = IOERR_BADADDRESS;   /* past 2^32 blocks */
        return;
    }
#if MC_AUTO_SYNC
    auto_sync(mc, u);
#endif
    while (done < len) {
        ULONG n = len - done, actual = 0, blocks;
        UBYTE cdb[10];
        if (n > MAX_XFER)
            n = MAX_XFER;
        blocks = n >> sh;
        cdb[0] = write ? 0x2A : 0x28;
        cdb[1] = 0;
        cdb[2] = lba >> 24; cdb[3] = lba >> 16; cdb[4] = lba >> 8; cdb[5] = lba;
        cdb[6] = 0;
        cdb[7] = blocks >> 8; cdb[8] = blocks;
        cdb[9] = 0;
        err = scsi(mc, u, cdb, 10, data + done, n, !write, &actual, 0);
        done += actual;
        if (err) {
            ior->io_Error = err;
            break;
        }
        if (actual != n) {
            ior->io_Error = HFERR_DMA;
            break;
        }
        lba += blocks;
    }
    ior->io_Actual = done;
}

static void scsicmd(struct MC *mc, struct MCUnit *u, struct IOStdReq *ior)
{
    struct SCSICmd *sc = ior->io_Data;
    struct Cmd c;
    LONG err;

    if (ior->io_Length < sizeof *sc) {
        ior->io_Error = IOERR_BADLENGTH;
        return;
    }
    if (!sc->scsi_Command || !sc->scsi_CmdLength || sc->scsi_CmdLength > 16) {
        ior->io_Error = IOERR_BADLENGTH;
        return;
    }
    memset(&c, 0, sizeof c);
    c.cdb = sc->scsi_Command;
    c.cdblen = sc->scsi_CmdLength;
    c.in = sc->scsi_Flags & SCSIF_READ;
    c.data = (UBYTE *)sc->scsi_Data;
    c.len = sc->scsi_Length;
    c.mode = ((u->sync_offset && c.len >= MC_SYNC_EXT_MIN) ||
              (c.in && c.len >= MC_EXT_READ_MIN)) ? MCMODE_EXT24 : MCMODE_DMA16;
    c.sync_period = u->sync_period;
    c.sync_offset = u->sync_offset;
    sc->scsi_Actual = 0;
    sc->scsi_CmdActual = 0;
    sc->scsi_SenseActual = 0;
    err = esp_exec(mc, u->target, u->lun, &c);
    sc->scsi_Actual = c.actual;
    sc->scsi_Status = err ? 0 : c.status;  /* no status byte without a status phase */
    if (err) {
        ior->io_Error = err;
        return;
    }
    sc->scsi_CmdActual = c.cdblen;
    if (c.status) {
        if (c.status == 0x02 && (sc->scsi_Flags & SCSIF_AUTOSENSE) && sc->scsi_SenseData) {
            UWORD n = sc->scsi_SenseLength, k;
            sense(mc, u);
            if (n > sizeof mc->sense)
                n = sizeof mc->sense;
            for (k = 0; k < n; k++)
                sc->scsi_SenseData[k] = mc->sense[k];
            sc->scsi_SenseActual = n;
        }
        ior->io_Error = HFERR_BadStatus;
    }
    ior->io_Actual = ior->io_Length;
}

static void geometry(struct MC *mc, struct MCUnit *u, struct IOStdReq *ior)
{
    struct DriveGeometry *g = ior->io_Data;
    ULONG total, heads = 16, secs = 63;
    LONG err;

    if (ior->io_Length < sizeof *g) {
        ior->io_Error = IOERR_BADLENGTH;
        return;
    }
    u->shift = 0;                       /* read it again: the medium may have changed */
    if ((err = read_capacity(mc, u))) {
        ior->io_Error = err;
        return;
    }
    total = u->blocks ? u->blocks : 0xFFFFFFFFUL;
    memset(g, 0, sizeof *g);
    g->dg_SectorSize = 1UL << u->shift;
    g->dg_TotalSectors = total;
    g->dg_Heads = heads;
    g->dg_TrackSectors = secs;
    g->dg_CylSectors = heads * secs;
    g->dg_Cylinders = total / (heads * secs);
    g->dg_BufMemType = MEMF_PUBLIC;
    g->dg_DeviceType = u->type;
    g->dg_Flags = u->removable ? DGF_REMOVABLE : 0;
    ior->io_Actual = sizeof *g;
}

static void unit_cmd(struct MC *mc, struct MCUnit *u, struct IOStdReq *ior, UBYTE op, UBYTE arg)
{
    UBYTE cdb[6] = { op, 0, 0, 0, arg, 0 };
    ior->io_Error = scsi(mc, u, cdb, 6, 0, 0, 0, 0, 0);
}

/* MCCMD_DIAG: one SCSI command in the chosen transfer mode, for MCDiag. */
static void diag(struct MC *mc, struct MCUnit *u, struct IOStdReq *ior)
{
    struct MCDiagReq *d = ior->io_Data;
    struct Cmd c;
    ULONG s0;

    if (ior->io_Length < sizeof *d || d->mode >= MCMODES || d->cdblen > 16) {
        ior->io_Error = IOERR_BADLENGTH;
        return;
    }
    memset(&d->st, 0, sizeof d->st);
    memset(&c, 0, sizeof c);
    s0 = mc->sleeps;
    c.cdb = d->cdb;
    c.cdblen = d->cdblen;
    c.in = d->in;
    c.data = d->data;
    c.len = d->len;
    c.mode = d->mode;
    c.sync_period = u->sync_period;
    c.sync_offset = u->sync_offset;
    c.st = &d->st;
    d->err = esp_exec(mc, u->target, u->lun, &c);
    d->actual = c.actual;
    d->status = c.status;
    d->st.stray = mc->stray;
    d->st.sleeps = mc->sleeps - s0;
    ior->io_Actual = sizeof *d;
}

#if MC_SYNC_DIAG
/* MCCMD_SYNC: MCDiag-only conservative SDTR negotiation. */
static void syncdiag(struct MC *mc, struct MCUnit *u, struct IOStdReq *ior)
{
    struct MCSyncReq *r = ior->io_Data;

    if (ior->io_Length < sizeof *r) {
        ior->io_Error = IOERR_BADLENGTH;
        return;
    }
    if (r->ask_offset == 0xFF) {        /* private read-only status query */
        r->period = 0;
        r->offset = u->sync_offset;
        r->chip_period = u->sync_period;
        r->step = r->stat = r->intr = r->seq = 0;
        r->err = 0;
        ior->io_Actual = sizeof *r;
        return;
    }
    if (r->ask_offset > 15 ||
        (r->ask_offset && r->ask_period < 25)) {
        ior->io_Error = IOERR_BADLENGTH;
        return;
    }
    r->err = esp_sync(mc, u->target, u->lun, r);
    if (!r->err) {
        u->sync_period = r->chip_period;
        u->sync_offset = r->offset;
#if MC_AUTO_SYNC
        u->sync_tried = 1;
#endif
    }
    ior->io_Actual = sizeof *r;
}
#endif

#ifdef ROM_BUILD
#include "boot.c"
#endif

static void perform(struct MC *mc, struct IOStdReq *ior)
{
    struct MCUnit *u = (struct MCUnit *)ior->io_Unit;
    ULONG hi = ior->io_Actual;          /* high half of the offset, 64-bit commands */

    ior->io_Error = 0;
    switch (ior->io_Command) {
    case MC_PROBE:
        probe(mc, ior);
        break;
#ifdef ROM_BUILD
    case MC_MOUNT:
        mount_all(mc, ior);
        break;
#endif
    case CMD_READ:
    case ETD_READ:
        rw(mc, u, ior, 0, ior->io_Offset, 0);
        break;
    case CMD_WRITE:
    case TD_FORMAT:
    case ETD_WRITE:
    case ETD_FORMAT:
        rw(mc, u, ior, 0, ior->io_Offset, 1);
        break;
    case TD_READ64:
    case NSCMD_TD_READ64:
    case NSCMD_ETD_READ64:
        rw(mc, u, ior, hi, ior->io_Offset, 0);
        break;
    case TD_WRITE64:
    case TD_FORMAT64:
    case NSCMD_TD_WRITE64:
    case NSCMD_TD_FORMAT64:
    case NSCMD_ETD_WRITE64:
    case NSCMD_ETD_FORMAT64:
        rw(mc, u, ior, hi, ior->io_Offset, 1);
        break;
    case HD_SCSICMD:
        scsicmd(mc, u, ior);
        break;
    case MCCMD_DIAG:
        diag(mc, u, ior);
        break;
#if MC_SYNC_DIAG
    case MCCMD_SYNC:
        syncdiag(mc, u, ior);
        break;
#endif
    case TD_GETGEOMETRY:
        geometry(mc, u, ior);
        break;
    case TD_EJECT:
        unit_cmd(mc, u, ior, 0x1B, ior->io_Length ? 0x02 : 0x03);
        break;
    case CMD_START:
        unit_cmd(mc, u, ior, 0x1B, 0x01);
        break;
    case CMD_STOP:
        unit_cmd(mc, u, ior, 0x1B, 0x00);
        break;
    case TD_CHANGESTATE: {
        UBYTE cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
        ior->io_Actual = scsi(mc, u, cdb, 6, 0, 0, 0, 0, 0) ? 1 : 0;
        break;
    }
    case TD_GETNUMTRACKS:               /* tracks of 63 blocks, as TD_GETGEOMETRY reports */
        if (!(ior->io_Error = read_capacity(mc, u)))
            ior->io_Actual = (u->blocks ? u->blocks : 0xFFFFFFFFUL) / 63;
        break;
    case TD_SEEK:
    case ETD_SEEK:
    case TD_SEEK64:
    case NSCMD_TD_SEEK64:
    case NSCMD_ETD_SEEK64:
        break;
    default:
        ior->io_Error = IOERR_NOCMD;
    }
}

/* ---- the task ---- */

static void task_main(void)
{
    struct Task *me = (struct Task *)FindTask(0);
    struct MC *mc = me->tc_UserData;
    LONG psig = AllocSignal(-1), isig = AllocSignal(-1), tsig = AllocSignal(-1);
    struct MsgPort reqport;

    if (psig < 0 || isig < 0 || tsig < 0)
        goto fail;
    memset(&reqport, 0, sizeof reqport);
    reqport.mp_Node.ln_Type = NT_MSGPORT;
    reqport.mp_Flags = PA_SIGNAL;
    reqport.mp_SigBit = psig;
    reqport.mp_SigTask = me;
    NewList(&reqport.mp_MsgList);
    mc->tport.mp_Node.ln_Type = NT_MSGPORT;
    mc->tport.mp_Flags = PA_SIGNAL;
    mc->tport.mp_SigBit = tsig;
    mc->tport.mp_SigTask = me;
    NewList(&mc->tport.mp_MsgList);
    mc->treq.tr_node.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    mc->treq.tr_node.io_Message.mn_ReplyPort = &mc->tport;
    mc->treq.tr_node.io_Message.mn_Length = sizeof mc->treq;
    if (OpenDevice((STRPTR)timer_name, UNIT_MICROHZ, (struct IORequest *)&mc->treq, 0))
        goto fail;
    mc->timer_open = 1;
    mc->irqsig = 1UL << isig;
    mc->port = &reqport;
    esp_init(mc);
    Signal(mc->starter, SIGF_SINGLE);
    esp_busreset(mc);                   /* a target may be stuck from before */
    for (;;) {
        struct IOStdReq *ior;
        WaitPort(&reqport);
        while ((ior = (struct IOStdReq *)GetMsg(&reqport)) != 0) {
            perform(mc, ior);
            ReplyMsg(&ior->io_Message);
        }
    }
fail:
    mc->port = 0;
    Signal(mc->starter, SIGF_SINGLE);
    Wait(0);
}

static int start_task(struct MC *mc)
{
    struct Task *t = (struct Task *)AllocMem(sizeof *t + TASK_STACK, MEMF_PUBLIC | MEMF_CLEAR);
    UBYTE *stack = (UBYTE *)(t + 1);
    if (!t)
        return 0;
    t->tc_Node.ln_Type = NT_TASK;
    t->tc_Node.ln_Pri = 10;
    t->tc_Node.ln_Name = (char *)dev_name;
    t->tc_SPLower = stack;
    t->tc_SPUpper = stack + TASK_STACK;
    t->tc_SPReg = stack + TASK_STACK;
    t->tc_UserData = mc;
    mc->task = t;
    mc->starter = (struct Task *)FindTask(0);
    SetSignal(0, SIGF_SINGLE);
    AddTask(t, (APTR)task_main, 0);
    Wait(SIGF_SINGLE);
    return mc->port != 0;
}

/* ---- device vectors ---- */

static struct MC *dev_init(struct MC *mc __asm("d0"), BPTR seg __asm("a0"), struct ExecBase *sb __asm("a6"))
{
    struct ConfigDev *cd;

    SysBase = sb;
    mc->lib.lib_Node.ln_Type = NT_DEVICE;
    mc->lib.lib_Node.ln_Name = (char *)dev_name;
    mc->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    mc->lib.lib_Version = VERSION;
    mc->lib.lib_Revision = REVISION;
    mc->lib.lib_IdString = (APTR)dev_id;
    mc->seglist = seg;
    cd = 0;
    if ((ExpansionBase = (struct Library *)OpenLibrary((STRPTR)exp_name, 0))) {
        cd = (struct ConfigDev *)FindConfigDev(0, MC_MANUFACTURER, MC_PRODUCT);
        CloseLibrary(ExpansionBase);
    }
    /* The ROM's own driver must not be running, or both would drive the chip. */
    if (!cd || cd->cd_Driver || FindName(&SysBase->DeviceList, (STRPTR)"mtecscsi.device"))
        goto fail;
    mc->board = cd->cd_BoardAddr;
    if (!start_task(mc))
        goto fail;
    cd->cd_Driver = mc;
    cd->cd_Flags &= ~CDF_CONFIGME;
    mc->irq.is_Node.ln_Type = NT_INTERRUPT;
    mc->irq.is_Node.ln_Pri = 20;
    mc->irq.is_Node.ln_Name = (char *)dev_name;
    mc->irq.is_Data = mc;
    mc->irq.is_Code = mc_irq_entry;
    AddIntServer(INTB_PORTS, &mc->irq);
#ifdef ROM_BUILD
    {
        /* Mount the partitions now, before dos.library starts booting. The
           task does the SCSI work; this waits for it. */
        struct MsgPort mp;
        struct IOStdReq mr;
        memset(&mp, 0, sizeof mp);
        mp.mp_Node.ln_Type = NT_MSGPORT;
        mp.mp_Flags = PA_SIGNAL;
        mp.mp_SigBit = SIGB_SINGLE;
        mp.mp_SigTask = (struct Task *)FindTask(0);
        NewList(&mp.mp_MsgList);
        memset(&mr, 0, sizeof mr);
        mr.io_Message.mn_ReplyPort = &mp;
        mr.io_Message.mn_Length = sizeof mr;
        mr.io_Device = (struct Device *)&mc->lib;
        mr.io_Command = MC_MOUNT;
        mr.io_Data = cd;
        SetSignal(0, SIGF_SINGLE);
        PutMsg(mc->port, &mr.io_Message);
        WaitPort(&mp);
        GetMsg(&mp);
    }
#endif
    return mc;
fail:
    FreeMem((UBYTE *)mc - mc->lib.lib_NegSize, mc->lib.lib_NegSize + mc->lib.lib_PosSize);
    return 0;
}

static void dev_open(struct IOStdReq *ior __asm("a1"), ULONG unit __asm("d0"), ULONG flags __asm("d1"),
                     struct MC *mc __asm("a6"))
{
    ULONG target = unit % 10, lun = unit / 10;
    UWORD idx;
    (void)flags;

    ior->io_Error = IOERR_OPENFAIL;
    if (target >= MAX_TARGET || target == OWN_ID || lun >= MAX_LUN)
        return;
    idx = lun * MAX_TARGET + target;
    mc->lib.lib_OpenCnt++;              /* keeps us while we wait */
    if (!mc->units[idx]) {
        struct MsgPort mp;
        struct IOStdReq pr;
        memset(&mp, 0, sizeof mp);
        mp.mp_Node.ln_Type = NT_MSGPORT;
        mp.mp_Flags = PA_SIGNAL;
        mp.mp_SigBit = SIGB_SINGLE;
        mp.mp_SigTask = (struct Task *)FindTask(0);
        NewList(&mp.mp_MsgList);
        memset(&pr, 0, sizeof pr);
        pr.io_Message.mn_ReplyPort = &mp;
        pr.io_Message.mn_Length = sizeof pr;
        pr.io_Device = (struct Device *)&mc->lib;
        pr.io_Command = MC_PROBE;
        pr.io_Offset = idx;
        SetSignal(0, SIGF_SINGLE);
        PutMsg(mc->port, &pr.io_Message);
        WaitPort(&mp);
        GetMsg(&mp);
        if (!mc->units[idx]) {
            mc->lib.lib_OpenCnt--;
            ior->io_Error = pr.io_Error ? pr.io_Error : IOERR_OPENFAIL;
            return;
        }
    }
    ior->io_Unit = &mc->units[idx]->u;
    mc->units[idx]->u.unit_OpenCnt++;
    mc->lib.lib_Flags &= ~LIBF_DELEXP;
    ior->io_Error = 0;
}

static BPTR dev_close(struct IOStdReq *ior __asm("a1"), struct MC *mc __asm("a6"))
{
    struct MCUnit *u = (struct MCUnit *)ior->io_Unit;
    if (u)
        u->u.unit_OpenCnt--;
    mc->lib.lib_OpenCnt--;
    ior->io_Unit = (APTR)-1;
    ior->io_Device = (APTR)-1;
    return 0;                           /* stays loaded: it owns the hardware */
}

static BPTR dev_expunge(struct MC *mc __asm("a6"))
{
    (void)mc;
    return 0;
}

static ULONG dev_null(void)
{
    return 0;
}

static const UWORD cmds[] = {
    CMD_RESET, CMD_READ, CMD_WRITE, CMD_UPDATE, CMD_CLEAR, CMD_STOP, CMD_START,
    TD_MOTOR, TD_SEEK, TD_FORMAT, TD_REMOVE, TD_CHANGENUM, TD_CHANGESTATE, TD_GETNUMTRACKS,
    TD_PROTSTATUS, TD_ADDCHANGEINT, TD_REMCHANGEINT, TD_GETGEOMETRY, TD_EJECT,
    ETD_READ, ETD_WRITE, ETD_SEEK, ETD_FORMAT, ETD_UPDATE, ETD_CLEAR, ETD_MOTOR,
    TD_READ64, TD_WRITE64, TD_SEEK64, TD_FORMAT64, HD_SCSICMD,
    NSCMD_DEVICEQUERY, NSCMD_TD_READ64, NSCMD_TD_WRITE64, NSCMD_TD_SEEK64, NSCMD_TD_FORMAT64,
    NSCMD_ETD_READ64, NSCMD_ETD_WRITE64, NSCMD_ETD_SEEK64, NSCMD_ETD_FORMAT64, 0
};

static void dev_beginio(struct IOStdReq *ior __asm("a1"), struct MC *mc __asm("a6"))
{
    struct MCUnit *u = (struct MCUnit *)ior->io_Unit;

    ior->io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ior->io_Error = 0;
    /* The ETD commands fail once the medium changed after iotd_Count. */
    if (((ior->io_Command & TDF_EXTCOM) || (ior->io_Command & 0xFFF0) == NSCMD_ETD_READ64) &&
        (LONG)((struct IOExtTD *)ior)->iotd_Count < (LONG)u->changes) {
        ior->io_Error = TDERR_DiskChanged;
        goto reply;
    }
    switch (ior->io_Command) {
    case CMD_RESET:
    case CMD_UPDATE:
    case CMD_CLEAR:
    case ETD_UPDATE:
    case ETD_CLEAR:
    case ETD_MOTOR:
    case TD_MOTOR:
        ior->io_Actual = 0;
        break;
    case TD_PROTSTATUS:                 /* answered at once, as callers expect */
        ior->io_Actual = u->wprot;
        break;
    case TD_REMOVE:                     /* the old single change interrupt; 0 removes it */
        u->remove = ior->io_Data;
        ior->io_Actual = 0;
        break;
    case TD_CHANGENUM:
        ior->io_Actual = u->changes;
        break;
    case NSCMD_DEVICEQUERY: {
        struct NSDeviceQueryResult *q = ior->io_Data;
        if (ior->io_Length < sizeof *q) {
            ior->io_Error = IOERR_BADLENGTH;
            break;
        }
        q->nsdqr_DevQueryFormat = 0;
        q->nsdqr_SizeAvailable = sizeof *q;
        q->nsdqr_DeviceType = NSDEVTYPE_TRACKDISK;
        q->nsdqr_DeviceSubType = 0;
        q->nsdqr_SupportedCommands = (UWORD *)cmds;
        ior->io_Actual = sizeof *q;
        break;
    }
    case TD_ADDCHANGEINT:
        Forbid();
        AddTail(&u->changeints, &ior->io_Message.mn_Node);
        Permit();
        ior->io_Flags &= ~IOF_QUICK;
        return;                         /* replied by TD_REMCHANGEINT */
    case TD_REMCHANGEINT:
        Forbid();
        Remove(&ior->io_Message.mn_Node);
        Permit();
        break;
    default:
        ior->io_Flags &= ~IOF_QUICK;
        PutMsg(mc->port, &ior->io_Message);
        return;
    }
reply:
    if (!(ior->io_Flags & IOF_QUICK))
        ReplyMsg(&ior->io_Message);
}

static ULONG dev_abortio(struct IOStdReq *ior __asm("a1"), struct MC *mc __asm("a6"))
{
    struct Node *n;
    ULONG r = IOERR_NOCMD;
    Forbid();
    for (n = mc->port->mp_MsgList.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (n == &ior->io_Message.mn_Node)
            break;
    if (!n->ln_Succ && ior->io_Command == TD_ADDCHANGEINT && ior->io_Message.mn_Node.ln_Type == NT_MESSAGE)
        n = &ior->io_Message.mn_Node;   /* a waiting change interrupt */
    if (n->ln_Succ) {
        Remove(n);
        ior->io_Error = IOERR_ABORTED;
        ReplyMsg(&ior->io_Message);
        r = 0;
    }
    Permit();
    return r;
}

static const APTR functable[] = {
    (APTR)dev_open, (APTR)dev_close, (APTR)dev_expunge, (APTR)dev_null,
    (APTR)dev_beginio, (APTR)dev_abortio, (APTR)-1
};

static const ULONG inittable[4] = {
    sizeof(struct MC), (ULONG)functable, 0, (ULONG)dev_init
};

const struct Resident romtag __attribute__((used)) = {
    RTC_MATCHWORD, (struct Resident *)&romtag, (APTR)(&romtag + 1), RTF_AUTOINIT,
    VERSION, NT_DEVICE, 0, (char *)dev_name, (char *)dev_id, (APTR)inittable
};
