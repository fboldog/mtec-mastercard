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
 * It lists the devices the driver found, then continues the boot (Return)
 * or skips the card's partitions for this boot (Esc). Anything that fails
 * to open falls back to a normal boot.
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

#define ROWS  7                         /* SCSI IDs 0-6 */
#define BUTTON_W_MIN 200
#define MARGIN 16                       /* window border to content */
#define PAD_X  10                       /* list box edge to text */
#define PAD_V  2                        /* box edge to the letters, above and below */

#ifdef IRQXFER
#define MODE_TEXT "interrupt mode"
#else
#define MODE_TEXT "polled mode"
#endif

static const char title[] = "M-Tec MasterCard SCSI";
static const char wtitle[] = "MasterCard SCSI boot options";
static const char heading[] = "Devices found";
static const char info[] = "mastercard.device 0.1, " MODE_TEXT ", asynchronous SCSI";
static const char none[] = "No SCSI devices answered.";

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

static char *put(char *d, const char *s, int n)
{
    while (n-- && *s)
        *d++ = *s++;
    return d;
}

static char *num(char *d, ULONG v)
{
    char t[10];
    int n = 0;
    do
        t[n++] = '0' + v % 10;
    while ((v /= 10) && n < 10);
    while (n)
        *d++ = t[--n];
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

/* "ID 1  QUANTUM  BlueSCSI Pico      1.0   Disk     8044 MB" */
static int line(struct MCUnit *u, char *b)
{
    char *d = b;
    d = put(d, "ID ", 3);
    *d++ = '0' + u->target;
    *d++ = ' ';
    *d++ = ' ';
    d = put(d, u->ident, 8);            /* vendor */
    *d++ = ' ';
    d = put(d, u->ident + 8, 16);       /* product */
    *d++ = ' ';
    d = put(d, u->ident + 24, 4);       /* revision */
    *d++ = ' ';
    *d++ = ' ';
    d = put(d, type_name(u->type), 8);
    if (u->shift && u->blocks) {
        *d++ = ' ';
        *d++ = ' ';
        d = num(d, u->blocks >> (20 - u->shift));
        d = put(d, " MB", 3);
    }
    return d - b;
}

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
    int done = 0, skip = 0;
    WORD fh, top, h, y, x, tw, bw, ww, lw, cw, boxh, padt;
    UWORD k, rows = 0;

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
    fh = scr->Font->ta_YSize;
    top = scr->WBorTop + fh + 1;        /* below the window title bar */
    /* Layout, all from the screen's font. One content width cw is shared by
       the heading, the list box, the info line and the two buttons, inset
       MARGIN from the window borders. List text sits PAD_X inside the box.
       Vertically the letters end at the font's baseline and the rows below it
       are only for descenders, so the top gets those rows as extra padding:
       the visible text then sits centred in any font (topaz 8, topaz 9...). */
    lw = 0;
    for (k = 0; k < ROWS; k++)
        if (mc->units[k]) {
            char b[64];
            WORD w = TextLength(&scr->RastPort, (STRPTR)b, line(mc->units[k], b));
            if (w > lw)
                lw = w;
            rows++;
        }
    if (!rows)
        lw = TextLength(&scr->RastPort, (STRPTR)none, sizeof none - 1);
    cw = lw + 2 * PAD_X;
    tw = TextLength(&scr->RastPort, (STRPTR)info, sizeof info - 1);
    if (tw > cw)
        cw = tw;
    bw = TextLength(&scr->RastPort, (STRPTR)"Skip card this boot", 19) + 24;
    if (bw < BUTTON_W_MIN)
        bw = BUTTON_W_MIN;
    if (2 * bw + MARGIN > cw)
        cw = 2 * bw + MARGIN;
    ww = scr->WBorLeft + MARGIN + cw + MARGIN + scr->WBorRight;
    if (ww > scr->Width)
        ww = scr->Width;
    padt = PAD_V + (fh - 1 - scr->RastPort.TxBaseline);
    boxh = 1 + padt + (rows ? rows : 1) * (fh + 2) - 2 + PAD_V + 1;
    h = top + 6 + fh + 4 + boxh + 8 + fh + 10 + (fh + 6) + 6 + scr->WBorBottom;

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

    /* The device list, in a recessed box like a list view. */
    x = win->BorderLeft + MARGIN;
    y = top + 6;
    text(win->RPort, dri->dri_Pens[HIGHLIGHTTEXTPEN], x, y + win->RPort->TxBaseline, heading, sizeof heading - 1);
    y += fh + 4;
    {
        struct TagItem bt[] = { { GT_VisualInfo, (ULONG)vi }, { GTBB_Recessed, TRUE }, { TAG_DONE, 0 } };
        DrawBevelBoxA(win->RPort, x, y, cw, boxh, bt);
    }
    {
        WORD ty = y + 1 + padt;
        if (!rows)
            text(win->RPort, dri->dri_Pens[TEXTPEN], x + PAD_X, ty + win->RPort->TxBaseline, none, sizeof none - 1);
        for (k = 0; k < ROWS; k++)
            if (mc->units[k]) {
                char b[64];
                int n = line(mc->units[k], b);
                text(win->RPort, dri->dri_Pens[TEXTPEN], x + PAD_X, ty + win->RPort->TxBaseline, b, n);
                ty += fh + 2;
            }
    }
    y += boxh + 8;
    text(win->RPort, dri->dri_Pens[TEXTPEN], x, y + win->RPort->TxBaseline, info, sizeof info - 1);

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
