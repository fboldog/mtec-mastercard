/* MTecTest - tests a large disk on mtecscsi.device with MTecPatch loaded.
 *
 *   MTecTest SCAN                finds the disk, reports its RDB, and writes
 *                                RAM:MountList, RAM:MountAll, RAM:Tests and RAM:WriteTest
 *   MTecTest READ UNIT n         compares every access path, read-only
 *   MTecTest WRITE UNIT n        writes test data at each size mark and puts
 *                                the original data back
 *   MTecTest FILE VOL DH0:       writes, checks and deletes a 4 MB file
 */
__asm("  .text\n  jmp _start\n");

#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <dos/filehandler.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/newstyle.h>
#include <devices/hardblocks.h>

#define DEV_NAME  "mtecscsi.device"
#define LOG       ">>SYS:mtectest.log"
#define SPAN      0x20000UL
#define FILE_SIZE 0x400000UL
#define CHUNK     0x10000UL
#define G4        0x100000000ULL

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

#include "gen/stubs.h"

LONG start(void);

static const char vers[] __attribute__((used)) = "$VER: MTecTest 1.0 (31.10.2026)";

#define P(fmt, ...) do { ULONG a_[] = { 0, ##__VA_ARGS__ }; VPrintf((STRPTR)fmt, (LONG *)(a_ + 1)); } while (0)
#define F(f, fmt, ...) do { ULONG a_[] = { 0, ##__VA_ARGS__ }; VFPrintf(f, (STRPTR)fmt, (LONG *)(a_ + 1)); } while (0)

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

enum { VIA_SCSI, VIA_CMD, VIA_TD64, VIA_NSD, VIAS };

static const char *const via_name[VIAS] = { "SCSI(10)", "CMD", "TD64", "NSD" };

struct Mark { const char *name; ULONG hi, lo; UBYTE write_via; };

static const struct Mark marks[] = {
    { "1 GB", 0, 0x3FFF0000, VIA_CMD },
    { "2 GB", 0, 0x7FFF0000, VIA_CMD },
    { "3 GB", 0, 0xBFFF0000, VIA_CMD },
    { "4 GB", 0, 0xFFFF0000, VIA_TD64 },
    { "5 GB", 1, 0x3FFF0000, VIA_NSD },
    { 0, 0, 0, 0 }
};

static struct MsgPort port;
static struct IOStdReq io;
static ULONG capacity;
static ULONG failures;
static ULONG last_status, last_actual;

static void init_io(void)
{
    port.mp_Node.ln_Type = NT_MSGPORT;
    port.mp_Flags = PA_SIGNAL;
    port.mp_SigBit = AllocSignal(-1);
    port.mp_SigTask = (struct Task *)FindTask(NULL);
    port.mp_MsgList.lh_Head = (struct Node *)&port.mp_MsgList.lh_Tail;
    port.mp_MsgList.lh_TailPred = (struct Node *)&port.mp_MsgList.lh_Head;
    io.io_Message.mn_ReplyPort = &port;
    io.io_Message.mn_Length = sizeof(io);
}

static LONG scsi(UBYTE *cdb, void *data, ULONG len, UBYTE flags)
{
    struct SCSICmd c;
    UBYTE sense[18];

    memset(&c, 0, sizeof(c));
    c.scsi_Data = data;
    c.scsi_Length = len;
    c.scsi_Command = cdb;
    c.scsi_CmdLength = 10;
    c.scsi_Flags = flags | SCSIF_AUTOSENSE;
    c.scsi_SenseData = sense;
    c.scsi_SenseLength = sizeof(sense);
    io.io_Command = HD_SCSICMD;
    io.io_Flags = 0;
    io.io_Data = &c;
    io.io_Length = sizeof(c);
    DoIO((struct IORequest *)&io);
    if (io.io_Error) return io.io_Error;
    last_status = c.scsi_Status;
    last_actual = c.scsi_Actual;
    return c.scsi_Status || c.scsi_Actual < len ? 45 : 0;
}

static LONG rw10(ULONG hi, ULONG lo, ULONG len, void *data, BOOL wr)
{
    UBYTE cdb[10];
    ULONG lba = (hi << 23) | (lo >> 9), n = len >> 9;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = wr ? 0x2A : 0x28;
    cdb[2] = lba >> 24; cdb[3] = lba >> 16; cdb[4] = lba >> 8; cdb[5] = lba;
    cdb[7] = n >> 8; cdb[8] = n;
    return scsi(cdb, data, len, wr ? SCSIF_WRITE : SCSIF_READ);
}

static LONG xfer(int via, ULONG hi, ULONG lo, ULONG len, void *data, BOOL wr)
{
    if (via == VIA_SCSI)
        return rw10(hi, lo, len, data, wr);
    io.io_Command = via == VIA_CMD ? (wr ? CMD_WRITE : CMD_READ)
                  : via == VIA_TD64 ? (wr ? TD_WRITE64 : TD_READ64)
                  : (wr ? NSCMD_TD_WRITE64 : NSCMD_TD_READ64);
    io.io_Flags = 0;
    io.io_Data = data;
    io.io_Length = len;
    io.io_Offset = lo;
    io.io_Actual = hi;
    DoIO((struct IORequest *)&io);
    if (io.io_Error) return io.io_Error;
    return io.io_Actual == len ? 0 : 45;
}

static BOOL same(const UBYTE *a, const UBYTE *b, ULONG n)
{
    while (n--)
        if (*a++ != *b++) return FALSE;
    return TRUE;
}

static BOOL read_capacity(void)
{
    UBYTE cdb[10];
    ULONG cap[16];   /* the driver may store more than the 8 bytes asked for */

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    if (scsi(cdb, cap, 8, SCSIF_READ)) {
        P("Unit: READ CAPACITY failed, error %ld, status %ld, got %ld bytes\n", (LONG)io.io_Error, last_status, last_actual);
        return FALSE;
    }
    if (cap[1] != 512) {
        P("Unit: block size %lu is not supported\n", cap[1]);
        return FALSE;
    }
    capacity = cap[0] + 1;
    return TRUE;
}

static BOOL fits(const struct Mark *m)
{
    unsigned long long end = ((unsigned long long)m->hi << 32) + m->lo + SPAN;
    return end <= (unsigned long long)capacity << 9;
}

static BOOL uses_cmd(const struct Mark *m)
{
    return !m->hi && m->lo + SPAN > m->lo;
}

/* Reads a mark through every path and compares each with buf. */
static void compare_paths(const struct Mark *m, UBYTE *buf, UBYTE *tmp, const char *what)
{
    int v;

    for (v = 0; v < VIAS; v++) {
        LONG e;
        BOOL ok;
        if (v == VIA_CMD && !uses_cmd(m)) continue;
        memset(tmp, 0xA5, SPAN);
        e = xfer(v, m->hi, m->lo, SPAN, tmp, FALSE);
        ok = !e && same(buf, tmp, SPAN);
        P("  %-6s %-8s %-8s err %ld %s\n", (ULONG)m->name, (ULONG)what, (ULONG)via_name[v], e,
          (ULONG)(ok ? "ok" : "FAILED"));
        if (!ok) failures++;
    }
}

/* Shows whether the driver stores more than a short SCSI read asked for. */
static void short_read(const char *name, UBYTE *cdb, ULONG len)
{
    static UBYTE b[64];
    LONG e;
    int i, last = -1;

    memset(b, 0xEE, sizeof(b));
    e = scsi(cdb, b, len, SCSIF_READ);
    for (i = 0; i < (int)sizeof(b); i++)
        if (b[i] != 0xEE) last = i;
    P("  %-13s asked %ld, reported %ld, stored up to byte %ld, err %ld %s\n", (ULONG)name, len, last_actual,
      (LONG)(last + 1), e, (ULONG)(last_actual > len || last + 1 > (int)len ? "OVERRUN" : "ok"));
}

static void short_reads(void)
{
    UBYTE cdb[10];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    short_read("READ CAPACITY", cdb, 8);
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x5A;
    cdb[2] = 0x3F;
    cdb[8] = 12;
    short_read("MODE SENSE", cdb, 12);
}

static void read_marks(void)
{
    const struct Mark *m;
    UBYTE *a = (UBYTE *)AllocMem(SPAN, MEMF_PUBLIC);
    UBYTE *b = (UBYTE *)AllocMem(SPAN, MEMF_PUBLIC);

    if (a && b) {
        for (m = marks; m->name; m++) {
            if (!fits(m)) { P("  %-6s beyond end of disk, skipped\n", (ULONG)m->name); continue; }
            memset(a, 0xA5, SPAN);
            if (rw10(m->hi, m->lo, SPAN, a, FALSE)) {
                P("  %-6s READ(10) failed\n", (ULONG)m->name);
                failures++;
                continue;
            }
            compare_paths(m, a, b, "same");
        }
    } else {
        P("Out of memory\n");
        failures++;
    }
    if (a) FreeMem(a, SPAN);
    if (b) FreeMem(b, SPAN);
}

static void write_marks(void)
{
    const struct Mark *m;
    UBYTE *save = (UBYTE *)AllocMem(SPAN, MEMF_PUBLIC);
    UBYTE *pat = (UBYTE *)AllocMem(SPAN, MEMF_PUBLIC);
    UBYTE *tmp = (UBYTE *)AllocMem(SPAN, MEMF_PUBLIC);
    ULONG i;

    if (save && pat && tmp) {
        for (m = marks; m->name; m++) {
            int w = m->write_via;
            LONG e;
            if (!fits(m)) { P("  %-6s beyond end of disk, skipped\n", (ULONG)m->name); continue; }
            if (rw10(m->hi, m->lo, SPAN, save, FALSE)) {
                P("  %-6s could not save the data, skipped\n", (ULONG)m->name);
                failures++;
                continue;
            }
            for (i = 0; i < SPAN >> 2; i++)
                ((ULONG *)pat)[i] = m->lo + (i << 2) + 0x5A5A0000UL;
            e = xfer(w, m->hi, m->lo, SPAN, pat, TRUE);
            P("  %-6s write via %s err %ld\n", (ULONG)m->name, (ULONG)via_name[w], e);
            if (e) failures++;
            compare_paths(m, pat, tmp, "written");
            e = xfer(w, m->hi, m->lo, SPAN, save, TRUE);
            if (!e) e = rw10(m->hi, m->lo, SPAN, tmp, FALSE);
            if (e || !same(save, tmp, SPAN)) {
                P("  %-6s RESTORE FAILED err %ld: this area is damaged\n", (ULONG)m->name, e);
                failures++;
            } else {
                P("  %-6s original data restored\n", (ULONG)m->name);
            }
        }
    } else {
        P("Out of memory\n");
        failures++;
    }
    if (save) FreeMem(save, SPAN);
    if (pat) FreeMem(pat, SPAN);
    if (tmp) FreeMem(tmp, SPAN);
}

static LONG unit_test(LONG unit, BOOL wr)
{
    if (OpenDevice((STRPTR)DEV_NAME, unit, (struct IORequest *)&io, 0)) {
        P("Cannot open " DEV_NAME " unit %ld\n", unit);
        return RETURN_FAIL;
    }
    if (read_capacity()) {
        P("Unit %ld: %lu MB\n", unit, capacity >> 11);
        if (wr) {
            write_marks();
        } else {
            short_reads();
            read_marks();
        }
    } else {
        P("READ CAPACITY failed or the block size is not 512\n");
        failures++;
    }
    CloseDevice((struct IORequest *)&io);
    P("RESULT %s: %ld failures\n", (ULONG)(wr ? "write test" : "read test"), failures);
    return failures ? RETURN_ERROR : RETURN_OK;
}

static LONG file_test(const char *vol)
{
    static char name[80];
    char *d = name;
    const char *s = vol;
    ULONG *buf = (ULONG *)AllocMem(CHUNK, MEMF_PUBLIC);
    ULONG pos, i, bad = 0;
    BPTR f;

    while (*s && d < name + 60) *d++ = *s++;
    memcpy(d, "mtectest.bin", 13);
    if (!buf) return RETURN_FAIL;

    f = Open((STRPTR)name, MODE_NEWFILE);
    if (!f) {
        P("%s: cannot create the test file, error %ld\n", (ULONG)vol, IoErr());
        FreeMem(buf, CHUNK);
        return RETURN_ERROR;
    }
    for (pos = 0; pos < FILE_SIZE && !bad; pos += CHUNK) {
        for (i = 0; i < CHUNK >> 2; i++) buf[i] = pos + (i << 2) + 0x12340000UL;
        if ((ULONG)Write(f, buf, CHUNK) != CHUNK) {
            P("%s: write error %ld at %lu\n", (ULONG)vol, IoErr(), pos);
            bad++;
        }
    }
    Close(f);
    if (!bad && (f = Open((STRPTR)name, MODE_OLDFILE))) {
        for (pos = 0; pos < FILE_SIZE && !bad; pos += CHUNK) {
            if ((ULONG)Read(f, buf, CHUNK) != CHUNK) {
                P("%s: read error %ld at %lu\n", (ULONG)vol, IoErr(), pos);
                bad++;
            }
            for (i = 0; i < CHUNK >> 2 && !bad; i++)
                if (buf[i] != pos + (i << 2) + 0x12340000UL) {
                    P("%s: wrong data at %lu\n", (ULONG)vol, pos + (i << 2));
                    bad++;
                }
        }
        Close(f);
    } else if (!bad) {
        P("%s: cannot reopen the test file\n", (ULONG)vol);
        bad++;
    }
    DeleteFile((STRPTR)name);
    FreeMem(buf, CHUNK);
    P("RESULT file test %s %s\n", (ULONG)vol, (ULONG)(bad ? "FAILED" : "ok"));
    return bad ? RETURN_ERROR : RETURN_OK;
}

static BOOL mounted(const char *name)
{
    struct DosList *d = (struct DosList *)LockDosList(LDF_DEVICES | LDF_READ);
    BOOL r = FindDosEntry(d, (STRPTR)name, LDF_DEVICES) != 0;
    UnLockDosList(LDF_DEVICES | LDF_READ);
    return r;
}

static BOOL read_block(ULONG n, ULONG *b)
{
    return rw10(0, n << 9, 512, b, FALSE) == 0;
}

static BOOL checksum_ok(ULONG *b)
{
    ULONG i, n = b[1], s = 0;
    if (n < 2 || n > 128) return FALSE;
    for (i = 0; i < n; i++) s += b[i];
    return s == 0;
}

/* A checksum for each of 32 spots spread over the disk. The first run saves
   them in RAM:, later runs must match, so a transfer mode can be compared
   with the one before it on the same data. */
#define CRC_SPOTS 32
#define CRC_LEN   0x80000UL

static LONG crc_test(LONG unit)
{
    static ULONG now[CRC_SPOTS], was[CRC_SPOTS];
    ULONG *b = (ULONG *)AllocMem(CRC_LEN, MEMF_PUBLIC);
    BPTR f;
    int i;
    BOOL saved;
    LONG bad = 0;

    if (!b) return RETURN_FAIL;
    if (OpenDevice((STRPTR)DEV_NAME, unit, (struct IORequest *)&io, 0)) {
        FreeMem(b, CRC_LEN);
        return RETURN_FAIL;
    }
    if (!read_capacity()) bad = 1;
    for (i = 0; i < CRC_SPOTS && !bad; i++) {
        ULONG lba = (capacity - (CRC_LEN >> 9)) / CRC_SPOTS * i, j, s = 0;
        LONG e = xfer(VIA_TD64, lba >> 23, lba << 9, CRC_LEN, b, FALSE);
        if (e) {
            P("  block %lu: read error %ld\n", lba, e);
            bad++;
            break;
        }
        for (j = 0; j < CRC_LEN >> 2; j++)
            s = ((s << 3) | (s >> 29)) + b[j];
        now[i] = s;
    }
    CloseDevice((struct IORequest *)&io);
    FreeMem(b, CRC_LEN);
    f = Open((STRPTR)"RAM:MTecCRC", MODE_OLDFILE);
    saved = f && Read(f, was, sizeof(was)) == sizeof(was);
    if (f) Close(f);
    if (!bad && !saved) {
        f = Open((STRPTR)"RAM:MTecCRC", MODE_NEWFILE);
        if (f) {
            Write(f, now, sizeof(now));
            Close(f);
        }
        P("RESULT checksums saved as the reference\n");
        return RETURN_OK;
    }
    for (i = 0; i < CRC_SPOTS && !bad; i++)
        if (now[i] != was[i]) {
            P("  spot %ld: checksum differs\n", (LONG)i);
            bad++;
        }
    P("RESULT checksums %s\n", (ULONG)(bad ? "FAILED" : "match"));
    return bad ? RETURN_ERROR : RETURN_OK;
}

/* Logs what a partition's first sectors hold, to explain a failed mount. */
static void probe_partition(ULONG first)
{
    static const UBYTE at[] = { 0, 1, 2, 4, 8, 16 };
    static ULONG s[128];
    UWORD i;

    for (i = 0; i < sizeof(at); i++) {
        ULONG lba = first + at[i];
        if (rw10(lba >> 23, lba << 9, 512, s, FALSE)) {
            P("  sector +%ld: read failed\n", (LONG)at[i]);
            continue;
        }
        P("  sector +%ld: id %08lx", (LONG)at[i], s[0]);
        if ((s[0] >> 8) == 0x504653 && s[1])
            P(" options %08lx disksize %lu reserved_blksize %lu", s[1], s[21], s[16] >> 16);
        P("\n");
    }
}

/* C:Mount has no SectorPerBlock keyword, so this sets it in a mounted entry
   whose handler has not started yet. */
static LONG set_spb(const char *name, ULONG spb)
{
    struct DosList *d = (struct DosList *)LockDosList(LDF_DEVICES | LDF_READ);
    LONG rc = RETURN_ERROR;

    d = (struct DosList *)FindDosEntry(d, (STRPTR)name, LDF_DEVICES);
    if (d && d->dol_misc.dol_handler.dol_Startup) {
        struct FileSysStartupMsg *m = BADDR(d->dol_misc.dol_handler.dol_Startup);
        ULONG *env = BADDR(m->fssm_Environ);
        if (d->dol_Task)
            P("%s: handler already running, block size not set\n", (ULONG)name);
        else if (env[DE_TABLESIZE] >= DE_SECSPERBLK) {
            env[DE_SECSPERBLK] = spb;
            rc = RETURN_OK;
        }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    P("%s: %lu sectors per block, %s\n", (ULONG)name, spb, (ULONG)(rc ? "FAILED" : "set"));
    /* Mount = 0 leaves the handler unstarted, and later tools need it running. */
    if (!rc) {
        static char dev[40];
        char *e = dev;
        while (*name && e < dev + 38) *e++ = *name++;
        *e++ = ':';
        *e = 0;
        DeviceProc((STRPTR)dev);
    }
    return rc;
}

/* Finds the RDB and writes the files that run the rest of the tests. */
static LONG scan(void)
{
    ULONG *b = (ULONG *)AllocMem(512, MEMF_PUBLIC);
    LONG unit, found = -1;
    ULONG block = 0xFFFFFFFF, i;
    BPTR ml, ts, wt, ma, sy;
    int parts = 0;

    if (!b) return RETURN_FAIL;
    for (unit = 0; unit < 7 && found < 0; unit++) {
        if (OpenDevice((STRPTR)DEV_NAME, unit, (struct IORequest *)&io, 0)) {
            P("Unit %ld: no device, error %ld\n", unit, (LONG)io.io_Error);
            continue;
        }
        if (read_capacity()) {
            P("Unit %ld: disk of %lu MB\n", unit, capacity >> 11);
            for (i = 0; i < 16; i++)
                if (read_block(i, b) && b[0] == IDNAME_RIGIDDISK && checksum_ok(b)) {
                    found = unit;
                    block = ((struct RigidDiskBlock *)b)->rdb_PartitionList;
                    break;
                }
            if (found < 0) P("Unit %ld: no RDB\n", unit);
        }
        if (found < 0) CloseDevice((struct IORequest *)&io);
    }
    if (found < 0) {
        P("No disk with an RDB was found\n");
        FreeMem(b, 512);
        return RETURN_ERROR;
    }

    ml = Open((STRPTR)"RAM:MountList", MODE_NEWFILE);
    ts = Open((STRPTR)"RAM:Tests", MODE_NEWFILE);
    wt = Open((STRPTR)"RAM:WriteTest", MODE_NEWFILE);
    ma = Open((STRPTR)"RAM:MountAll", MODE_NEWFILE);
    sy = Open((STRPTR)"RAM:SyncTest", MODE_NEWFILE);
    if (ml && ts && wt && ma && sy) {
        /* A hang here must not leave the floppy half written, so each step logs
           to RAM: and the floppy is written, and given time to settle, between
           steps. */
        static const UBYTE rates[] = { 5, 6, 8, 10 };
        #define SY_LOG "Type RAM:step >>SYS:mtectest.log\nWait 5\n"
        UWORD r;
        F(sy, "Echo \"Async reference\"\nEcho \"Starting async reference\" >>SYS:mtectest.log\nWait 5\n");
        F(sy, "MTecTest CRC UNIT %ld >RAM:step\n" SY_LOG, found);
        F(sy, "devtest -b " DEV_NAME " %ld >RAM:step\n" SY_LOG, found);
        for (r = 0; r < sizeof(rates); r++) {
            F(sy, "Echo \"Sync rate %ld\"\nEcho \"Starting sync rate %ld\" >>SYS:mtectest.log\nWait 5\n", (LONG)rates[r], (LONG)rates[r]);
            F(sy, "MTecSync RATE %ld >RAM:step\n" SY_LOG, (LONG)rates[r]);
            F(sy, "MTecTest CRC UNIT %ld >RAM:step\n" SY_LOG, found);
            F(sy, "devtest -b " DEV_NAME " %ld >RAM:step\n" SY_LOG, found);
        }
        F(sy, "Echo \"Back to async\"\nMTecSync OFF >RAM:step\n" SY_LOG);
        F(sy, "MTecTest CRC UNIT %ld >RAM:step\n" SY_LOG, found);
        F(ts, "Echo \"Comparing read paths\"\nMTecTest READ UNIT %ld " LOG "\n", found);
        F(wt, "Echo \"Raw write test\"\nMTecTest WRITE UNIT %ld " LOG "\nMTecPatch STATUS " LOG "\n", found);
        F(wt, "Echo \"Geometry\"\ndevtest -g " DEV_NAME " %ld " LOG "\n", found);
        F(wt, "Echo \"Probing the bus\"\ndevtest -p " DEV_NAME " " LOG "\n");

        while (block != 0xFFFFFFFF && parts < 16 && read_block(block, b) &&
               b[0] == IDNAME_PARTITION && checksum_ok(b)) {
            struct PartitionBlock *p = (struct PartitionBlock *)b;
            ULONG *e = p->pb_Environment;
            ULONG dt = e[DE_DOSTYPE], low = e[DE_LOWCYL], high = e[DE_UPPERCYL];
            ULONG spb = e[DE_TABLESIZE] >= DE_SECSPERBLK && e[DE_SECSPERBLK] ? e[DE_SECSPERBLK] : 1;
            ULONG cyl = e[DE_NUMHEADS] * e[DE_BLKSPERTRACK];
            ULONG first_mb = low * (cyl >> 3) >> 8, last_mb = ((high + 1) * (cyl >> 3)) >> 8;
            static char name[32], dosname[34];
            UBYTE len = p->pb_DriveName[0];
            BOOL pfs = (dt & 0xFFFFFF00) == 0x50465300 || (dt & 0xFFFFFF00) == 0x50445300;
            BOOL ffs = (dt & 0xFFFFFF00) == 0x444F5300;
            BOOL over4 = last_mb > 4096;
            BOOL nomount = (p->pb_Flags & PBFF_NOMOUNT) != 0;
            BOOL early, blocked;

            if (len > 30) len = 30;
            memcpy(name, p->pb_DriveName + 1, len);
            name[len] = 0;
            memcpy(dosname, name, len);
            dosname[len] = ':';
            dosname[len + 1] = 0;
            early = mounted(name);
            parts++;

            P("Partition %s: %lu-%lu MB, dostype %08lx, cyl %lu-%lu, %s\n", (ULONG)name, first_mb, last_mb,
              dt, low, high, (ULONG)(nomount ? "automount off" : "automount on"));
            P("  %lu heads, %lu sectors, %lu sectors per block, block %lu bytes, %lu sectors total\n",
              e[DE_NUMHEADS], e[DE_BLKSPERTRACK], spb, e[DE_SIZEBLOCK] << 2, (high - low + 1) * cyl);
            blocked = early && ffs && last_mb > 2048;
            if (early)
                P("  WARNING: already mounted, so it started before MTecPatch was loaded\n");
            if (blocked)
                P("  WARNING: skipped. FFS cannot use this partition: turn automount off in the RDB\n");
            if (!pfs && !ffs)
                P("  WARNING: unknown filesystem, no filesystem test\n");

            if ((pfs || ffs) && !early) {
                F(ml, "%s:\n Device = " DEV_NAME "\n Unit = %ld\n Flags = 0\n", (ULONG)name, found);
                F(ml, " Surfaces = %lu\n BlocksPerTrack = %lu\n", e[DE_NUMHEADS], e[DE_BLKSPERTRACK]);
                F(ml, " Reserved = %lu\n PreAlloc = %lu\n Interleave = %lu\n", e[DE_RESERVEDBLKS], e[DE_PREFAC], e[DE_INTERLEAVE]);
                F(ml, " LowCyl = %lu\n HighCyl = %lu\n Buffers = %lu\n", low, high, e[DE_NUMBUFFERS]);
                F(ml, " BufMemType = %lu\n MaxTransfer = 0x%lx\n Mask = 0x%lx\n", e[DE_BUFMEMTYPE], e[DE_MAXTRANSFER], e[DE_MASK]);
                F(ml, " DosType = 0x%lx\n StackSize = 8192\n GlobVec = -1\n Mount = %ld\n", dt, (LONG)(spb > 1 ? 0 : 1));
                if (pfs) F(ml, " FileSystem = L:pfs3aio\n");
                else if (over4) F(ml, " FileSystem = L:FFS46\n");
                F(ml, "#\n");
                F(ts, "Echo \"Mounting %s\"\nMount %s FROM RAM:MountList " LOG "\n", (ULONG)dosname, (ULONG)dosname);
                F(ma, "Mount %s FROM RAM:MountList >NIL:\n", (ULONG)dosname);
                if (spb > 1) {
                    F(ts, "MTecTest SPB %lu VOL %s " LOG "\n", spb, (ULONG)name);
                    F(ma, "MTecTest SPB %lu VOL %s >NIL:\n", spb, (ULONG)name);
                }
            }
            if ((pfs || ffs) && !blocked) {
                F(ts, "Wait 2\nEcho \"File test on %s\"\n", (ULONG)dosname);
                F(ts, "MTecTest FILE VOL %s " LOG "\n", (ULONG)dosname);
            }
            block = p->pb_Next;
            probe_partition(low * cyl);
        }
        P("Found %ld partitions\n", (LONG)parts);
                F(ts, "Info " LOG "\nMTecPatch STATUS " LOG "\n");
        F(ts, "Echo \"Read speed\"\ndevtest -b " DEV_NAME " %ld " LOG "\n", found);
        F(ts, "Echo \"Packet types, read-only\"\ndevtest -t " DEV_NAME " %ld " LOG "\n", found);
        F(ts, "Echo \"Added commands\"\nCmdTest UNIT %ld " LOG "\n", found);
    } else {
        P("Cannot create the files in RAM:\n");
    }
    if (ml) Close(ml);
    if (ts) Close(ts);
    if (wt) Close(wt);
    if (ma) Close(ma);
    if (sy) Close(sy);
    CloseDevice((struct IORequest *)&io);
    FreeMem(b, 512);
    return RETURN_OK;
}

/* Replaces C:Ask from the install disk, a BCPL command that repeated its
   prompt on the real machine. Waits for one line: RC 5 for y, otherwise 0. */
static LONG ask(void)
{
    static const char q[] = "Run the raw write test? (y/N) ";
    BPTR con = Open((STRPTR)"*", MODE_OLDFILE);
    UBYTE line[80];
    LONG rc = RETURN_OK;

    if (!con) return rc;
    Write(con, (APTR)q, sizeof(q) - 1);
    if (Read(con, line, sizeof(line)) > 0 && (line[0] == 'y' || line[0] == 'Y'))
        rc = RETURN_WARN;
    Close(con);
    return rc;
}

static LONG run(void)
{
    LONG args[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rd = (struct RDArgs *)ReadArgs((STRPTR)"SCAN/S,READ/S,WRITE/S,FILE/S,UNIT/N,VOL/K,ASK/S,SPB/K/N,CRC/S", args, NULL);
    LONG rc = RETURN_ERROR;

    if (!rd) {
        PrintFault(IoErr(), (STRPTR)"MTecTest");
        return rc;
    }
    if (args[6]) rc = ask();
    else if (args[8] && args[4]) rc = crc_test(*(LONG *)args[4]);
    else if (args[7] && args[5]) rc = set_spb((const char *)args[5], *(ULONG *)args[7]);
    else if (args[0]) rc = scan();
    else if ((args[1] || args[2]) && args[4]) rc = unit_test(*(LONG *)args[4], args[2] != 0);
    else if (args[3] && args[5]) rc = file_test((const char *)args[5]);
    else P("Use SCAN, READ UNIT n, WRITE UNIT n, FILE VOL name or ASK\n");
    FreeArgs(rd);
    return rc;
}

LONG start(void)
{
    LONG rc = RETURN_FAIL;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
    if (DOSBase) {
        if (((struct Process *)FindTask(NULL))->pr_CLI) {
            init_io();
            rc = run();
        }
        CloseLibrary((struct Library *)DOSBase);
    }
    return rc;
}
