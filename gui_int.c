/*
 * GUI_INT.C - MarranoFTPI: user interface made only with intuition.library
 * and graphics.library V33 calls (Kickstart 1.2/1.3 and newer).
 *
 * No GadTools, no emulation library: buttons are boolean gadgets with
 * Border structures, the text fields are string gadgets, the lists are drawn
 * with Text() and scrolled by a proportional gadget plus two arrow buttons,
 * the menu strip is built from Menu/MenuItem/IntuiText structures.
 * Everything is static: the only allocations are the text of the lists.
 *
 * Like gui_mui.c and gui_gt.c this file knows nothing about FTP: it forwards
 * the user actions to the core_* functions (core.h) and implements gui.h.
 *
 * File lists: a click marks/unmarks a line ('*'), so several files can be
 * selected for Download/Upload/Delete; a double click enters a directory or
 * transfers a file. If nothing is marked the last clicked line is used.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/text.h>
#include <graphics/rastport.h>
#include <string.h>
#include <stdio.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>

#include <caf/caf_types.h>
#include <caf/thread_helpers.h>
#include <caf/caf_platform.h>

#include "core.h"
#include "gui.h"
#include "gui_lines.h"

/* IntuitionBase/GfxBase: defined here, not left extern, so sc.lib does not
   auto-open them (SysBase/DOSBase belong to c.o) */
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;

/* c.o switches to this stack size if the shell gave less */
long __near __stack = 16384;

static const char VersTag[] = "$VER: MarranoFTPI 1.0 (13.11.2026) intuition/graphics, OS 1.3+";

/* ---------------------------------------------------------------------------
 * Layout constants (topaz 8 everywhere)
 * ------------------------------------------------------------------------- */
#define FH			8			/* font height */
#define FW			8			/* font width */
#define BTN_H		12
#define STR_H		12			/* string gadget frame */
#define GAP			2
#define SW			16			/* scroller column */
#define AH			9			/* arrow buttons */
#define MAX_COLS	99

#define STATUS_ROWS		3
#define QUEUE_ROWS		3
#define STATUS_MAX		48
#define STATUS_LEN		90

static struct TextAttr Topaz8 = { (STRPTR) "topaz.font", 8, 0, 0 };
static struct TextFont *g_font;

/* pens: 1.3 = blue/white/black/orange, 2.0+ = grey/black/white/blue */
static UBYTE g_bg = 0, g_text = 1, g_shine = 1, g_shadow = 2, g_fill = 3, g_hitext = 2;

/* ---------------------------------------------------------------------------
 * IDs
 * ------------------------------------------------------------------------- */
enum {
	GID_LPATH = 1, GID_RPATH,
	GID_CONNECT, GID_STOP, GID_DOWNLOAD, GID_UPLOAD, GID_DELETE, GID_LPARENT, GID_RPARENT,
	GID_REFRESHL, GID_REFRESHR, GID_RUNQ, GID_STEPQ, GID_CLEARQ, GID_SITES, GID_QUIT,

	GID_AB_NAME = 100, GID_AB_HOST, GID_AB_PORT, GID_AB_USER, GID_AB_PASS,
	GID_AB_TYPE, GID_AB_NEW, GID_AB_APPLY, GID_AB_DELETE, GID_AB_CONNECT, GID_AB_SAVE, GID_AB_CLOSE,

	GID_GS_TRANSFER = 200, GID_GS_CONNECT, GID_GS_QUEUE, GID_GS_PARTIAL, GID_GS_SAVE, GID_GS_CANCEL,

	/* scroller gadgets of the lists, UserData points to the IList */
	GID_PROP = 1000, GID_UP, GID_DOWN
};

enum { MN_SITES, MN_SETTINGS, MN_SAVECFG, MN_ABOUT, MN_QUIT, MN_COUNT };

/* labels of the toggle buttons: same length, they are drawn with JAM2 */
static char *ConnTypes[] = { "Active ", "Passive", "SFTP   " };
static char *QueueTypes[] = { "Stop at error", "Process all  " };
static char *PartialTypes[] = { "Delete", "Keep  " };

/* ---------------------------------------------------------------------------
 * Widgets
 * ------------------------------------------------------------------------- */
typedef struct IWin {
	struct Window *win;
	struct Gadget *first, *last;
} IWin;

typedef struct Btn {
	struct Gadget g;
	struct Border b1, b2;
	WORD xy1[6], xy2[6];
	struct IntuiText it;
} Btn;

typedef struct Str {
	struct Gadget g;
	struct StringInfo si;
	struct Border b1, b2;
	WORD xy1[6], xy2[6];
	UBYTE buf[512];
} Str;

typedef char *(*LineFn)(int i);

typedef struct IList {
	struct Window *win;
	WORD x, y, w, h;			/* frame */
	WORD tx, ty;				/* first character */
	int rows, cols, top, count, cursor;
	LineList *ll;				/* text from a LineList ... */
	LineFn fn;					/* ... or from a function */
	struct Gadget prop;
	struct PropInfo pi;
	struct Image knob;
	Btn up, down;
} IList;

static UBYTE g_undo[512];

static void add_gad(IWin *w, struct Gadget *g)
{
	g->NextGadget = NULL;
	if (w->last)
		w->last->NextGadget = g;
	else
		w->first = g;
	w->last = g;
}

/* Two Border structures: raised (button) or recessed (string field) */
static void make_bevel(struct Border *b1, struct Border *b2, WORD *xy1, WORD *xy2,
					   WORD left, WORD top, WORD w, WORD h, bool recessed)
{
	xy1[0] = 0;		xy1[1] = h - 1;	xy1[2] = 0;		xy1[3] = 0;		xy1[4] = w - 1;	xy1[5] = 0;
	xy2[0] = 1;		xy2[1] = h - 1;	xy2[2] = w - 1;	xy2[3] = h - 1;	xy2[4] = w - 1;	xy2[5] = 1;
	b1->LeftEdge = b2->LeftEdge = left;
	b1->TopEdge = b2->TopEdge = top;
	b1->FrontPen = recessed ? g_shadow : g_shine;
	b2->FrontPen = recessed ? g_shine : g_shadow;
	b1->BackPen = b2->BackPen = 0;
	b1->DrawMode = b2->DrawMode = JAM1;
	b1->Count = b2->Count = 3;
	b1->XY = xy1;
	b2->XY = xy2;
	b1->NextBorder = b2;
	b2->NextBorder = NULL;
}

static void btn_init(IWin *w, Btn *b, WORD x, WORD y, WORD wd, WORD h, char *label, UWORD id)
{
	caf_memset(b, 0, sizeof(Btn));
	make_bevel(&b->b1, &b->b2, b->xy1, b->xy2, 0, 0, wd, h, false);
	b->it.FrontPen = g_text;
	b->it.BackPen = g_bg;
	b->it.DrawMode = JAM2;
	b->it.LeftEdge = (wd - (WORD) strlen(label) * FW) / 2;
	b->it.TopEdge = (h - FH) / 2;
	b->it.ITextFont = &Topaz8;
	b->it.IText = (UBYTE *) label;
	b->g.LeftEdge = x;
	b->g.TopEdge = y;
	b->g.Width = wd;
	b->g.Height = h;
	b->g.Flags = GADGHCOMP;
	b->g.Activation = RELVERIFY;
	b->g.GadgetType = BOOLGADGET;
	b->g.GadgetRender = (APTR) &b->b1;
	b->g.GadgetText = &b->it;
	b->g.GadgetID = id;
	add_gad(w, &b->g);
}

/* Changes the text of a toggle button (labels of the same length) */
static void btn_label(Btn *b, struct Window *win, char *label)
{
	b->it.IText = (UBYTE *) label;
	if (win)
		RefreshGList(&b->g, win, NULL, 1);
}

static void str_init(IWin *w, Str *s, WORD x, WORD y, WORD wd, int maxchars, UWORD id)
{
	caf_memset(s, 0, sizeof(Str));
	if (maxchars > (int) sizeof(s->buf) - 1)
		maxchars = sizeof(s->buf) - 1;
	make_bevel(&s->b1, &s->b2, s->xy1, s->xy2, -4, -2, wd, STR_H, true);
	s->si.Buffer = s->buf;
	s->si.UndoBuffer = g_undo;
	s->si.MaxChars = maxchars + 1;
	s->g.LeftEdge = x + 4;
	s->g.TopEdge = y + 2;
	s->g.Width = wd - 8;
	s->g.Height = STR_H - 3;
	s->g.Flags = GADGHCOMP;
	s->g.Activation = RELVERIFY;
	s->g.GadgetType = STRGADGET;
	s->g.GadgetRender = (APTR) &s->b1;
	s->g.SpecialInfo = (APTR) &s->si;
	s->g.GadgetID = id;
	add_gad(w, &s->g);
}

static void str_set(Str *s, struct Window *win, const char *text)
{
	UWORD pos = 0;

	if (win)
		pos = RemoveGadget(win, &s->g);
	caf_strncpy((char *) s->buf, (char *) (text ? text : ""), s->si.MaxChars - 1);
	s->si.BufferPos = 0;
	s->si.DispPos = 0;
	if (win) {
		AddGadget(win, &s->g, pos);
		RefreshGList(&s->g, win, NULL, 1);
	}
}

static char *str_get(Str *s)
{
	return (char *) s->buf;
}

/* ---------------------------------------------------------------------------
 * Drawing helpers
 * ------------------------------------------------------------------------- */
static void bevel(struct RastPort *rp, WORD x, WORD y, WORD w, WORD h, bool recessed)
{
	SetAPen(rp, recessed ? g_shadow : g_shine);
	Move(rp, x, y + h - 1);
	Draw(rp, x, y);
	Draw(rp, x + w - 1, y);
	SetAPen(rp, recessed ? g_shine : g_shadow);
	Move(rp, x + 1, y + h - 1);
	Draw(rp, x + w - 1, y + h - 1);
	Draw(rp, x + w - 1, y + 1);
}

/* Text padded with blanks to cols characters */
static void text_at(struct RastPort *rp, WORD x, WORD y, const char *s, int cols, UBYTE fg, UBYTE bg)
{
	char buf[MAX_COLS + 1];

	if (cols > MAX_COLS)
		cols = MAX_COLS;
	if (cols <= 0)
		return;
	gl_put_field(buf, s, cols, false);
	SetDrMd(rp, JAM2);
	SetAPen(rp, fg);
	SetBPen(rp, bg);
	Move(rp, x, y + rp->TxBaseline);
	Text(rp, (STRPTR) buf, cols);
}

static void label_at(struct RastPort *rp, WORD x, WORD y, const char *s)
{
	text_at(rp, x, y, s, strlen(s), g_text, g_bg);
}

/* ---------------------------------------------------------------------------
 * Lists
 * ------------------------------------------------------------------------- */
static int il_count(IList *l)
{
	return l->ll ? l->ll->count : l->count;
}

static char *il_text(IList *l, int i)
{
	return l->ll ? ll_line(l->ll, i) : l->fn(i);
}

static void il_layout(IWin *w, IList *l, WORD x, WORD y, WORD wd, WORD h, LineList *ll, LineFn fn)
{
	WORD sx = x + wd - 1 - SW;

	caf_memset(l, 0, sizeof(IList));
	l->x = x;
	l->y = y;
	l->w = wd;
	l->h = h;
	l->tx = x + 3;
	l->ty = y + 2;
	l->rows = (h - 4) / FH;
	l->cols = (sx - 1 - l->tx) / FW;
	if (l->cols > MAX_COLS)
		l->cols = MAX_COLS;
	l->cursor = -1;
	l->ll = ll;
	l->fn = fn;

	l->pi.Flags = AUTOKNOB | FREEVERT;
	l->pi.HorizBody = MAXBODY;
	l->pi.VertBody = MAXBODY;
	l->prop.LeftEdge = sx + 1;
	l->prop.TopEdge = y + 1;
	l->prop.Width = SW - 2;
	l->prop.Height = h - 3 - 2 * AH;
	l->prop.Flags = GADGHNONE;
	l->prop.Activation = GADGIMMEDIATE | RELVERIFY | FOLLOWMOUSE;
	l->prop.GadgetType = PROPGADGET;
	l->prop.GadgetRender = (APTR) &l->knob;
	l->prop.SpecialInfo = (APTR) &l->pi;
	l->prop.GadgetID = GID_PROP;
	l->prop.UserData = (APTR) l;
	add_gad(w, &l->prop);

	btn_init(w, &l->up, sx, y + h - 1 - 2 * AH, SW, AH, "^", GID_UP);
	btn_init(w, &l->down, sx, y + h - 1 - AH, SW, AH, "v", GID_DOWN);
	l->up.g.Activation |= GADGIMMEDIATE;
	l->down.g.Activation |= GADGIMMEDIATE;
	l->up.g.UserData = (APTR) l;
	l->down.g.UserData = (APTR) l;
}

static void il_draw(IList *l)
{
	struct RastPort *rp;
	int r, n = il_count(l);

	if (!l->win)
		return;
	rp = l->win->RPort;
	for (r = 0; r < l->rows; r++) {
		int i = l->top + r;
		bool hi = (bool) (i == l->cursor && i < n);

		text_at(rp, l->tx, l->ty + r * FH, i < n ? il_text(l, i) : "", l->cols,
				hi ? g_hitext : g_text, hi ? g_fill : g_bg);
	}
}

static void il_prop(IList *l)
{
	int n = il_count(l), max = n - l->rows;
	UWORD body = MAXBODY, pot = 0;

	if (!l->win)
		return;
	if (max > 0) {
		body = (UWORD) (((ULONG) MAXBODY * l->rows) / n);
		pot = (UWORD) (((ULONG) MAXPOT * l->top) / max);
	}
	NewModifyProp(&l->prop, l->win, NULL, AUTOKNOB | FREEVERT, 0, pot, MAXBODY, body, 1);
}

static void il_set_top(IList *l, int top, bool move_knob)
{
	int max = il_count(l) - l->rows;

	if (top > max)
		top = max;
	if (top < 0)
		top = 0;
	if (top != l->top) {
		l->top = top;
		il_draw(l);
	}
	if (move_knob)
		il_prop(l);
}

/* the contents changed: redraw everything, keep top if possible */
static void il_refresh(IList *l)
{
	int top = l->top;

	l->top = -1;				/* il_set_top() always redraws */
	il_set_top(l, top, true);
}

static void il_from_knob(IList *l)
{
	int max = il_count(l) - l->rows;

	if (max <= 0)
		return;
	il_set_top(l, (int) (((ULONG) l->pi.VertPot * max + MAXPOT / 2) / MAXPOT), false);
}

/* line under the mouse, -1 if none */
static int il_hit(IList *l, WORD mx, WORD my)
{
	int i;

	if (!l->win || mx < l->tx || mx >= l->tx + l->cols * FW || my < l->ty || my >= l->ty + l->rows * FH)
		return -1;
	i = l->top + (my - l->ty) / FH;
	return i < il_count(l) ? i : -1;
}

static void il_frame(IList *l)
{
	struct RastPort *rp = l->win->RPort;

	bevel(rp, l->x, l->y, l->w - SW, l->h, true);
}

/* scroller events, shared by all the windows */
static IList *g_drag, *g_rep;
static struct Gadget *g_rep_gad;
static int g_rep_dir;

static bool scroll_event(ULONG class, struct Gadget *gad)
{
	IList *l;

	if (class == MOUSEMOVE) {
		if (g_drag)
			il_from_knob(g_drag);
		return true;
	}
	if (class == INTUITICKS) {
		if (g_rep && (g_rep_gad->Flags & SELECTED))
			il_set_top(g_rep, g_rep->top + g_rep_dir, true);
		return true;
	}
	if ((class != GADGETDOWN && class != GADGETUP) || !gad || gad->GadgetID < GID_PROP)
		return false;
	l = (IList *) gad->UserData;
	if (gad->GadgetID == GID_PROP) {
		il_from_knob(l);
		g_drag = class == GADGETDOWN ? l : NULL;
	} else if (class == GADGETDOWN) {
		g_rep = l;
		g_rep_gad = gad;
		g_rep_dir = gad->GadgetID == GID_UP ? -1 : 1;
		il_set_top(l, l->top + g_rep_dir, true);
	} else
		g_rep = NULL;
	return true;
}

static void forget_scroll(void)
{
	g_drag = NULL;
	g_rep = NULL;
}

/* ---------------------------------------------------------------------------
 * Windows
 * ------------------------------------------------------------------------- */
static WORD g_scr_w, g_scr_h, g_title_h, g_bottom_h;
static IWin g_main, g_ab, g_gs;

static struct Window *open_window(WORD left, WORD top, WORD w, WORD h, char *title, ULONG idcmp)
{
	struct NewWindow nw;
	struct Window *win;

	caf_memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = left;
	nw.TopEdge = top;
	nw.Width = w;
	nw.Height = h;
	nw.DetailPen = (UBYTE) -1;
	nw.BlockPen = (UBYTE) -1;
	nw.IDCMPFlags = idcmp;
	nw.Flags = WINDOWDRAG | WINDOWDEPTH | WINDOWCLOSE | ACTIVATE | SMART_REFRESH | NOCAREREFRESH;
	nw.Title = (UBYTE *) title;
	nw.Type = WBENCHSCREEN;
	win = OpenWindow(&nw);
	if (win && g_font)
		SetFont(win->RPort, g_font);
	return win;
}

static void attach(IWin *w)
{
	if (w->first) {
		AddGList(w->win, w->first, (UWORD) -1, -1, NULL);
		RefreshGList(w->first, w->win, NULL, -1);
	}
}

static void close_iwin(IWin *w)
{
	struct IntuiMessage *msg;

	forget_scroll();
	if (w->win) {
		if (w->win->MenuStrip)
			ClearMenuStrip(w->win);
		while ((msg = (struct IntuiMessage *) GetMsg(w->win->UserPort)))
			ReplyMsg((struct Message *) msg);
		CloseWindow(w->win);
	}
	w->win = NULL;
	w->first = w->last = NULL;
}

#define IDCMP_COMMON (CLOSEWINDOW | GADGETUP | GADGETDOWN | MOUSEBUTTONS | MOUSEMOVE | INTUITICKS)

/* ---------------------------------------------------------------------------
 * Menu (built by hand: no GadTools on 1.3)
 * ------------------------------------------------------------------------- */
static struct IntuiText g_mtext[MN_COUNT];
static struct MenuItem g_mitem[MN_COUNT];
static struct Menu g_menu;
static char *g_mlabels[MN_COUNT] = { "Address book...", "Settings...", "Save config", "About", "Quit" };
static char g_mkeys[MN_COUNT] = { 'A', 'S', 'W', 0, 'Q' };

static void build_menu(void)
{
	int i;
	WORD w = 17 * FW + COMMWIDTH + 8;

	for (i = 0; i < MN_COUNT; i++) {
		g_mtext[i].FrontPen = 0;
		g_mtext[i].BackPen = 1;
		g_mtext[i].DrawMode = JAM1;
		g_mtext[i].LeftEdge = 2;
		g_mtext[i].TopEdge = 1;
		g_mtext[i].ITextFont = &Topaz8;
		g_mtext[i].IText = (UBYTE *) g_mlabels[i];

		g_mitem[i].NextItem = i + 1 < MN_COUNT ? &g_mitem[i + 1] : NULL;
		g_mitem[i].LeftEdge = 0;
		g_mitem[i].TopEdge = i * (FH + 2);
		g_mitem[i].Width = w;
		g_mitem[i].Height = FH + 2;
		g_mitem[i].Flags = ITEMTEXT | ITEMENABLED | HIGHCOMP | (g_mkeys[i] ? COMMSEQ : 0);
		g_mitem[i].ItemFill = (APTR) &g_mtext[i];
		g_mitem[i].Command = g_mkeys[i];
	}
	g_menu.NextMenu = NULL;
	g_menu.LeftEdge = 0;
	g_menu.TopEdge = 0;
	g_menu.Width = 8 * FW + 8;
	g_menu.Height = FH + 2;
	g_menu.Flags = MENUENABLED;
	g_menu.MenuName = (BYTE *) "Project";
	g_menu.FirstItem = &g_mitem[0];
}

/* ---------------------------------------------------------------------------
 * Main window
 * ------------------------------------------------------------------------- */
static IList g_status_l, g_files_l[2], g_queue_l;
static Str g_path[2];
static Btn g_btn[14];
static WORD g_info_x, g_info_y, g_info_w;

static LineList g_files[2], g_queue;
static char g_marks[2][MAX_VIEW_ENTRIES];
static int g_nmarks[2];
static int g_selected[2] = { -1, -1 };
static ULONG g_click_s[2], g_click_us[2];
static int g_click_idx[2] = { -1, -1 };

static char g_status_text[STATUS_MAX][STATUS_LEN + 1];
static int g_status_first, g_status_n;
static char g_info_text[100];
static char g_title[64];

static char *status_line(int i)
{
	return g_status_text[(g_status_first + i) % STATUS_MAX];
}

static void draw_info(void)
{
	if (g_main.win)
		text_at(g_main.win->RPort, g_info_x + 4, g_info_y + 2, g_info_text, (g_info_w - 8) / FW, g_text, g_bg);
}

static void update_title(void)
{
	if (!g_main.win)
		return;
	strcpy(g_title, core_is_connected() ? "MarranoFTPI 1.0 - connected" : "MarranoFTPI 1.0 - not connected");
	SetWindowTitles(g_main.win, (UBYTE *) g_title, (UBYTE *) -1);
}

static void layout_main(void)
{
	struct Window *win = g_main.win;
	WORD x0 = win->BorderLeft + 4, y = win->BorderTop + GAP;
	WORD iw = win->Width - win->BorderLeft - win->BorderRight - 8;
	WORD ih = win->Height - win->BorderTop - win->BorderBottom - 2 * GAP;
	WORD half = (iw - 4) / 2, fixed, list_h, bw, sh, qh;
	int i, rows;
	static char *labels[14] = { "Connect", "Stop", "Download", "Upload", "Delete", "Parent L", "Parent R",
								"Refresh L", "Refresh R", "Run queue", "Step", "Clear queue", "Sites", "Quit" };
	static UWORD ids[14] = { GID_CONNECT, GID_STOP, GID_DOWNLOAD, GID_UPLOAD, GID_DELETE, GID_LPARENT, GID_RPARENT,
							 GID_REFRESHL, GID_REFRESHR, GID_RUNQ, GID_STEPQ, GID_CLEARQ, GID_SITES, GID_QUIT };

	sh = STATUS_ROWS * FH + 4;
	qh = QUEUE_ROWS * FH + 4;
	fixed = sh + GAP + STR_H + GAP + GAP + qh + GAP + BTN_H + GAP + BTN_H + GAP + BTN_H;
	rows = (ih - fixed - 4) / FH;
	if (rows < 3)
		rows = 3;
	list_h = rows * FH + 4;

	g_main.first = g_main.last = NULL;
	il_layout(&g_main, &g_status_l, x0, y, iw, sh, NULL, status_line);
	y += sh + GAP;
	str_init(&g_main, &g_path[GUI_LOCAL], x0, y, half, 500, GID_LPATH);
	str_init(&g_main, &g_path[GUI_REMOTE], x0 + half + 4, y, half, 500, GID_RPATH);
	y += STR_H + GAP;
	il_layout(&g_main, &g_files_l[GUI_LOCAL], x0, y, half, list_h, &g_files[GUI_LOCAL], NULL);
	il_layout(&g_main, &g_files_l[GUI_REMOTE], x0 + half + 4, y, half, list_h, &g_files[GUI_REMOTE], NULL);
	y += list_h + GAP;
	il_layout(&g_main, &g_queue_l, x0, y, iw, qh, &g_queue, NULL);
	y += qh + GAP;
	g_info_x = x0;
	g_info_y = y;
	g_info_w = iw;
	y += BTN_H + GAP;
	bw = (iw - 6 * 2) / 7;
	for (i = 0; i < 14; i++)
		btn_init(&g_main, &g_btn[i], x0 + (i % 7) * (bw + 2), y + (i / 7) * (BTN_H + GAP), bw, BTN_H, labels[i], ids[i]);
}

static void render_main(void)
{
	struct RastPort *rp = g_main.win->RPort;
	int i;

	g_status_l.win = g_files_l[0].win = g_files_l[1].win = g_queue_l.win = g_main.win;
	il_frame(&g_status_l);
	il_frame(&g_files_l[0]);
	il_frame(&g_files_l[1]);
	il_frame(&g_queue_l);
	bevel(rp, g_info_x, g_info_y, g_info_w, BTN_H, true);
	draw_info();
	g_status_l.count = g_status_n;
	il_refresh(&g_status_l);
	for (i = 0; i < 2; i++)
		il_refresh(&g_files_l[i]);
	il_refresh(&g_queue_l);
}

/* ---------------------------------------------------------------------------
 * Address book window
 * ------------------------------------------------------------------------- */
static IList g_ab_l;
static Str g_ab_str[5];
static Btn g_ab_type, g_ab_btn[6];
static int g_ab_sel = -1, g_ab_conn_type;
static bool g_ab_new;
static ULONG g_ab_click_s, g_ab_click_us;
static int g_ab_click_idx = -1;
static WORD g_ab_lx, g_ab_ly;

static char *host_line(int i)
{
	Host *h = core_host_get(i);

	return h ? h->entryname : "";
}

static void ab_show(int index)
{
	Host *h = core_host_get(index);
	static Host empty;
	struct Window *win = g_ab.win;

	if (!h) {
		caf_memset(&empty, 0, sizeof(empty));
		strcpy(empty.port, "21");
		h = &empty;
	}
	str_set(&g_ab_str[0], win, h->entryname);
	str_set(&g_ab_str[1], win, h->hostname);
	str_set(&g_ab_str[2], win, h->port);
	str_set(&g_ab_str[3], win, h->username);
	str_set(&g_ab_str[4], win, h->password);
	g_ab_conn_type = h->conn_type >= CTYPE_ACTV && h->conn_type <= CTYPE_SFTP ? h->conn_type : CTYPE_ACTV;
	btn_label(&g_ab_type, win, ConnTypes[g_ab_conn_type]);
}

static void ab_refresh(int sel)
{
	if (!g_ab.win)
		return;
	if (sel >= core_host_count())
		sel = core_host_count() - 1;
	g_ab_sel = sel;
	g_ab_new = false;
	g_ab_l.count = core_host_count();
	g_ab_l.cursor = sel;
	il_refresh(&g_ab_l);
	ab_show(sel);
}

static void close_ab(void)
{
	close_iwin(&g_ab);
	g_ab_l.win = NULL;
}

static void open_ab(void)
{
	WORD w = 600, h, x0, y, lw = 216, fx, fw, bw;
	static char *labels[] = { "Name", "Host", "Port", "User", "Password" };
	static UWORD ids[] = { GID_AB_NAME, GID_AB_HOST, GID_AB_PORT, GID_AB_USER, GID_AB_PASS };
	static int maxc[] = { ENTRYNAME_CONFIG_LEN - 1, HOSTNAME_CONFIG_LEN - 1, PORT_COFING_LEN - 1,
						  USERNAME_CONFIG_LEN - 1, PASSWORD_CONFIG_LEN - 1 };
	static char *blabels[] = { "New", "Apply", "Delete", "Connect", "Save cfg", "Close" };
	static UWORD bids[] = { GID_AB_NEW, GID_AB_APPLY, GID_AB_DELETE, GID_AB_CONNECT, GID_AB_SAVE, GID_AB_CLOSE };
	struct RastPort *rp;
	int i;

	if (g_ab.win) {
		WindowToFront(g_ab.win);
		ActivateWindow(g_ab.win);
		return;
	}
	if (w > g_scr_w)
		w = g_scr_w;
	h = g_title_h + GAP + 6 * (STR_H + GAP) + GAP + BTN_H + GAP + g_bottom_h;
	if (h > g_scr_h - 12)
		h = g_scr_h - 12;
	g_ab.win = open_window((g_scr_w - w) / 2, 12, w, h, "MarranoFTPI address book", IDCMP_COMMON);
	if (!g_ab.win) {
		gui_status("ERROR: Cannot open the address book window");
		return;
	}
	x0 = g_ab.win->BorderLeft + 4;
	y = g_ab.win->BorderTop + GAP;
	fx = x0 + lw + 8 + 9 * FW;
	fw = w - g_ab.win->BorderRight - 4 - fx;

	g_ab.first = g_ab.last = NULL;
	il_layout(&g_ab, &g_ab_l, x0, y, lw, ((6 * (STR_H + GAP) - GAP - 4) / FH) * FH + 4, NULL, host_line);
	for (i = 0; i < 5; i++)
		str_init(&g_ab, &g_ab_str[i], fx, y + i * (STR_H + GAP), fw, maxc[i], ids[i]);
	btn_init(&g_ab, &g_ab_type, fx, y + 5 * (STR_H + GAP), 9 * FW, BTN_H, ConnTypes[0], GID_AB_TYPE);
	g_ab_lx = fx - 9 * FW;
	g_ab_ly = y;
	y = g_ab.win->Height - g_ab.win->BorderBottom - GAP - BTN_H;
	bw = (w - g_ab.win->BorderLeft - g_ab.win->BorderRight - 8 - 5 * 4) / 6;
	for (i = 0; i < 6; i++)
		btn_init(&g_ab, &g_ab_btn[i], x0 + i * (bw + 4), y, bw, BTN_H, blabels[i], bids[i]);

	attach(&g_ab);
	rp = g_ab.win->RPort;
	for (i = 0; i < 5; i++)
		label_at(rp, g_ab_lx, g_ab_ly + i * (STR_H + GAP) + 2, labels[i]);
	label_at(rp, g_ab_lx, g_ab_ly + 5 * (STR_H + GAP) + 2, "Mode");
	g_ab_l.win = g_ab.win;
	il_frame(&g_ab_l);
	ab_refresh(core_host_count() ? 0 : -1);
}

static void ab_apply(void)
{
	Host h;

	caf_memset(&h, 0, sizeof(h));
	caf_strncpy(h.entryname, str_get(&g_ab_str[0]), ENTRYNAME_CONFIG_LEN);
	caf_strncpy(h.hostname, str_get(&g_ab_str[1]), HOSTNAME_CONFIG_LEN);
	caf_strncpy(h.port, str_get(&g_ab_str[2]), PORT_COFING_LEN);
	caf_strncpy(h.username, str_get(&g_ab_str[3]), USERNAME_CONFIG_LEN);
	caf_strncpy(h.password, str_get(&g_ab_str[4]), PASSWORD_CONFIG_LEN);
	h.conn_type = g_ab_conn_type;
	if (!h.entryname[0])
		caf_strncpy(h.entryname, h.hostname, ENTRYNAME_CONFIG_LEN);
	if (!h.hostname[0]) {
		gui_status("STATUS: the host name is empty");
		return;
	}
	if (g_ab_new || g_ab_sel < 0) {
		int idx = core_host_add(&h);
		if (idx < 0) {
			gui_status("STATUS: the address book is full");
			return;
		}
		g_ab_sel = idx;
	} else
		core_host_set(g_ab_sel, &h);
	ab_refresh(g_ab_sel);
}

static void ab_connect(int index)
{
	if (index < 0 || index >= core_host_count())
		return;
	close_ab();
	core_connect_host(index);
}

static void ab_event(ULONG class, UWORD code, struct Gadget *gad, WORD mx, WORD my, ULONG secs, ULONG micros)
{
	int i;

	if (class == CLOSEWINDOW) {
		close_ab();
		return;
	}
	if (scroll_event(class, gad))
		return;
	if (class == MOUSEBUTTONS && code == SELECTDOWN) {
		i = il_hit(&g_ab_l, mx, my);
		if (i < 0)
			return;
		if (i == g_ab_click_idx && DoubleClick(g_ab_click_s, g_ab_click_us, secs, micros)) {
			g_ab_click_idx = -1;
			ab_connect(i);
			return;
		}
		g_ab_click_idx = i;
		g_ab_click_s = secs;
		g_ab_click_us = micros;
		g_ab_sel = i;
		g_ab_new = false;
		g_ab_l.cursor = i;
		il_draw(&g_ab_l);
		ab_show(i);
		return;
	}
	if (class != GADGETUP)
		return;
	switch (gad->GadgetID) {
		case GID_AB_TYPE:
			g_ab_conn_type = (g_ab_conn_type + 1) % 3;
			btn_label(&g_ab_type, g_ab.win, ConnTypes[g_ab_conn_type]);
			break;
		case GID_AB_NEW:
			if (core_host_count() >= MAX_HOSTS) {
				gui_status("STATUS: the address book is full");
				break;
			}
			g_ab_new = true;
			g_ab_l.cursor = -1;
			il_draw(&g_ab_l);
			ab_show(-1);
			ActivateGadget(&g_ab_str[0].g, g_ab.win, NULL);
			break;
		case GID_AB_APPLY:	ab_apply(); break;
		case GID_AB_DELETE:
			if (g_ab_new)
				ab_refresh(g_ab_sel);
			else if (g_ab_sel >= 0) {
				core_host_delete(g_ab_sel);
				ab_refresh(g_ab_sel);
			}
			break;
		case GID_AB_CONNECT:
			if (g_ab_new)
				ab_apply();
			ab_connect(g_ab_sel);
			break;
		case GID_AB_SAVE:
			gui_status(core_save_config() ? "STATUS: Configuration saved" : "STATUS: Error while saving configuration");
			break;
		case GID_AB_CLOSE:	close_ab(); break;
	}
}

/* ---------------------------------------------------------------------------
 * Settings window
 * ------------------------------------------------------------------------- */
static Str g_gs_transfer, g_gs_connect;
static Btn g_gs_queue_b, g_gs_partial_b, g_gs_use, g_gs_cancel;
static int g_gs_queue, g_gs_partial;

static void close_gs(void)
{
	close_iwin(&g_gs);
}

static void open_gs(void)
{
	WORD w = 360, h, x0, y, fx;
	struct RastPort *rp;
	char num[16];

	if (g_gs.win) {
		WindowToFront(g_gs.win);
		ActivateWindow(g_gs.win);
		return;
	}
	h = g_title_h + GAP + 4 * (BTN_H + GAP) + GAP + BTN_H + GAP + g_bottom_h;
	g_gs.win = open_window((g_scr_w - w) / 2, 24, w, h, "MarranoFTPI settings", IDCMP_COMMON);
	if (!g_gs.win) {
		gui_status("ERROR: Cannot open the settings window");
		return;
	}
	x0 = g_gs.win->BorderLeft + 4;
	y = g_gs.win->BorderTop + GAP;
	fx = x0 + 21 * FW;
	g_gs_queue = core_get_setting(SETTING_QUEUE_TYPE) ? 1 : 0;
	g_gs_partial = core_get_setting(SETTING_KEEP_PARTIAL) ? 1 : 0;

	g_gs.first = g_gs.last = NULL;
	str_init(&g_gs, &g_gs_transfer, fx, y, 80, 4, GID_GS_TRANSFER);
	str_init(&g_gs, &g_gs_connect, fx, y + (BTN_H + GAP), 80, 4, GID_GS_CONNECT);
	btn_init(&g_gs, &g_gs_queue_b, fx, y + 2 * (BTN_H + GAP), 15 * FW, BTN_H, QueueTypes[g_gs_queue], GID_GS_QUEUE);
	btn_init(&g_gs, &g_gs_partial_b, fx, y + 3 * (BTN_H + GAP), 15 * FW, BTN_H, PartialTypes[g_gs_partial], GID_GS_PARTIAL);
	btn_init(&g_gs, &g_gs_use, x0, y + 4 * (BTN_H + GAP) + GAP, 100, BTN_H, "Use", GID_GS_SAVE);
	btn_init(&g_gs, &g_gs_cancel, w - g_gs.win->BorderRight - 4 - 100, y + 4 * (BTN_H + GAP) + GAP, 100, BTN_H, "Cancel", GID_GS_CANCEL);
	sprintf(num, "%d", core_get_setting(SETTING_TRANSFER_TIMEOUT));
	str_set(&g_gs_transfer, NULL, num);
	sprintf(num, "%d", core_get_setting(SETTING_CONNECT_TIMEOUT));
	str_set(&g_gs_connect, NULL, num);

	attach(&g_gs);
	rp = g_gs.win->RPort;
	label_at(rp, x0, y + 2, "Transfer timeout (s)");
	label_at(rp, x0, y + (BTN_H + GAP) + 2, "Connect timeout (s)");
	label_at(rp, x0, y + 2 * (BTN_H + GAP) + 2, "Queue");
	label_at(rp, x0, y + 3 * (BTN_H + GAP) + 2, "Partial downloads");
}

static void gs_event(ULONG class, struct Gadget *gad)
{
	if (class == CLOSEWINDOW) {
		close_gs();
		return;
	}
	if (class != GADGETUP)
		return;
	switch (gad->GadgetID) {
		case GID_GS_QUEUE:
			g_gs_queue ^= 1;
			btn_label(&g_gs_queue_b, g_gs.win, QueueTypes[g_gs_queue]);
			break;
		case GID_GS_PARTIAL:
			g_gs_partial ^= 1;
			btn_label(&g_gs_partial_b, g_gs.win, PartialTypes[g_gs_partial]);
			break;
		case GID_GS_SAVE:
			core_set_setting(SETTING_TRANSFER_TIMEOUT, caf_atol(str_get(&g_gs_transfer)));
			core_set_setting(SETTING_CONNECT_TIMEOUT, caf_atol(str_get(&g_gs_connect)));
			core_set_setting(SETTING_QUEUE_TYPE, g_gs_queue);
			core_set_setting(SETTING_KEEP_PARTIAL, g_gs_partial);
			close_gs();
			break;
		case GID_GS_CANCEL:
			close_gs();
			break;
	}
}

/* ---------------------------------------------------------------------------
 * File lists
 * ------------------------------------------------------------------------- */
static void set_mark(int which, int i, bool on)
{
	if (i < 0 || i >= g_files[which].count || g_marks[which][i] == on)
		return;
	g_marks[which][i] = on;
	g_nmarks[which] += on ? 1 : -1;
	ll_line(&g_files[which], i)[0] = on ? '*' : ' ';
}

static void list_click(int which, int i, ULONG secs, ULONG micros)
{
	IList *l = &g_files_l[which];

	if (i == g_click_idx[which] && DoubleClick(g_click_s[which], g_click_us[which], secs, micros)) {
		/* the first click of the double click toggled the mark: undo it */
		set_mark(which, i, !g_marks[which][i]);
		il_draw(l);
		g_click_idx[which] = -1;
		if (which == GUI_LOCAL)
			core_local_open(i);
		else
			core_remote_open(i);
		return;
	}
	g_click_idx[which] = i;
	g_click_s[which] = secs;
	g_click_us[which] = micros;
	g_selected[which] = i;
	l->cursor = i;
	if (!gl_is_parent(&g_files[which], i))
		set_mark(which, i, !g_marks[which][i]);
	il_draw(l);
}

/* ---------------------------------------------------------------------------
 * gui.h implementation
 * ------------------------------------------------------------------------- */
void gui_status(const char *text)
{
	int slot;

	if (g_status_n < STATUS_MAX)
		slot = (g_status_first + g_status_n++) % STATUS_MAX;
	else {
		slot = g_status_first;					/* recycle the oldest line */
		g_status_first = (g_status_first + 1) % STATUS_MAX;
	}
	caf_strncpy(g_status_text[slot], (char *) text, STATUS_LEN);
	g_status_l.count = g_status_n;
	if (g_status_l.win) {
		g_status_l.top = -1;					/* force the redraw */
		il_set_top(&g_status_l, g_status_n - g_status_l.rows, true);
	}
}

void gui_transfer_info(const char *text)
{
	caf_strncpy(g_info_text, (char *) text, sizeof(g_info_text) - 1);
	draw_info();
}

void gui_set_connected(bool connected)
{
	update_title();
	if (!connected) {
		ll_free(&g_files[GUI_REMOTE]);
		g_nmarks[GUI_REMOTE] = 0;
		g_selected[GUI_REMOTE] = -1;
		g_files_l[GUI_REMOTE].cursor = -1;
		g_files_l[GUI_REMOTE].top = 0;
		il_refresh(&g_files_l[GUI_REMOTE]);
	}
}

void gui_set_path(int which, const char *path)
{
	str_set(&g_path[which], g_main.win, path);
}

void gui_set_list(int which, ViewColumn **entries, int count)
{
	IList *l = &g_files_l[which];

	caf_memset(g_marks[which], 0, sizeof(g_marks[which]));
	g_nmarks[which] = 0;
	g_selected[which] = -1;
	g_click_idx[which] = -1;
	if (!gl_format_files(&g_files[which], entries, count, l->cols))
		gui_status("ERROR: not enough memory for the file list");
	l->cursor = -1;
	l->top = 0;
	il_refresh(l);
}

int gui_get_selection(int which, int *idx, int max)
{
	int i, n = 0;

	for (i = 0; i < g_files[which].count && n < max; i++)
		if (g_marks[which][i])
			idx[n++] = i;
	if (!n && g_selected[which] >= 0 && max > 0)
		idx[n++] = g_selected[which];
	if (g_nmarks[which]) {
		for (i = 0; i < g_files[which].count; i++)
			set_mark(which, i, false);
		il_draw(&g_files_l[which]);
	}
	return n;
}

void gui_queue_changed(void)
{
	if (!gl_format_queue(&g_queue, g_queue_l.cols ? g_queue_l.cols : 78))
		gui_status("ERROR: not enough memory for the queue list");
	il_refresh(&g_queue_l);
}

void gui_hosts_changed(void)
{
	if (g_ab.win)
		ab_refresh(0);
}

ULONG gui_signals(void)
{
	ULONG s = 0;

	if (g_main.win)
		s |= 1L << g_main.win->UserPort->mp_SigBit;
	if (g_ab.win)
		s |= 1L << g_ab.win->UserPort->mp_SigBit;
	if (g_gs.win)
		s |= 1L << g_gs.win->UserPort->mp_SigBit;
	return s;
}

static void about(void)
{
	gui_status("MarranoFTPI 1.0 - (C) Stefano Crosara aka Suppah");
	gui_status("Plain intuition/graphics user interface, Kickstart 1.2+");
}

/* Returns FALSE to quit at once */
static bool main_event(ULONG class, UWORD code, struct Gadget *gad, WORD mx, WORD my, ULONG secs, ULONG micros)
{
	int i;

	switch (class) {
		case CLOSEWINDOW:
			return false;

		case MENUPICK:
			while (code != MENUNULL) {
				struct MenuItem *item = ItemAddress(&g_menu, code);

				if (!item)
					break;
				switch (ITEMNUM(code)) {
					case MN_SITES:		open_ab(); break;
					case MN_SETTINGS:	open_gs(); break;
					case MN_SAVECFG:
						gui_status(core_save_config() ? "STATUS: Configuration saved" : "STATUS: Error while saving configuration");
						break;
					case MN_ABOUT:		about(); break;
					case MN_QUIT:		core_request_quit(); break;
				}
				code = item->NextSelect;
			}
			return true;

		case MOUSEBUTTONS:
			if (code == SELECTDOWN)
				for (i = 0; i < 2; i++) {
					int line = il_hit(&g_files_l[i], mx, my);
					if (line >= 0)
						list_click(i, line, secs, micros);
				}
			return true;
	}

	if (scroll_event(class, gad) || class != GADGETUP)
		return true;

	switch (gad->GadgetID) {
		case GID_LPATH:		core_set_local_path(str_get(&g_path[GUI_LOCAL])); break;
		case GID_RPATH:		core_set_remote_path(str_get(&g_path[GUI_REMOTE])); break;
		case GID_CONNECT:	core_connect_toggle(); break;
		case GID_STOP:		core_stop(); break;
		case GID_DOWNLOAD:	core_download_selected(); break;
		case GID_UPLOAD:	core_upload_selected(); break;
		case GID_DELETE:	core_delete_selected(); break;
		case GID_LPARENT:	core_local_parent(); break;
		case GID_RPARENT:	core_remote_parent(); break;
		case GID_REFRESHL:	core_set_local_path(str_get(&g_path[GUI_LOCAL])); break;
		case GID_REFRESHR:	core_remote_refresh(); break;
		case GID_RUNQ:		core_queue_run(); break;
		case GID_STEPQ:		core_queue_step(); break;
		case GID_CLEARQ:	core_queue_clear(); break;
		case GID_SITES:		open_ab(); break;
		case GID_QUIT:		core_request_quit(); break;
	}
	return true;
}

typedef struct IMsgCopy {
	ULONG class, secs, micros;
	UWORD code;
	WORD mx, my;
	struct Gadget *gad;
} IMsgCopy;

static bool next_msg(IWin *w, IMsgCopy *m)
{
	struct IntuiMessage *msg;

	if (!w->win || !(msg = (struct IntuiMessage *) GetMsg(w->win->UserPort)))
		return false;
	m->class = msg->Class;
	m->code = msg->Code;
	m->gad = (struct Gadget *) msg->IAddress;
	m->mx = msg->MouseX;
	m->my = msg->MouseY;
	m->secs = msg->Seconds;
	m->micros = msg->Micros;
	ReplyMsg((struct Message *) msg);
	return true;
}

bool gui_handle(ULONG sigs)
{
	IMsgCopy m;

	(void) sigs;
	while (next_msg(&g_main, &m))
		if (!main_event(m.class, m.code, m.gad, m.mx, m.my, m.secs, m.micros))
			return false;
	while (next_msg(&g_ab, &m))
		ab_event(m.class, m.code, m.gad, m.mx, m.my, m.secs, m.micros);
	while (next_msg(&g_gs, &m))
		gs_event(m.class, m.gad);
	return true;
}

/* ---------------------------------------------------------------------------
 * Open / close
 * ------------------------------------------------------------------------- */
bool gui_open(void)
{
	struct Screen scr;

	IntuitionBase = (struct IntuitionBase *) OpenLibrary((UBYTE *) "intuition.library", 33);
	GfxBase = (struct GfxBase *) OpenLibrary((UBYTE *) "graphics.library", 33);
	if (!IntuitionBase || !GfxBase) {
		printf("MarranoFTPI needs Kickstart 1.2 or newer\n");
		return false;
	}
	if (((struct Library *) SysBase)->lib_Version >= 36) {
		/* 2.0+ default pens: 0 grey, 1 black, 2 white, 3 blue */
		g_shine = 2;
		g_shadow = 1;
		g_hitext = 2;
	}
	ll_init(&g_files[0]);
	ll_init(&g_files[1]);
	ll_init(&g_queue);
	g_font = OpenFont(&Topaz8);

	if (!GetScreenData((APTR) &scr, sizeof(struct Screen), WBENCHSCREEN, NULL)) {
		printf("Cannot read the Workbench screen data\n");
		return false;
	}
	g_scr_w = scr.Width;
	g_scr_h = scr.Height;
	g_title_h = scr.WBorTop + (scr.Font ? scr.Font->ta_YSize : FH) + 1;
	g_bottom_h = scr.WBorBottom;

	g_main.win = open_window(0, scr.BarHeight + 1, g_scr_w, g_scr_h - scr.BarHeight - 1, "MarranoFTPI",
							 IDCMP_COMMON | MENUPICK);
	if (!g_main.win) {
		printf("Cannot open the main window\n");
		return false;
	}
	layout_main();
	str_set(&g_path[GUI_LOCAL], NULL, core_local_path());
	attach(&g_main);
	render_main();
	build_menu();
	SetMenuStrip(g_main.win, &g_menu);
	update_title();
	return true;
}

void gui_close(void)
{
	close_ab();
	close_gs();
	close_iwin(&g_main);
	g_status_l.win = g_files_l[0].win = g_files_l[1].win = g_queue_l.win = NULL;
	ll_free(&g_files[0]);
	ll_free(&g_files[1]);
	ll_free(&g_queue);
	if (g_font)
		CloseFont(g_font);
	g_font = NULL;
	if (GfxBase)
		CloseLibrary((struct Library *) GfxBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *) IntuitionBase);
	GfxBase = NULL;
	IntuitionBase = NULL;
}

int main(int argc, char *argv[])
{
	(void) VersTag;
	if (core_init() && gui_open()) {
		core_start();
		core_run();
	}
	gui_close();
	core_cleanup();
	return RETURN_OK;
}
