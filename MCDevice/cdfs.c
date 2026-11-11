/* cdfs.c - a small read-only ISO 9660 file system, for booting from CD with
 * the ROM build of mastercard.device. Included into boot.c.
 *
 * The ROM has no room for a full CD file system, so a bootable CD with no
 * CD01 file system registered gets this one: plain ISO 9660 names (";1" and
 * a trailing "." dropped, compared without case), directories, files, and
 * the packets DOS, the Shell and Workbench use to boot and read from it.
 * Discs can be changed; a lock or file of a disc taken out waits for it to
 * come back. There is no Rock Ridge, Joliet or audio, and nothing can be
 * written. Based on ODFileSystem (https://github.com/reinauer/ODFileSystem,
 * Copyright (c) 2026 Stefan Reinauer, BSD-2-Clause), cut down to plain
 * ISO 9660 for booting; its ISO record handling follows backends/iso9660.
 *
 * It runs as the handler process DOS starts for CD0, from a seglist that
 * jumps into the driver, and reads through mastercard.device with DoIO:
 * whole sectors go straight into the caller's buffer, the rest through one
 * cached sector.
 */
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <devices/input.h>
#include <devices/inputevent.h>

struct DosLibrary *DOSBase;
#include "gen/cdfsstubs.h"

#define SECTOR 2048

/* A file or directory: its extent, and its parent directory's. The record
   that names it is at rsec:roff; rsec 0 if not known yet. */
struct Obj {
    ULONG lba, size;
    ULONG plba, psize;
    ULONG rsec;
    UWORD roff;
    UWORD dir;
    struct DosList *vol;        /* the disc it is on */
};

struct CLock {
    struct FileLock fl;
    struct Obj o;
};

struct CFile {
    struct Obj o;
    ULONG pos;
};

struct CD {
    struct MsgPort *port;       /* the handler's packet port */
    struct MsgPort ioport;
    struct IOStdReq io;
    UBYTE *buf;                 /* one sector */
    ULONG bufsec;               /* which, or ~0 */
    struct Obj root;
    struct DosList *vol;        /* the disc in the drive, or 0 */
    ULONG blocks;
    UWORD locks;                /* locks and files on it */
    UBYTE add, tried;           /* vol still to go on the DOS list; disc read */
    struct DosList *old;        /* the last disc, kept while it has locks */
    struct DosList *rem;        /* to take off the DOS list */
    UWORD oldlocks;
    ULONG changes;              /* TD_CHANGENUM when last looked */
    struct MsgPort tport;
    struct timerequest treq;    /* a look at the drive every second */
    struct IOStdReq inp;        /* input.device, to tell Workbench */
};

static LONG cd_read(struct CD *c, ULONG sec, void *dst, ULONG len)
{
    c->io.io_Command = CMD_READ;
    c->io.io_Offset = sec * SECTOR;
    c->io.io_Length = len;
    c->io.io_Data = dst;
    return DoIO((struct IORequest *)&c->io) || c->io.io_Actual != len;
}

static UBYTE *sector(struct CD *c, ULONG sec)
{
    if (c->bufsec != sec) {
        c->bufsec = ~0UL;
        if (cd_read(c, sec, c->buf, SECTOR))
            return 0;
        c->bufsec = sec;
    }
    return c->buf;
}

/* ISO 9660 stores numbers both ways; these read the big-endian copies. */
#define BE32(p) (*(ULONG *)(p))

/* The data starts after the extended attribute record, if there is one. */
static void set_obj(struct Obj *o, const UBYTE *r)
{
    o->lba = BE32(r + 6) + r[1];
    o->size = BE32(r + 14);
    o->dir = r[25] & 2;
}

/* The next record of directory o from byte *pos, without "." and "..";
   0 at the end. The record stays in the sector buffer. */
static UBYTE *dir_next(struct CD *c, struct Obj *o, ULONG *pos, struct Obj *found)
{
    while (*pos < o->size) {
        ULONG sec = o->lba + (*pos >> 11);
        UWORD off = *pos & (SECTOR - 1);
        UBYTE *r = sector(c, sec);
        if (!r)
            return 0;
        r += off;
        if (!r[0] || off + r[0] > SECTOR) {
            *pos = (*pos | (SECTOR - 1)) + 1;   /* records don't cross sectors */
            continue;
        }
        *pos += r[0];
        if ((r[32] == 1 && r[33] <= 1) || (r[25] & 4))
            continue;                           /* ".", "..", associated */
        if (found) {
            set_obj(found, r);
            found->vol = o->vol;
            found->plba = o->lba;
            found->psize = o->size;
            found->rsec = sec;
            found->roff = off;
        }
        return r;
    }
    return 0;
}

/* The name of record r without ";1" and a trailing ".", at most 106
   characters, as a FileInfoBlock holds. */
static UWORD rec_name(const UBYTE *r, char *d)
{
    UWORD n = 0, k;
    for (k = 0; k < r[32] && r[33 + k] != ';' && n < 106; k++)
        d[n++] = r[33 + k];
    if (n && d[n - 1] == '.')
        n--;
    return n;
}

static UBYTE upper(UBYTE ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) ? ch - 32 : ch;
}

/* The directory holding o, a file or directory; its own parent is in its
   ".." record. */
static int up(struct CD *c, struct Obj *o)
{
    UBYTE *r;
    if (o->lba == c->root.lba)
        return 0;
    o->lba = o->plba;
    o->size = o->psize;
    o->dir = 2;
    o->rsec = 0;
    o->plba = o->psize = 0;
    if (o->lba != c->root.lba) {
        if (!(r = sector(c, o->lba)))
            return 0;
        r += r[0];                              /* "." then ".." */
        o->plba = BE32(r + 6);
        o->psize = BE32(r + 14);
    }
    return 1;
}

/* Follows a path from o: "a/b", "/" for the parent. A "vol:" or "assign:"
   in front is skipped: DOS passes no lock for a volume's root and the
   assign's lock for an assign, as ODFileSystem relies on too. */
static LONG locate(struct CD *c, struct Obj *o, const UBYTE *bname)
{
    UWORD len = bname[0], i = 1, k;
    const UBYTE *s = bname + 1;
    for (k = 0; k < len; k++)
        if (s[k] == ':')
            i = k + 2;                          /* DOS gives the lock to start from */
    while (i <= len) {
        UWORD start = i, n;
        while (i <= len && bname[i] != '/')
            i++;
        n = i - start;
        i++;
        if (!n) {
            if (!up(c, o))
                return ERROR_OBJECT_NOT_FOUND;
            continue;
        }
        if (!o->dir)
            return ERROR_OBJECT_NOT_FOUND;
        {
            struct Obj d = *o;
            ULONG pos = 0;
            UBYTE *r;
            char nm[108];
            for (;;) {
                if (!(r = dir_next(c, &d, &pos, o)))
                    return ERROR_OBJECT_NOT_FOUND;
                if (rec_name(r, nm) == n) {
                    for (k = 0; k < n && upper(nm[k]) == upper(bname[start + k]); k++)
                        ;
                    if (k == n)
                        break;
                }
            }
        }
    }
    return 0;
}

/* Days since 1978 for an ISO date (years since 1900, month, day...). */
static void iso_date(const UBYTE *d, struct DateStamp *ds)
{
    static const UWORD cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    LONG y = d[0] + 1900;
    ds->ds_Days = ds->ds_Minute = ds->ds_Tick = 0;
    if (y < 1978 || d[1] < 1 || d[1] > 12)
        return;
    ds->ds_Days = 365 * (y - 1978) + (y - 1977) / 4 + cum[d[1] - 1] + d[2] - 1 +
                  (!(y & 3) && d[1] > 2);
    ds->ds_Minute = d[3] * 60 + d[4];
    ds->ds_Tick = d[5] * TICKS_PER_SECOND;
}

static void fill_fib(struct CD *c, struct FileInfoBlock *fib, const UBYTE *r)
{
    UBYTE n;
    LONG type = r[25] & 2 ? ST_USERDIR : ST_FILE;
    memset(&fib->fib_DirEntryType, 0, sizeof *fib - 4);
    fib->fib_DirEntryType = fib->fib_EntryType = type;
    n = rec_name(r, (char *)fib->fib_FileName + 1);
    fib->fib_FileName[0] = n;
    fib->fib_Size = BE32(r + 14);
    fib->fib_NumBlocks = (fib->fib_Size + SECTOR - 1) / SECTOR;
    fib->fib_Protection = FIBF_ARCHIVE | FIBF_WRITE | FIBF_DELETE;     /* "---ar-e-" */
    iso_date(r + 18, &fib->fib_Date);
    (void)c;
}

/* Examine: the root is the volume, anything else its record, looked up in
   the parent directory if not known. */
static LONG examine(struct CD *c, struct Obj *o, struct FileInfoBlock *fib)
{
    UBYTE *r;
    if (o->lba == c->root.lba) {
        UBYTE *v = (UBYTE *)BADDR(c->vol->dol_Name);
        memset(&fib->fib_DirEntryType, 0, sizeof *fib - 4);
        fib->fib_DirEntryType = fib->fib_EntryType = ST_ROOT;
        memcpy(fib->fib_FileName, v, v[0] + 1);
        fib->fib_Date = c->vol->dol_misc.dol_volume.dol_VolumeDate;
    } else {
        if (!o->rsec) {
            struct Obj p = { o->plba, o->psize, 0, 0, 0, 0, 2, 0 }, f;
            ULONG pos = 0;
            while ((r = dir_next(c, &p, &pos, &f)) && f.lba != o->lba)
                ;
            if (!r)
                return ERROR_OBJECT_NOT_FOUND;
            o->rsec = f.rsec;
            o->roff = f.roff;
        }
        if (!(r = sector(c, o->rsec)))
            return ERROR_NOT_A_DOS_DISK;
        fill_fib(c, fib, r + o->roff);
    }
    fib->fib_DiskKey = 0;                       /* ExNext starts at the top */
    return 0;
}

static BPTR new_lock(struct CD *c, struct Obj *o, LONG access)
{
    struct CLock *l = (struct CLock *)AllocMem(sizeof *l, MEMF_PUBLIC | MEMF_CLEAR);
    if (!l)
        return 0;
    l->fl.fl_Key = o->lba;
    l->fl.fl_Access = access;
    l->fl.fl_Task = c->port;
    l->fl.fl_Volume = MKBADDR(c->vol);
    l->o = *o;
    c->locks++;
    return MKBADDR(l);
}

/* The object a lock names; the root for none. */
static struct Obj *lock_obj(struct CD *c, BPTR lock)
{
    return lock ? &((struct CLock *)BADDR(lock))->o : &c->root;
}

static void reply(struct CD *c, struct DosPacket *dp, LONG r1, LONG r2)
{
    struct MsgPort *to = dp->dp_Port;
    struct Message *m = dp->dp_Link;
    dp->dp_Res1 = r1;
    dp->dp_Res2 = r2;
    dp->dp_Port = c->port;
    m->mn_Node.ln_Name = (char *)dp;
    PutMsg(to, m);
}

static LONG file_read(struct CD *c, struct CFile *f, UBYTE *d, ULONG len, LONG *err)
{
    ULONG done = 0;
    if (len > f->o.size - f->pos)
        len = f->o.size - f->pos;
    while (done < len) {
        ULONG off = f->pos & (SECTOR - 1), sec = f->o.lba + (f->pos >> 11), n = len - done;
        if (!off && n >= SECTOR) {
            n &= ~(SECTOR - 1);
            if (cd_read(c, sec, d + done, n))
                break;
        } else {
            UBYTE *b = sector(c, sec);
            if (!b)
                break;
            if (n > SECTOR - off)
                n = SECTOR - off;
            memcpy(d + done, b + off, n);
        }
        done += n;
        f->pos += n;
    }
    if (done < len) {
        *err = ERROR_NOT_A_DOS_DISK;
        return -1;
    }
    return done;
}

/* Whether o's disc is in the drive. */
static LONG check(struct CD *c, struct Obj *o)
{
    return !c->vol ? ERROR_NO_DISK : o->vol != c->vol ? ERROR_DEVICE_NOT_MOUNTED : 0;
}

/* A lock or file is gone: an old disc's volume goes too with its last one. */
static void release(struct CD *c, struct Obj *o)
{
    if (o->vol == c->vol)
        c->locks--;
    else if (o->vol == c->old && !--c->oldlocks) {
        c->rem = c->old;
        c->old = 0;
    }
}

/* One packet; returns its first result, *err its second. */
static LONG packet(struct CD *c, struct DosPacket *dp, LONG *err)
{
    LONG a1 = dp->dp_Arg1, a2 = dp->dp_Arg2, a3 = dp->dp_Arg3;
    struct Obj o;
    switch (dp->dp_Type) {
    case ACTION_IS_FILESYSTEM:
    case ACTION_FLUSH:
    case ACTION_INHIBIT:
        return DOSTRUE;
    case ACTION_CURRENT_VOLUME:
        return MKBADDR(c->vol);
    case ACTION_LOCATE_OBJECT:
        o = *lock_obj(c, a1);
        if ((*err = check(c, &o)) || (*err = locate(c, &o, (UBYTE *)BADDR(a2))))
            return 0;
        return new_lock(c, &o, a3);
    case ACTION_COPY_DIR:
        if (!a1)
            return 0;
        /* fall through */
    case ACTION_COPY_DIR_FH:
        o = dp->dp_Type == ACTION_COPY_DIR ? *lock_obj(c, a1) : ((struct CFile *)a1)->o;
        if ((*err = check(c, &o)))
            return 0;
        return new_lock(c, &o, SHARED_LOCK);
    case ACTION_FREE_LOCK:
        if (a1) {
            release(c, lock_obj(c, a1));
            FreeMem(BADDR(a1), sizeof(struct CLock));
        }
        return DOSTRUE;
    case ACTION_PARENT:
    case ACTION_PARENT_FH:
        o = dp->dp_Type == ACTION_PARENT ? *lock_obj(c, a1) : ((struct CFile *)a1)->o;
        if ((*err = check(c, &o)) || !up(c, &o))
            return 0;                           /* the root has none */
        return new_lock(c, &o, SHARED_LOCK);
    case ACTION_SAME_LOCK:
        return lock_obj(c, a1)->lba == lock_obj(c, a2)->lba ? DOSTRUE : DOSFALSE;
    case ACTION_EXAMINE_OBJECT:
    case ACTION_EXAMINE_FH:
        o = dp->dp_Type == ACTION_EXAMINE_OBJECT ? *lock_obj(c, a1) : ((struct CFile *)a1)->o;
        if ((*err = check(c, &o)) || (*err = examine(c, &o, (struct FileInfoBlock *)BADDR(a2))))
            return DOSFALSE;
        if (dp->dp_Type == ACTION_EXAMINE_OBJECT && a1)
            lock_obj(c, a1)->rsec = o.rsec, lock_obj(c, a1)->roff = o.roff;
        return DOSTRUE;
    case ACTION_EXAMINE_NEXT: {
        struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(a2);
        ULONG pos = fib->fib_DiskKey;
        UBYTE *r;
        o = *lock_obj(c, a1);
        if ((*err = check(c, &o)))
            return DOSFALSE;
        if (!o.dir || !(r = dir_next(c, &o, &pos, 0))) {
            *err = ERROR_NO_MORE_ENTRIES;
            return DOSFALSE;
        }
        fill_fib(c, fib, r);
        fib->fib_DiskKey = pos;
        return DOSTRUE;
    }
    case ACTION_FINDINPUT:
    case ACTION_FH_FROM_LOCK: {
        struct FileHandle *fh = (struct FileHandle *)BADDR(a1);
        struct CFile *f;
        o = *lock_obj(c, a2);
        if ((*err = check(c, &o)) ||
            (dp->dp_Type == ACTION_FINDINPUT && (*err = locate(c, &o, (UBYTE *)BADDR(a3)))))
            return DOSFALSE;
        if (o.dir) {
            *err = ERROR_OBJECT_WRONG_TYPE;
            return DOSFALSE;
        }
        if (!(f = (struct CFile *)AllocMem(sizeof *f, MEMF_PUBLIC | MEMF_CLEAR))) {
            *err = ERROR_NO_FREE_STORE;
            return DOSFALSE;
        }
        f->o = o;
        fh->fh_Arg1 = (LONG)f;
        if (dp->dp_Type == ACTION_FH_FROM_LOCK)
            FreeMem(BADDR(a2), sizeof(struct CLock));       /* the file takes its count */
        else
            c->locks++;
        return DOSTRUE;
    }
    case ACTION_READ:
        if ((*err = check(c, &((struct CFile *)a1)->o)))
            return -1;
        return file_read(c, (struct CFile *)a1, (UBYTE *)a2, a3, err);
    case ACTION_SEEK: {
        struct CFile *f = (struct CFile *)a1;
        LONG old = f->pos, to = a2 + (a3 == OFFSET_BEGINNING ? 0 : a3 == OFFSET_END ? (LONG)f->o.size : old);
        if ((*err = check(c, &f->o)))
            return -1;
        if (to < 0 || (ULONG)to > f->o.size) {
            *err = ERROR_SEEK_ERROR;
            return -1;
        }
        f->pos = to;
        return old;
    }
    case ACTION_END:
        release(c, &((struct CFile *)a1)->o);
        FreeMem((APTR)a1, sizeof(struct CFile));
        return DOSTRUE;
    case ACTION_INFO:
        a1 = a2;
        /* fall through */
    case ACTION_DISK_INFO: {
        struct InfoData *id = (struct InfoData *)BADDR(a1);
        memset(id, 0, sizeof *id);
        id->id_DiskState = ID_WRITE_PROTECTED;
        id->id_NumBlocks = id->id_NumBlocksUsed = c->blocks;
        id->id_BytesPerBlock = SECTOR;
        id->id_DiskType = c->vol ? ID_DOS_DISK : ID_NO_DISK_PRESENT;
        id->id_VolumeNode = MKBADDR(c->vol);
        id->id_InUse = c->locks;
        return DOSTRUE;
    }
    case ACTION_DELETE_OBJECT:
    case ACTION_RENAME_OBJECT:
    case ACTION_CREATE_DIR:
    case ACTION_SET_PROTECT:
    case ACTION_SET_COMMENT:
    case ACTION_SET_DATE:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE:
    case ACTION_WRITE:
    case ACTION_RENAME_DISK:
        *err = ERROR_DISK_WRITE_PROTECTED;
        return DOSFALSE;
    }
    *err = ERROR_ACTION_NOT_KNOWN;
    return DOSFALSE;
}

/* The primary volume descriptor: the first of type 1 in the set that starts
   at sector 16 (a boot record may come before it). */
static UBYTE *pvd(struct CD *c)
{
    ULONG s;
    UBYTE *p;
    for (s = 16; s < 32 && (p = sector(c, s)) && same(p + 1, "CD001", 5) && p[0] != 255; s++)
        if (p[0] == 1)
            return p;
    return 0;
}

/* Tells Workbench a disc came or went. */
static void event(struct CD *c, UBYTE cls)
{
    struct InputEvent ie;
    if (!c->inp.io_Device)
        return;
    memset(&ie, 0, sizeof ie);
    ie.ie_Class = cls;
    c->inp.io_Command = IND_WRITEEVENT;
    c->inp.io_Data = &ie;
    c->inp.io_Length = sizeof ie;
    DoIO((struct IORequest *)&c->inp);
}

/* The disc in the drive: its volume, or the last disc's again if it is the
   same one (same name and date), so its locks work again. */
static void mount(struct CD *c)
{
    struct DosList *v;
    struct DateStamp ds;
    UBYTE *p, *on;
    char name[33];
    int k, n = 0;
    c->bufsec = ~0UL;
    if (!(p = pvd(c)))
        return;
    set_obj(&c->root, p + 156);
    c->blocks = BE32(p + 84);
    for (k = 0; k < 32; k++)                    /* volume ID, without trailing spaces */
        if ((name[k] = p[40 + k]) != ' ' && name[k])
            n = k + 1;
    if (!n)
        name[n++] = 'C', name[n++] = 'D';       /* no volume ID */
    name[n] = 0;
    iso_date(p + 156 + 18, &ds);
    if ((v = c->old) && (on = (UBYTE *)BADDR(v->dol_Name))[0] == n && same(on + 1, name, n) &&
        same((UBYTE *)&v->dol_misc.dol_volume.dol_VolumeDate, (char *)&ds, sizeof ds)) {
        c->old = 0;
        c->locks = c->oldlocks;
    } else {
        if (!(v = (struct DosList *)MakeDosEntry((STRPTR)name, DLT_VOLUME)))
            return;
        v->dol_misc.dol_volume.dol_DiskType = ID_DOS_DISK;
        v->dol_misc.dol_volume.dol_VolumeDate = ds;
        c->add = 1;
        c->locks = 0;
    }
    v->dol_Task = c->port;
    c->vol = c->root.vol = v;
    event(c, IECLASS_DISKINSERTED);
}

/* The disc went: its volume stays, not mounted, while it has locks. */
static void unmount(struct CD *c)
{
    struct DosList *v = c->vol;
    v->dol_Task = 0;
    c->vol = c->root.vol = 0;
    if (c->locks) {
        c->old = v;                             /* an older one stays as it is */
        c->oldlocks = c->locks;
    } else
        c->rem = v;
    event(c, IECLASS_DISKREMOVED);
}

/* Volumes on and off the DOS list, when it is free: a handler must not
   wait for it, DOS may be waiting for us. */
static void dos_list(struct CD *c)
{
    if ((c->add || c->rem) && (ULONG)AttemptLockDosList(LDF_VOLUMES | LDF_WRITE) > 1) {
        if (c->rem) {
            RemDosEntry(c->rem);
            FreeDosEntry(c->rem);
            c->rem = 0;
        }
        if (c->add) {
            AddDosEntry(c->vol);
            c->add = 0;
        }
        UnLockDosList(LDF_VOLUMES | LDF_WRITE);
    }
}

/* Every second: a disc out, a new one in. */
static void poll(struct CD *c)
{
    int in;
    if (c->add || c->rem)
        return;                                 /* the DOS list first */
    c->io.io_Command = TD_CHANGESTATE;
    in = !DoIO((struct IORequest *)&c->io) && !c->io.io_Actual;
    c->io.io_Command = TD_CHANGENUM;
    DoIO((struct IORequest *)&c->io);
    if (!in || c->io.io_Actual != c->changes) {
        if (c->vol)
            unmount(c);
        c->changes = c->io.io_Actual;
        c->tried = 0;
    }
    if (in && !c->vol && !c->tried) {
        c->tried = 1;
        mount(c);
    }
}

static void tick(struct CD *c)
{
    c->treq.tr_node.io_Command = TR_ADDREQUEST;
    c->treq.tr_time.tv_secs = 1;
    c->treq.tr_time.tv_micro = 0;
    SendIO((struct IORequest *)&c->treq);
}

static void init_port(struct MsgPort *mp, struct Task *me)
{
    mp->mp_Node.ln_Type = NT_MSGPORT;
    mp->mp_SigBit = AllocSignal(-1);
    mp->mp_SigTask = me;
    NewList(&mp->mp_MsgList);
}

/* The handler process. DOS sends the startup packet first. */
static void cdfs_main(void)
{
    struct Process *me = (struct Process *)FindTask(0);
    struct CD cd, *c = &cd;
    struct DosPacket *dp;
    struct FileSysStartupMsg *fssm;
    struct DeviceNode *dn;
    struct Message *m;

    memset(c, 0, sizeof cd);
    c->port = &me->pr_MsgPort;
    WaitPort(c->port);
    m = (struct Message *)GetMsg(c->port);
    dp = (struct DosPacket *)m->mn_Node.ln_Name;
    fssm = (struct FileSysStartupMsg *)BADDR(dp->dp_Arg2);
    dn = (struct DeviceNode *)BADDR(dp->dp_Arg3);

    init_port(&c->ioport, &me->pr_Task);
    init_port(&c->tport, &me->pr_Task);
    c->io.io_Message.mn_ReplyPort = c->inp.io_Message.mn_ReplyPort = &c->ioport;
    c->treq.tr_node.io_Message.mn_ReplyPort = &c->tport;
    if (!(DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 36)) ||
        !(c->buf = (UBYTE *)AllocMem(SECTOR, MEMF_PUBLIC)) ||
        OpenDevice((STRPTR)dev_name, fssm->fssm_Unit, (struct IORequest *)&c->io, 0) ||
        OpenDevice((STRPTR)timer_name, UNIT_VBLANK, (struct IORequest *)&c->treq, 0)) {
        reply(c, dp, DOSFALSE, ERROR_NO_FREE_STORE);
        return;                                 /* resources stay: this is rare */
    }
    OpenDevice((STRPTR)"input.device", 0, (struct IORequest *)&c->inp, 0);
    if (c->inp.io_Error)
        c->inp.io_Device = 0;
    c->io.io_Command = TD_CHANGENUM;
    DoIO((struct IORequest *)&c->io);
    c->changes = c->io.io_Actual;
    c->tried = 1;
    mount(c);
    dn->dn_Task = c->port;
    reply(c, dp, DOSTRUE, 0);
    tick(c);
    for (;;) {
        dos_list(c);
        Wait(1UL << c->port->mp_SigBit | 1UL << c->tport.mp_SigBit);
        while ((m = (struct Message *)GetMsg(c->port))) {
            LONG err = 0, r;
            dp = (struct DosPacket *)m->mn_Node.ln_Name;
            r = packet(c, dp, &err);
            reply(c, dp, r, err);
        }
        if (GetMsg(&c->tport)) {
            poll(c);
            tick(c);
        }
    }
}

/* A seglist for DOS to start the handler from: a jump into the driver. */
static ULONG cdfs_seg[4];

static BPTR cdfs_seglist(void)
{
    ULONG a = (ULONG)cdfs_main;
    cdfs_seg[0] = sizeof cdfs_seg;
    cdfs_seg[2] = 0x4EF90000UL | a >> 16;       /* jmp a.l */
    cdfs_seg[3] = a << 16;
    CacheClearU();
    return MKBADDR(&cdfs_seg[1]);
}
