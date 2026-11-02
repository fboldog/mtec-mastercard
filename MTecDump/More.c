/* More - small text pager that needs nothing but dos.library.
 *
 *   More FILE
 *
 * Space next page, Return or cursor down next line, B or cursor up back,
 * Q, Esc or CTRL-C quit. Redirected output gets the whole file.
 */
/* AmigaOS enters at the first byte of the first hunk, so this must stay ahead
   of every include: the call stubs are real functions and would land first. */
__asm("  .text\n  jmp _start\n");

#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>

#define CSI 0x9B

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *ExpansionBase;

#include "gen/stubs.h"

LONG start(void);

static const char vers[] __attribute__((used)) = "$VER: More 1.0 (30.10.2026)";

#define P(fmt, ...) do { ULONG a_[] = { 0, ##__VA_ARGS__ }; VPrintf((STRPTR)fmt, (LONG *)(a_ + 1)); } while (0)

/* GCC can emit calls to this even with no C library linked. */
__attribute__((used)) void *memset(void *d, int c, unsigned long n)
{
    UBYTE *p = d;
    while (n--) *p++ = (UBYTE)c;
    return d;
}

static UBYTE *text;
static ULONG size;
static ULONG wrap = 79, page = 20;

/* One screen row: stops after a newline or at the wrap width. */
static ULONG next_row(ULONG p)
{
    ULONG c = 0;
    while (p < size) {
        if (text[p++] == '\n')
            break;
        if (++c == wrap) {
            if (p < size && text[p] == '\n') p++;
            break;
        }
    }
    return p;
}

static ULONG count_rows(void)
{
    ULONG p = 0, n = 0;
    while (p < size) {
        p = next_row(p);
        n++;
    }
    return n;
}

static void draw(ULONG top, ULONG total)
{
    BPTR out = Output();
    ULONG p = 0, i, last = top + page;

    if (last > total) last = total;
    for (i = 0; i < top; i++)
        p = next_row(p);
    Write(out, "\f", 1);
    for (; i < last; i++) {
        ULONG e = next_row(p);
        Write(out, text + p, e - p);
        if (text[e - 1] != '\n') Write(out, "\n", 1);
        p = e;
    }
    P("\x9B" "7m rows %ld-%ld of %ld%s  Space next, B back, Q quit \x9B" "0m",
      top + 1, last, total, (ULONG)(last == total ? " (end)" : ""));
    Flush(out);
}

static int get_key(BPTR in)
{
    UBYTE c;
    if (Read(in, &c, 1) != 1)
        return 'q';
    if (c != CSI)
        return c;
    /* Keep only the last byte of a control sequence: A up, B down. */
    while (Read(in, &c, 1) == 1 && (c < 0x40 || c > 0x7E))
        ;
    return c == 'A' ? 'b' : c == 'B' ? '\r' : 0;
}

/* Asks the console for its size: the answer is CSI 1;1;rows;cols r */
static void window_size(BPTR in)
{
    UBYTE c;
    ULONG num[4] = { 0, 0, 0, 0 };
    int i = 0;

    Write(Output(), "\x9B" "0 q", 4);
    while (WaitForChar(in, 500000) && Read(in, &c, 1) == 1 && c != 'r') {
        if (c == ';') {
            if (i < 3) i++;
        } else if (c >= '0' && c <= '9') {
            num[i] = num[i] * 10 + (c - '0');
        }
    }
    if (i == 3 && num[2] >= 3 && num[3] >= 20) {
        page = num[2] - 1;
        wrap = num[3] - 1;
    }
}

static void view(void)
{
    BPTR in = Input();
    ULONG top = 0, total, max;

    SetMode(in, 1);
    window_size(in);
    total = count_rows();
    max = total > page ? total - page : 0;
    for (;;) {
        int k;
        draw(top, total);
        k = get_key(in);
        if (k == 'q' || k == 'Q' || k == 3 || k == 27)
            break;
        if (k == ' ' || k == 'f') {
            top += page;
        } else if (k == '\r' || k == '\n') {
            top++;
        } else if (k == 'b' || k == 'B' || k == 8) {
            top = top > page ? top - page : 0;
        }
        if (top > max) top = max;
    }
    Write(Output(), "\n", 1);
    SetMode(in, 0);
}

static LONG run(void)
{
    LONG args[1] = { 0 };
    struct RDArgs *rd;
    BPTR f;
    LONG rc = RETURN_OK;

    rd = (struct RDArgs *)ReadArgs((STRPTR)"FILE/A", args, NULL);
    if (!rd) {
        PrintFault(IoErr(), (STRPTR)"More");
        return RETURN_ERROR;
    }
    f = Open((STRPTR)args[0], MODE_OLDFILE);
    if (f) {
        Seek(f, 0, OFFSET_END);
        size = Seek(f, 0, OFFSET_BEGINNING);
        if ((LONG)size > 0 && (text = (UBYTE *)AllocMem(size, MEMF_PUBLIC))) {
            if ((ULONG)Read(f, text, size) == size) {
                if (IsInteractive(Input()) && IsInteractive(Output()))
                    view();
                else
                    Write(Output(), text, size);
            } else {
                PrintFault(IoErr(), (STRPTR)args[0]);
                rc = RETURN_FAIL;
            }
            FreeMem(text, size);
        } else if ((LONG)size > 0) {
            P("Out of memory\n");
            rc = RETURN_FAIL;
        }
        Close(f);
    } else {
        PrintFault(IoErr(), (STRPTR)args[0]);
        rc = RETURN_FAIL;
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
