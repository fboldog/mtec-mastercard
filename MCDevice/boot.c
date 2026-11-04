/* boot.c - mounting at start-up, for the ROM build of mastercard.device.
 * Included into dev.c, so it uses its SCSI helpers; runs in the driver task.
 *
 * For each target 0-6 on LUN 0 that is a disk with 512-byte blocks: find the
 * Rigid Disk Block in blocks 0-15, load the file systems stored in it that
 * FileSystem.resource doesn't already have (in this version or newer), and
 * add a boot node for each partition that isn't marked "no mount". Bootable
 * partitions get their boot priority; the others -128, so they mount but
 * don't boot. dos.library isn't running yet, so file systems are loaded by
 * the small hunk loader here, not LoadSeg.
 */
#include <devices/hardblocks.h>
#include <resources/filesysres.h>
#include <libraries/expansion.h>
#include <dos/filehandler.h>

#define RDB_LIMIT 16
#define LSEG_DATA 123

static const char fsr_name[] = "FileSystem.resource";

static int read_block(struct MC *mc, struct MCUnit *u, ULONG blk, ULONG *buf)
{
    UBYTE cdb[10] = { 0x28, 0, blk >> 24, blk >> 16, blk >> 8, blk, 0, 0, 1, 0 };
    ULONG actual = 0;
    return !scsi(mc, u, cdb, 10, buf, 512, 1, &actual, 0) && actual == 512;
}

/* A block of the RDB: right ID, sane length, sum of all longs zero. */
static int good_block(ULONG *b, ULONG id)
{
    ULONG n = b[1], sum = 0, k;
    if (b[0] != id || n < 3 || n > 128)
        return 0;
    for (k = 0; k < n; k++)
        sum += b[k];
    return sum == 0;
}

/* ---- the hunk loader, reading a LoadSegBlock chain ---- */

struct Stream {
    struct MC *mc;
    struct MCUnit *u;
    ULONG next;             /* next LSEG block, or -1 */
    ULONG *buf;
    UWORD pos, n;           /* longs used and available in buf */
    UWORD half;             /* RELOC32SHORT: a word is waiting */
    UWORD word;
    int err;
};

static ULONG sget(struct Stream *s)
{
    if (s->err)
        return 0;
    if (s->pos >= s->n) {
        if (s->next == 0xFFFFFFFFUL || !read_block(s->mc, s->u, s->next, s->buf) ||
            !good_block(s->buf, IDNAME_LOADSEG)) {
            s->err = 1;
            return 0;
        }
        s->n = s->buf[1] - 5;
        if (s->n > LSEG_DATA)
            s->n = LSEG_DATA;
        s->pos = 0;
        s->next = s->buf[4];
    }
    return s->buf[5 + s->pos++];
}

static UWORD sgetw(struct Stream *s)
{
    ULONG l;
    if (s->half) {
        s->half = 0;
        return s->word;
    }
    l = sget(s);
    s->word = l;
    s->half = 1;
    return l >> 16;
}

static void free_seglist(BPTR seg)
{
    while (seg) {
        ULONG *m = (ULONG *)BADDR(seg) - 1;
        seg = m[1];
        FreeMem(m, m[0]);
    }
}

#define MAX_HUNKS 32

/* Loads an executable from the stream; returns its segment list, or 0. */
static BPTR load_hunks(struct Stream *s)
{
    ULONG *hunk[MAX_HUNKS];
    ULONG first, last, nh, h, k, t;
    BPTR seg = 0, *link = &seg;

    if (sget(s) != 0x3F3)                   /* HUNK_HEADER */
        return 0;
    while ((k = sget(s)) && !s->err)        /* resident library names */
        while (k--)
            sget(s);
    sget(s);                                /* table size */
    first = sget(s);
    last = sget(s);
    nh = last - first + 1;
    if (s->err || last < first || nh > MAX_HUNKS)
        return 0;
    for (h = 0; h < nh; h++) {
        ULONG size = sget(s), flags = MEMF_PUBLIC | MEMF_CLEAR, *m;
        if ((size & 0xC0000000UL) == 0xC0000000UL)
            flags = sget(s) | MEMF_CLEAR;
        else if (size & 0x40000000UL)
            flags |= MEMF_CHIP;
        size = (size & 0x3FFFFFFFUL) * 4 + 8;
        m = (ULONG *)AllocMem(size, flags);
        if (!m || s->err) {
            if (m)
                FreeMem(m, size);
            free_seglist(seg);
            return 0;
        }
        m[0] = size;
        *link = MKBADDR(&m[1]);
        link = (BPTR *)&m[1];
        hunk[h] = &m[2];
    }
    h = 0;
    while (h < nh && !s->err) {
        ULONG type = sget(s) & 0x3FFFFFFFUL, n;
        switch (type) {
        case 0x3E9:                         /* HUNK_CODE */
        case 0x3EA:                         /* HUNK_DATA */
            n = sget(s);
            for (k = 0; k < n; k++)
                hunk[h][k] = sget(s);
            break;
        case 0x3EB:                         /* HUNK_BSS: already cleared */
            sget(s);
            break;
        case 0x3EC:                         /* HUNK_RELOC32 */
            while ((n = sget(s)) && !s->err) {
                t = sget(s);
                if (t >= nh) {
                    s->err = 1;
                    break;
                }
                while (n--) {
                    ULONG off = sget(s);
                    *(ULONG *)((UBYTE *)hunk[h] + off) += (ULONG)hunk[t];
                }
            }
            break;
        case 0x3FC:                         /* HUNK_RELOC32SHORT */
        case 0x3F7:                         /* HUNK_DREL32, the same in executables */
            s->half = 0;
            while ((n = sgetw(s)) && !s->err) {
                t = sgetw(s);
                if (t >= nh) {
                    s->err = 1;
                    break;
                }
                while (n--) {
                    ULONG off = sgetw(s);
                    *(ULONG *)((UBYTE *)hunk[h] + off) += (ULONG)hunk[t];
                }
            }
            s->half = 0;                    /* padded to a long */
            break;
        case 0x3F0:                         /* HUNK_SYMBOL */
            while ((n = sget(s)) && !s->err)
                for (k = 0; k <= n; k++)
                    sget(s);
            break;
        case 0x3F1:                         /* HUNK_DEBUG */
            n = sget(s);
            while (n--)
                sget(s);
            break;
        case 0x3F2:                         /* HUNK_END */
            h++;
            break;
        default:
            s->err = 1;
        }
    }
    if (s->err) {
        free_seglist(seg);
        return 0;
    }
    CacheClearU();
    return seg;
}

/* ---- file systems ---- */

static struct FileSysEntry *find_fs(struct FileSysResource *fsr, ULONG dostype)
{
    struct FileSysEntry *f, *best = 0;
    for (f = (struct FileSysEntry *)fsr->fsr_FileSysEntries.lh_Head; f->fse_Node.ln_Succ;
         f = (struct FileSysEntry *)f->fse_Node.ln_Succ)
        if (f->fse_DosType == dostype && (!best || f->fse_Version > best->fse_Version))
            best = f;
    return best;
}

/* Loads the file systems of the RDB that the resource doesn't have yet. */
static void load_filesystems(struct MC *mc, struct MCUnit *u, struct FileSysResource *fsr, ULONG blk,
                             ULONG *buf, ULONG *lbuf)
{
    int guard = 0;
    while (blk != 0xFFFFFFFFUL && guard++ < 32) {
        struct FileSysHeaderBlock *fh = (struct FileSysHeaderBlock *)buf;
        struct FileSysEntry *old, *fse;
        struct Stream s;
        ULONG next, k;
        if (!read_block(mc, u, blk, buf) || !good_block(buf, IDNAME_FILESYSHEADER))
            return;
        next = fh->fhb_Next;
        old = find_fs(fsr, fh->fhb_DosType);
        if (old && old->fse_Version >= fh->fhb_Version) {
            blk = next;
            continue;
        }
        fse = (struct FileSysEntry *)AllocMem(sizeof *fse, MEMF_PUBLIC | MEMF_CLEAR);
        if (!fse)
            return;
        fse->fse_Node.ln_Name = (char *)dev_name;
        fse->fse_DosType = fh->fhb_DosType;
        fse->fse_Version = fh->fhb_Version;
        fse->fse_PatchFlags = fh->fhb_PatchFlags;
        for (k = 0; k < 9; k++)             /* fhb_Type .. fhb_GlobalVec */
            (&fse->fse_Type)[k] = (&fh->fhb_Type)[k];
        memset(&s, 0, sizeof s);
        s.mc = mc;
        s.u = u;
        s.next = fh->fhb_SegListBlocks;
        s.buf = lbuf;
        fse->fse_SegList = load_hunks(&s);
        if (!fse->fse_SegList) {
            FreeMem(fse, sizeof *fse);
        } else {
            fse->fse_PatchFlags |= 0x80;    /* the SegList is always ours */
            Forbid();
            AddHead(&fsr->fsr_FileSysEntries, &fse->fse_Node);
            Permit();
        }
        blk = next;
    }
}

/* ---- partitions ---- */

static void add_partitions(struct MC *mc, struct MCUnit *u, struct FileSysResource *fsr, ULONG blk,
                           ULONG *buf, struct ConfigDev *cd)
{
    int guard = 0;
    while (blk != 0xFFFFFFFFUL && guard++ < 64) {
        struct PartitionBlock *pb = (struct PartitionBlock *)buf;
        struct DosEnvec *de = (struct DosEnvec *)pb->pb_Environment;
        ULONG pp[4 + 20], k, n;
        char name[32];
        struct DeviceNode *dn;
        LONG pri;

        if (!read_block(mc, u, blk, buf) || !good_block(buf, IDNAME_PARTITION))
            return;
        blk = pb->pb_Next;
        if (pb->pb_Flags & PBFF_NOMOUNT)
            continue;
        n = pb->pb_DriveName[0];
        if (n > 30)
            n = 30;
        for (k = 0; k < n; k++)
            name[k] = pb->pb_DriveName[1 + k];
        name[n] = 0;
        pp[0] = (ULONG)name;
        pp[1] = (ULONG)dev_name;
        pp[2] = u->target + 10 * u->lun;
        pp[3] = pb->pb_DevFlags;
        n = de->de_TableSize + 1;
        if (n > 20)
            n = 20;
        for (k = 0; k < n; k++)
            pp[4 + k] = pb->pb_Environment[k];
        dn = (struct DeviceNode *)MakeDosNode(pp);
        if (!dn)
            continue;
        if (fsr) {
            /* Fill in the file system: each set bit of fse_PatchFlags
               names a field to copy, in DeviceNode order from dn_Type. */
            struct FileSysEntry *f = find_fs(fsr, de->de_DosType);
            if (f)
                for (k = 0; k < 9; k++)
                    if (f->fse_PatchFlags & (1UL << k))
                        (&dn->dn_Type)[k] = (&f->fse_Type)[k];
        }
        pri = (pb->pb_Flags & PBFF_BOOTABLE) && n > DE_BOOTPRI ? (LONG)pb->pb_Environment[DE_BOOTPRI] : -128;
        AddBootNode(pri, 0, dn, cd);
    }
}

/* MC_MOUNT, in the task: every disk on LUN 0. */
static void mount_all(struct MC *mc, struct IOStdReq *ior)
{
    struct FileSysResource *fsr = (struct FileSysResource *)OpenResource((STRPTR)fsr_name);
    struct ConfigDev *cd = (struct ConfigDev *)ior->io_Data;
    ULONG *buf = (ULONG *)AllocMem(1024, MEMF_PUBLIC);
    UWORD t;

    if (!buf)
        return;
    if (!(ExpansionBase = (struct Library *)OpenLibrary((STRPTR)exp_name, 37))) {
        FreeMem(buf, 1024);
        return;
    }
    for (t = 0; t < MAX_TARGET; t++) {
        struct IOStdReq pr;
        struct MCUnit *u;
        ULONG blk;
        if (t == OWN_ID)
            continue;
        if (!mc->units[t]) {
            memset(&pr, 0, sizeof pr);
            pr.io_Offset = t;
            probe(mc, &pr);
        }
        u = mc->units[t];
        if (!u || u->type != 0 || u->shift != 9)
            continue;                       /* disks with 512-byte blocks only */
        for (blk = 0; blk < RDB_LIMIT; blk++)
            if (read_block(mc, u, blk, buf) && good_block(buf, IDNAME_RIGIDDISK))
                break;
        if (blk == RDB_LIMIT)
            continue;
        {
            struct RigidDiskBlock *rdb = (struct RigidDiskBlock *)buf;
            ULONG parts = rdb->rdb_PartitionList, fs = rdb->rdb_FileSysHeaderList;
            if (fsr)
                load_filesystems(mc, u, fsr, fs, buf, buf + 128);
            add_partitions(mc, u, fsr, parts, buf, cd);
        }
    }
    CloseLibrary(ExpansionBase);
    FreeMem(buf, 1024);
}
