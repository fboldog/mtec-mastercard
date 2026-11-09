/* bootui.c - the optional boot menu of the ROM build.
 *
 * Holding the right mouse button at power-on opens it; both buttons stay
 * Kickstart's own Early Startup menu. It runs from the driver's init, after
 * intuition.library and before dos.library, so it uses the system's own look:
 * a screen like Workbench's (SA_LikeWorkbench, the default 3D pens and the
 * system font) with a GadTools window. gadtools.library is in the Kickstart
 * ROM but is only started on first use, by ramlib once DOS runs, so the menu
 * starts it itself with FindResident and InitResident, as ramlib would.
 *
 * It shows a table of the devices the driver found, each followed by the
 * partitions of its RDB, with * or - for mount and boot, then continues the
 * boot (Return) or skips the card's partitions for this boot (Esc). Anything
 * that fails to open falls back to a normal boot.
 */
#include <exec/execbase.h>
#include <exec/resident.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <graphics/rastport.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <utility/tagitem.h>
#include <devices/hardblocks.h>
#include "mc.h"

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase;

#include "gen/bootstubs.h"

void *memset(void *d, int c, unsigned long n);

#define POTINP (*(volatile UWORD *)0xDFF016)
#define CIAAPRA (*(volatile UBYTE *)0xBFE001)
#define POTINP_RMB 0x0400
#define CIAAPRA_LMB 0x40

#define GID_BOOT 1
#define GID_SKIP 2

#define ROWS   7                        /* SCSI IDs 0-6 */
#define COLS   7
#define CELL   24                       /* longest cell text */
#define MARGIN 16                       /* window border to content */
#define PAD_X  8                        /* table box edge to text */
#define PAD_V  2                        /* box edge to the letters, above and below */

#ifdef IRQXFER
#define MODE_TEXT "interrupt mode"
#else
#define MODE_TEXT "polled mode"
#endif

static const char title[] = "M-Tec MasterCard SCSI";
static const char wtitle[] = "MasterCard SCSI boot options";
static const char info[] = "mastercard.device 0.1, " MODE_TEXT ", asynchronous SCSI";
static const char none[] = "No SCSI devices answered.";

/* Column widths in characters, and which are right-aligned. */
static const UBYTE colw[COLS] = { 4, 5, 4, 20, 6, 4, 8 };
#define RIGHT(c) ((c) == 5 || (c) == 6)
static const char *const head[COLS] = {
    "Unit", "Mount", "Boot", "Device / Partition", "Type", "Pri", "Size"
};

/* Active-low buttons. Requiring left released leaves both buttons to
   Kickstart's Early Startup menu. */
int bootui_requested(void)
{
#ifdef MC_BOOTUI_ALWAYS
    return 1;                           /* emulator-only validation build */
#else
    return !(POTINP & POTINP_RMB) && (CIAAPRA & CIAAPRA_LMB);
#endif
}

/* ---- cell text ---- */

static char *put(char *d, const char *s, int n)
{
    while (n-- && *s)
        *d++ = *s++;
    return d;
}

static char *num(char *d, LONG v)
{
    char t[10];
    int n = 0;
    if (v < 0) {
        *d++ = '-';
        v = -v;
    }
    do
        t[n++] = '0' + v % 10;
    while ((v /= 10) && n < 10);
    while (n)
        *d++ = t[--n];
    return d;
}

static char *size(char *d, ULONG mb)
{
    if (mb >= 10000)
        return put(num(d, mb >> 10), " GB", 3);
    return put(num(d, mb), " MB", 3);
}

/* Vendor and product from INQUIRY, single-spaced. */
static char *ident(char *d, const char *s)
{
    char *start = d;
    int k, sp = 0;
    for (k = 0; k < 24; k++) {
        if (s[k] == ' ') {
            sp = 1;
            continue;
        }
        if (sp && d > start)
            *d++ = ' ';
        sp = 0;
        *d++ = s[k];
        if (k == 7)
            sp = 1;                     /* vendor and product are separate */
    }
    return d;
}

static const char *type_name(UBYTE t)
{
    switch (t) {
    case 0x00: return "Disk";
    case 0x01: return "Tape";
    case 0x04: return "WORM";
    case 0x05: return "CD-ROM";
    case 0x07: return "Optical";
    default:   return "Other";
    }
}

/* Fills the cells of row r: each device, then its partitions. Returns 0 past
   the end. Cell c holds end[c] characters. */
static int row(struct MC *mc, UWORD r, char cells[COLS][CELL], UBYTE end[COLS])
{
    UWORD k, i;
    for (k = 0; k < ROWS; k++) {
        struct MCUnit *u = mc->units[k];
        if (!u)
            continue;
        memset(end, 0, COLS);
        if (!r--) {
            int boot = 0;
            for (i = 0; i < mc->nparts; i++)
                if (mc->parts[i].target == k &&
                    (mc->parts[i].flags & (PBFF_BOOTABLE | PBFF_NOMOUNT)) == PBFF_BOOTABLE)
                    boot = 1;
            cells[0][0] = '0' + k;
            end[0] = 1;
            cells[1][0] = '*';
            end[1] = 1;
            cells[2][0] = boot ? '*' : '-';
            end[2] = 1;
            end[3] = ident(cells[3], u->ident) - cells[3];
            end[4] = put(cells[4], type_name(u->type), 7) - cells[4];
            if (u->shift && u->blocks)
                end[6] = size(cells[6], u->blocks >> (20 - u->shift)) - cells[6];
            return 1;
        }
        for (i = 0; i < mc->nparts; i++) {
            struct MCPart *p = &mc->parts[i];
            char *d;
            int b;
            if (p->target != k || r--)
                continue;
            cells[1][0] = p->flags & PBFF_NOMOUNT ? '-' : '*';
            end[1] = 1;
            cells[2][0] = (p->flags & (PBFF_BOOTABLE | PBFF_NOMOUNT)) == PBFF_BOOTABLE ? '*' : '-';
            end[2] = 1;
            end[3] = put(cells[3], p->name, 11) - cells[3];
            d = cells[4];
            for (b = 24; b > 0; b -= 8) {
                UBYTE ch = p->dostype >> b;
                *d++ = ch >= ' ' && ch < 127 ? ch : '?';
            }
            *d++ = '\\';
            b = p->dostype & 0xFF;
            *d++ = b < 10 ? '0' + b : b >= ' ' && b < 127 ? b : '?';
            end[4] = d - cells[4];
            end[5] = num(cells[5], p->bootpri) - cells[5];
            end[6] = size(cells[6], p->mb) - cells[6];
            return 1;
        }
    }
    return 0;
}

/* ---- drawing ---- */

static void text(struct RastPort *rp, UWORD pen, WORD x, WORD y, const char *s, int n)
{
    SetAPen(rp, pen);
    Move(rp, x, y);
    Text(rp, (STRPTR)s, n);
}

/* Returns non-zero only when the user chose to skip mounting this boot. */
int bootui_show(struct MC *mc)
{
    static UWORD pens[] = { (UWORD)~0 };
    struct Screen *scr = 0;
    struct Window *win = 0;
    struct DrawInfo *dri = 0;
    struct Gadget *glist = 0, *g;
    APTR vi = 0;
    struct NewGadget ng;
    struct Resident *rt;
    struct RastPort *rp;
    char cells[COLS][CELL];
    UBYTE end[COLS];
    WORD colx[COLS + 1];
    int done = 0, skip = 0;
    WORD fh, top, h, y, x, cw, bw, ww, boxh, padt, chw, rowh;
    UWORD k, c, rows = 0, maxrows;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39);
    GadToolsBase = (struct Library *)OpenLibrary((STRPTR)"gadtools.library", 39);
    if (!GadToolsBase && (rt = (struct Resident *)FindResident((STRPTR)"gadtools.library")))
        if (InitResident(rt, 0))
            GadToolsBase = (struct Library *)OpenLibrary((STRPTR)"gadtools.library", 39);
    if (!IntuitionBase || !GfxBase || !GadToolsBase)
        goto out;
    {
        struct TagItem st[] = {
            { SA_LikeWorkbench, TRUE },
            { SA_Pens, (ULONG)pens },
            { SA_Title, (ULONG)title },
            { SA_ShowTitle, TRUE },
            { SA_Quiet, FALSE },
            { TAG_DONE, 0 }
        };
        if (!(scr = (struct Screen *)OpenScreenTagList(0, st)))
            goto out;
    }
    if (!(dri = (struct DrawInfo *)GetScreenDrawInfo(scr)) || !(vi = (APTR)GetVisualInfoA(scr, 0)))
        goto out;

    /* Layout, from the screen's font. The columns sit one character apart and
       each cell is drawn at its own column, so they line up in any font. The
       letters end at the font's baseline; the rows below it are for
       descenders, so the top gets them as extra padding and the text sits
       centred in the box. */
    fh = scr->Font->ta_YSize;
    rowh = fh + 2;
    top = scr->WBorTop + fh + 1;        /* below the window title bar */
    chw = TextLength(&scr->RastPort, (STRPTR)"0", 1);
    colx[0] = 0;
    for (c = 0; c < COLS; c++)
        colx[c + 1] = colx[c] + (colw[c] + 1) * chw;
    cw = colx[COLS] - chw + 2 * PAD_X;
    padt = PAD_V + (fh - 1 - scr->RastPort.TxBaseline);
    while (row(mc, rows, cells, end))
        rows++;
    /* As many rows as fit below the title bars, header, info line and buttons. */
    maxrows = (scr->Height - scr->BarHeight - top - scr->WBorBottom - 3 * rowh - 2 * fh - 40) / rowh;
    if (rows > maxrows)
        rows = maxrows;
    boxh = 1 + padt + (rows ? rows + 1 : 2) * rowh + 4 - 2 + PAD_V + 1;     /* header, line, rows */
    bw = TextLength(&scr->RastPort, (STRPTR)"Skip card this boot", 19) + 24;
    ww = scr->WBorLeft + MARGIN + cw + MARGIN + scr->WBorRight;
    if (ww > scr->Width)
        ww = scr->Width;
    h = top + 6 + boxh + 8 + fh + 10 + (fh + 6) + 6 + scr->WBorBottom;

    if (!(g = (struct Gadget *)CreateContext(&glist)))
        goto out;
    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr = scr->Font;
    ng.ng_VisualInfo = vi;
    ng.ng_Width = bw;
    ng.ng_Height = fh + 6;
    ng.ng_TopEdge = h - scr->WBorBottom - 6 - ng.ng_Height;
    ng.ng_LeftEdge = scr->WBorLeft + MARGIN;
    ng.ng_GadgetText = (STRPTR)"_Boot";
    ng.ng_GadgetID = GID_BOOT;
    {
        struct TagItem gt[] = { { GT_Underscore, '_' }, { TAG_DONE, 0 } };
        g = (struct Gadget *)CreateGadgetA(BUTTON_KIND, g, &ng, gt);
        ng.ng_LeftEdge = scr->WBorLeft + MARGIN + cw - bw;
        ng.ng_GadgetText = (STRPTR)"_Skip card this boot";
        ng.ng_GadgetID = GID_SKIP;
        g = (struct Gadget *)CreateGadgetA(BUTTON_KIND, g, &ng, gt);
    }
    if (!g)
        goto out;
    {
        struct TagItem wt[] = {
            { WA_Left, (scr->Width - ww) / 2 },
            { WA_Top, (scr->Height - h) / 2 },
            { WA_Width, ww },
            { WA_Height, h },
            { WA_Title, (ULONG)wtitle },
            { WA_CustomScreen, (ULONG)scr },
            { WA_Gadgets, (ULONG)glist },
            { WA_IDCMP, BUTTONIDCMP | IDCMP_VANILLAKEY | IDCMP_REFRESHWINDOW },
            { WA_Flags, WFLG_ACTIVATE | WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SMART_REFRESH | WFLG_RMBTRAP },
            { TAG_DONE, 0 }
        };
        if (!(win = (struct Window *)OpenWindowTagList(0, wt)))
            goto out;
    }
    GT_RefreshWindow(win, 0);
    rp = win->RPort;

    /* The table, in a recessed box: a header, a line, then the rows, with a
       line between the columns. */
    x = win->BorderLeft + MARGIN;
    y = top + 6;
    {
        struct TagItem bt[] = { { GT_VisualInfo, (ULONG)vi }, { GTBB_Recessed, TRUE }, { TAG_DONE, 0 } };
        DrawBevelBoxA(rp, x, y, cw, boxh, bt);
    }
    {
        WORD tx = x + PAD_X, ty = y + 1 + padt, line_y = ty + rowh + 1;
        for (c = 0; c < COLS; c++) {
            int n = 0;
            while (head[c][n])
                n++;
            text(rp, dri->dri_Pens[HIGHLIGHTTEXTPEN],
                 tx + colx[c] + (RIGHT(c) ? colw[c] * chw - TextLength(rp, (STRPTR)head[c], n) : 0),
                 ty + rp->TxBaseline, head[c], n);
        }
        SetAPen(rp, dri->dri_Pens[SHADOWPEN]);
        Move(rp, x + 2, line_y);
        Draw(rp, x + cw - 3, line_y);
        for (c = 1; c < COLS; c++) {
            Move(rp, tx + colx[c] - chw / 2 - 1, y + 1);
            Draw(rp, tx + colx[c] - chw / 2 - 1, y + boxh - 2);
        }
        ty += rowh + 4;
        if (!rows)
            text(rp, dri->dri_Pens[TEXTPEN], tx + colx[3], ty + rp->TxBaseline, none, sizeof none - 1);
        for (k = 0; k < rows; k++) {
            row(mc, k, cells, end);
            for (c = 0; c < COLS; c++)
                if (end[c]) {
                    int n = end[c] > colw[c] ? colw[c] : end[c];
                    text(rp, dri->dri_Pens[TEXTPEN],
                         tx + colx[c] + (RIGHT(c) ? colw[c] * chw - TextLength(rp, (STRPTR)cells[c], n) : 0),
                         ty + rp->TxBaseline, cells[c], n);
                }
            ty += rowh;
        }
    }
    y += boxh + 8;
    text(rp, dri->dri_Pens[TEXTPEN], x, y + rp->TxBaseline, info, sizeof info - 1);

    while (!done) {
        struct IntuiMessage *m;
        Wait(1UL << win->UserPort->mp_SigBit);
        while ((m = (struct IntuiMessage *)GT_GetIMsg(win->UserPort)) != 0) {
            ULONG cls = m->Class;
            UWORD code = m->Code;
            struct Gadget *mg = (struct Gadget *)m->IAddress;
            GT_ReplyIMsg(m);
            if (cls == IDCMP_GADGETUP) {
                done = 1;
                skip = mg->GadgetID == GID_SKIP;
            } else if (cls == IDCMP_VANILLAKEY) {
                if (code == '\r' || code == 'b' || code == 'B') {
                    done = 1;
                } else if (code == 27 || code == 's' || code == 'S') {
                    done = skip = 1;
                }
            } else if (cls == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(win);
                GT_EndRefresh(win, TRUE);
            }
        }
    }
out:
    if (win)
        CloseWindow(win);
    if (glist)
        FreeGadgets(glist);
    if (vi)
        FreeVisualInfo(vi);
    if (dri)
        FreeScreenDrawInfo(scr, dri);
    if (scr)
        CloseScreen(scr);
    if (GadToolsBase)
        CloseLibrary(GadToolsBase);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    return skip;
}
