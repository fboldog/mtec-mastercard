/* MTecDump - locate mtecscsi.device and dump its ROM or RAM image.
 *
 *   MTecDump                         report only, never touches board space;
 *                                    ends with the dump commands to run
 *   MTecDump NAME=x.device           report on a different device
 *   MTecDump ADDR=hex LEN=hex TO=RAM:mtec.rom   dump that range
 *
 * The dump refuses a range that is not inside RAM or one autoconfig board
 * unless FORCE is given. Each block is read twice and differences counted.
 */
/* AmigaOS enters at the first byte of the first hunk, so this must stay ahead
   of every include: the call stubs are real functions and would land first. */
__asm("  .text\n  jmp _start\n");

#include <exec/execbase.h>
#include <exec/devices.h>
#include <exec/resident.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <libraries/configvars.h>

#define VERSTR "1.1"
#define CHUNK  4096UL
#define RAM_MARGIN 0x10000UL
#define ROM_ALIGN  0x10000UL

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *ExpansionBase;

#include "gen/stubs.h"

LONG start(void);

static const char vers[] __attribute__((used)) = "$VER: MTecDump " VERSTR " (30.10.2026)";
static const char tmpl[] = "NAME/K,ADDR/K,LEN/K,TO/K,FORCE/S";

#define P(fmt, ...) do { ULONG a_[] = { 0, ##__VA_ARGS__ }; VPrintf((STRPTR)fmt, (LONG *)(a_ + 1)); } while (0)

/* GCC can emit calls to this even with no C library linked. */
__attribute__((used)) void *memset(void *d, int c, unsigned long n)
{
    UBYTE *p = d;
    while (n--) *p++ = (UBYTE)c;
    return d;
}

static int parse_hex(const char *s, ULONG *out)
{
    ULONG v = 0;
    int n = 0;
    if (s[0] == '$') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++, n++) {
        char c = *s;
        if (c >= '0' && c <= '9') c -= '0';
        else if (c >= 'a' && c <= 'f') c -= 'a' - 10;
        else if (c >= 'A' && c <= 'F') c -= 'A' - 10;
        else return 0;
        v = (v << 4) | (ULONG)c;
    }
    *out = v;
    return n > 0 && n <= 8;
}

static int same_str(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static const char *safe_name(const char *s)
{
    return s ? s : "(no name)";
}

static void list_devices(void)
{
    struct Node *n;
    P("Devices in exec's list (base, Open vector):\n");
    Forbid();
    for (n = SysBase->DeviceList.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        UWORD *jmp = (UWORD *)((UBYTE *)n - 6);
        ULONG vec = (*jmp == 0x4EF9) ? *(ULONG *)(jmp + 1) : 0;
        P("  %08lx %08lx %s\n", (ULONG)n, vec, (ULONG)safe_name(n->ln_Name));
    }
    Permit();
}

static void list_resident(void)
{
    ULONG *p = (ULONG *)SysBase->ResModules;
    P("Resident modules outside $F80000-$FFFFFF:\n");
    while (p && *p) {
        if (*p & 0x80000000UL) {
            p = (ULONG *)(*p & 0x7FFFFFFFUL);
            continue;
        }
        if (*p < 0xF80000UL || *p > 0xFFFFFFUL) {
            struct Resident *r = (struct Resident *)*p;
            P("  %08lx v%ld pri %ld %s\n", (ULONG)r, (ULONG)r->rt_Version,
              (ULONG)(LONG)r->rt_Pri, (ULONG)safe_name(r->rt_Name));
        }
        p++;
    }
}

static const char *const lvo_names[] = {
    "Open", "Close", "Expunge", "Reserved", "BeginIO", "AbortIO"
};

static void report_device(struct Device *d, ULONG *lo, ULONG *hi)
{
    struct Library *l = &d->dd_Library;
    UWORD i, count = l->lib_NegSize / 6;
    *lo = 0xFFFFFFFFUL;
    *hi = 0;
    P("\nDevice %s at %08lx\n", (ULONG)safe_name(l->lib_Node.ln_Name), (ULONG)d);
    P("  version %ld.%ld  open count %ld  negsize %ld  possize %ld\n",
      (ULONG)l->lib_Version, (ULONG)l->lib_Revision, (ULONG)l->lib_OpenCnt,
      (ULONG)l->lib_NegSize, (ULONG)l->lib_PosSize);
    if (l->lib_IdString)
        P("  id: %s\n", (ULONG)l->lib_IdString);
    P("  Jump table:\n");
    for (i = 0; i < count; i++) {
        UWORD *jmp = (UWORD *)((UBYTE *)d - (i + 1) * 6);
        ULONG vec;
        if (*jmp != 0x4EF9)
            break;
        vec = *(ULONG *)(jmp + 1);
        if (vec && vec < *lo) *lo = vec;   /* the Reserved slot is often 0 */
        if (vec > *hi) *hi = vec;
        if (i < 6)
            P("   -%3ld %08lx %s\n", (ULONG)((i + 1) * 6), vec, (ULONG)lvo_names[i]);
        else
            P("   -%3ld %08lx private\n", (ULONG)((i + 1) * 6), vec);
    }
    if (*hi)
        P("  Vectors span %08lx - %08lx\n", *lo, *hi);
}

/* Returns the number of boards. If addr is set, *inside gets the board holding
   [addr, addr+len) or stays NULL. */
static int walk_boards(int print, ULONG lo, ULONG hi, ULONG addr, ULONG len,
                       struct ConfigDev **inside)
{
    struct ConfigDev *cd = NULL;
    int n = 0;
    if (print) P("\nAutoconfig boards:\n");
    while ((cd = (struct ConfigDev *)FindConfigDev(cd, -1, -1))) {
        ULONG base = (ULONG)cd->cd_BoardAddr, size = cd->cd_BoardSize;
        n++;
        if (print) {
            P("  mfr %ld prod %ld at %08lx size %08lx serial %08lx\n",
              (ULONG)cd->cd_Rom.er_Manufacturer, (ULONG)cd->cd_Rom.er_Product,
              base, size, cd->cd_Rom.er_SerialNumber);
            P("     type %02lx flags %02lx diagvec %04lx cd_flags %02lx driver %08lx%s\n",
              (ULONG)cd->cd_Rom.er_Type, (ULONG)cd->cd_Rom.er_Flags,
              (ULONG)cd->cd_Rom.er_InitDiagVec, (ULONG)cd->cd_Flags,
              (ULONG)cd->cd_Driver,
              (ULONG)((hi && lo >= base && hi < base + size) ? "  <-- driver code here" : ""));
        }
        if (inside && addr >= base && addr - base < size && len <= size - (addr - base))
            *inside = cd;
    }
    if (print && !n) P("  none\n");
    return n;
}

static void report_location(ULONG lo, ULONG hi)
{
    if (!hi)
        return;
    if (TypeOfMem((APTR)lo)) {
        P("\nDriver code is in RAM (mem attrs %08lx).\n", TypeOfMem((APTR)lo));
        P("The ROM copied it there; dump around the vectors, it is tied to this boot's addresses.\n");
    } else {
        P("\nDriver code is not in system RAM, so it probably runs from ROM.\n");
    }
}

/* A board is worth dumping if it holds the driver or has a boot ROM. Memory
   boards are left out: they would only dump their own RAM. */
static int board_wanted(struct ConfigDev *cd, ULONG lo, ULONG hi)
{
    ULONG base = (ULONG)cd->cd_BoardAddr;
    if (cd->cd_Rom.er_Type & ERTF_MEMLIST)
        return 0;
    if (hi && lo >= base && hi < base + cd->cd_BoardSize)
        return 1;
    return (cd->cd_Rom.er_Type & ERTF_DIAGVALID) != 0;
}

static void print_cmd(ULONG addr, ULONG len, const char *to, const char *force)
{
    P("  MTecDump ADDR=%08lx LEN=%08lx TO=RAM:%s%s\n", addr, len, (ULONG)to, (ULONG)force);
}

/* The driver may reach past its vectors, so take some room around them and
   keep inside the memory region that holds them. */
static void ram_range(ULONG lo, ULONG hi, ULONG *start, ULONG *end)
{
    struct MemHeader *mh;
    *start = (lo >= RAM_MARGIN ? lo - RAM_MARGIN : 0) & ~(CHUNK - 1);
    *end = (hi + RAM_MARGIN + CHUNK - 1) & ~(CHUNK - 1);
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head; mh->mh_Node.ln_Succ;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ) {
        if (lo >= (ULONG)mh->mh_Lower && lo < (ULONG)mh->mh_Upper) {
            if (*start < (ULONG)mh->mh_Lower) *start = (ULONG)mh->mh_Lower;
            if (*end > (ULONG)mh->mh_Upper) *end = (ULONG)mh->mh_Upper;
            break;
        }
    }
    Permit();
}

/* Stays the last thing printed, so the commands end up at the bottom of the
   report. */
static void suggest(ULONG lo, ULONG hi)
{
    struct ConfigDev *cd = NULL, *holder = NULL;
    int boards = 0, shown = 0;
    int in_ram = hi && TypeOfMem((APTR)lo);
    static char name[] = "board0.rom";

    while ((cd = (struct ConfigDev *)FindConfigDev(cd, -1, -1)))
        if (board_wanted(cd, lo, hi)) boards++;
    if (hi && !in_ram)
        walk_boards(0, 0, 0, lo, hi - lo + 1, &holder);

    if (boards) {
        P("\nBoard dumps also read the chips on the board, so wait for the disks\n");
        P("to go idle first.\n");
    }
    if (hi && !in_ram && !holder) {
        P("\nNo RAM or board claims the driver's addresses, so that dump needs FORCE.\n");
        P("An unmapped read can hang the machine.\n");
    }
    P("\nSuggested dump commands:\n");
    if (in_ram) {
        ULONG start, end;
        ram_range(lo, hi, &start, &end);
        print_cmd(start, end - start, "mtec.ram", "");
        shown++;
    } else if (hi && !holder) {
        ULONG start = lo & ~(ROM_ALIGN - 1), end = (hi + ROM_ALIGN) & ~(ROM_ALIGN - 1);
        print_cmd(start, end - start, "mtec.rom", " FORCE");
        shown++;
    }
    cd = NULL;
    while ((cd = (struct ConfigDev *)FindConfigDev(cd, -1, -1))) {
        if (!board_wanted(cd, lo, hi))
            continue;
        if (cd == holder) {
            print_cmd((ULONG)cd->cd_BoardAddr, cd->cd_BoardSize, "mtec.rom", "");
        } else {
            print_cmd((ULONG)cd->cd_BoardAddr, cd->cd_BoardSize, name, "");
            if (name[5] < '9') name[5]++;
        }
        shown++;
    }
    if (!shown)
        P("  none, no driver found and no board has a boot ROM\n");
}

static LONG dump(ULONG addr, ULONG len, const char *to, LONG force)
{
    struct ConfigDev *board = NULL;
    struct Process *me;
    APTR oldwin;
    UBYTE *a, *b;
    BPTR f;
    ULONG done = 0, diffs = 0, first_diff = 0;
    LONG rc = RETURN_OK;

    if (!len) {
        P("LEN must not be 0\n");
        return RETURN_ERROR;
    }
    if (addr + len < addr) {
        P("Range wraps past the end of memory\n");
        return RETURN_ERROR;
    }
    walk_boards(0, 0, 0, addr, len, &board);
    if (!board && !(TypeOfMem((APTR)addr) && TypeOfMem((APTR)(addr + len - 1))) && !force) {
        P("%08lx+%08lx is not inside RAM or a single board. Not reading it.\n", addr, len);
        P("Add FORCE only if you know it is mapped; an unmapped read can hang the machine.\n");
        return RETURN_ERROR;
    }

    a = (UBYTE *)AllocMem(CHUNK * 2, MEMF_PUBLIC);
    if (!a) {
        P("Out of memory\n");
        return RETURN_FAIL;
    }
    b = a + CHUNK;
    /* A mistyped volume would otherwise wait on an "insert volume" requester. */
    me = (struct Process *)FindTask(NULL);
    oldwin = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1L;
    f = Open((STRPTR)to, MODE_NEWFILE);
    me->pr_WindowPtr = oldwin;
    if (!f) {
        PrintFault(IoErr(), (STRPTR)to);
        FreeMem(a, CHUNK * 2);
        return RETURN_FAIL;
    }

    P("Dumping %08lx, %08lx bytes to %s\n", addr, len, (ULONG)to);
    while (done < len) {
        ULONG n = len - done, i;
        volatile UBYTE *src = (volatile UBYTE *)(addr + done);
        if (n > CHUNK) n = CHUNK;
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
            P("Stopped by CTRL-C, file is partial\n");
            rc = RETURN_WARN;
            break;
        }
        /* Byte reads, so the access width is the same on 8- and 16-bit ROMs. */
        CacheClearU();
        for (i = 0; i < n; i++) a[i] = src[i];
        CacheClearU();
        for (i = 0; i < n; i++) b[i] = src[i];
        for (i = 0; i < n; i++) {
            if (a[i] != b[i]) {
                if (!diffs) first_diff = addr + done + i;
                diffs++;
            }
        }
        if (Write(f, a, n) != n) {
            PrintFault(IoErr(), (STRPTR)"Write");
            rc = RETURN_FAIL;
            break;
        }
        done += n;
    }
    Close(f);
    FreeMem(a, CHUNK * 2);

    P("Wrote %08lx bytes\n", done);
    if (diffs) {
        P("WARNING: %ld bytes changed between reads, first at %08lx.\n", diffs, first_diff);
        P("That range is not plain ROM (registers or live RAM).\n");
        if (rc == RETURN_OK) rc = RETURN_WARN;
    } else if (done) {
        P("Both reads matched.\n");
    }
    return rc;
}

static LONG run(void)
{
    LONG args[5] = { 0, 0, 0, 0, 0 };
    struct RDArgs *rd;
    const char *name = "mtecscsi.device";
    LONG rc = RETURN_OK;

    rd = (struct RDArgs *)ReadArgs((STRPTR)tmpl, args, NULL);
    if (!rd) {
        PrintFault(IoErr(), (STRPTR)"MTecDump");
        return RETURN_ERROR;
    }
    if (args[0]) name = (const char *)args[0];

    if (args[1] || args[2] || args[3]) {
        ULONG addr, len;
        if (!args[1] || !args[2] || !args[3] ||
            !parse_hex((char *)args[1], &addr) || !parse_hex((char *)args[2], &len)) {
            P("Dump needs ADDR=hex LEN=hex TO=file\n");
            rc = RETURN_ERROR;
        } else {
            rc = dump(addr, len, (const char *)args[3], args[4]);
        }
    } else {
        struct Device *d = NULL;
        struct Node *n;
        ULONG lo = 0, hi = 0;

        P("MTecDump " VERSTR "  Kickstart %ld.%ld\n\n",
          (ULONG)SysBase->LibNode.lib_Version, (ULONG)SysBase->SoftVer);
        list_devices();
        P("\n");
        list_resident();

        Forbid();
        for (n = SysBase->DeviceList.lh_Head; n->ln_Succ; n = n->ln_Succ)
            if (n->ln_Name && same_str(n->ln_Name, name)) { d = (struct Device *)n; break; }
        Permit();

        if (d)
            report_device(d, &lo, &hi);
        else
            P("\n%s is not in exec's device list.\n", (ULONG)name);
        walk_boards(1, lo, hi, 0, 0, NULL);
        report_location(lo, hi);
        suggest(lo, hi);
    }
    FreeArgs(rd);
    return rc;
}

LONG start(void)
{
    struct Process *me;
    struct Message *wb = NULL;
    LONG rc = RETURN_FAIL;

    SysBase = *(struct ExecBase **)4;
    me = (struct Process *)FindTask(NULL);
    if (!me->pr_CLI) {
        WaitPort(&me->pr_MsgPort);
        wb = (struct Message *)GetMsg(&me->pr_MsgPort);
    }
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
    if (DOSBase) {
        ExpansionBase = (struct Library *)OpenLibrary((STRPTR)"expansion.library", 0);
        if (ExpansionBase) {
            if (!wb) rc = run();
            CloseLibrary(ExpansionBase);
        }
        CloseLibrary((struct Library *)DOSBase);
    }
    if (wb) {
        Forbid();
        ReplyMsg(wb);
    }
    return rc;
}
