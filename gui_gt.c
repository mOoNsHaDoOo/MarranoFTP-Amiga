/*
 * GUI_GT.C - MarranoFTP13GD: GadTools user interface for Kickstart 1.3+.
 *
 * Uses GTE.lib by Massimo Tantignone (GadTools V37 emulation, public
 * domain) through its EF_ functions: the same code runs on 1.3 and on 2.x/3.x
 * (always in emulation, gadtools.library is never opened).
 * Only intuition/graphics V33 calls are used here: no OpenWindowTags(),
 * no LockPubScreen() on 1.3, no ASL, no MUI.
 *
 * Like gui_mui.c this file knows nothing about FTP: it forwards the user
 * actions to the core_* functions (core.h) and implements gui.h.
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
#include <libraries/gadtools.h>
#include <string.h>
#include <stdio.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>

#include <caf/caf_types.h>
#include <caf/thread_helpers.h>
#include <caf/caf_platform.h>

#include "GTE/nl_gadtools.h"
#include "core.h"
#include "gui.h"
#include "gui_lines.h"

/* IntuitionBase and GfxBase are defined in gt_bases.c (near data, for
   GTE.lib) */

/* c.o switches to this stack size if the shell gave less */
long __near __stack = 16384;

static const char VersTag[] = "$VER: MarranoFTP13GD 1.0 (13.11.2026) GadTools emulation, OS 1.3+";

/* ---------------------------------------------------------------------------
 * Layout constants (topaz 8 everywhere)
 * ------------------------------------------------------------------------- */
#define FONT_H		8
#define FONT_W		8
#define ROW_H		(FONT_H + 4)	/* buttons, strings, cycles */
#define GAP			2
#define LV_EXTRA	4				/* listview border */
#define SCROLLER_W	16

#define STATUS_ROWS		3
#define QUEUE_ROWS		3
#define STATUS_MAX		48			/* status lines kept */
#define STATUS_LEN		90

static struct TextAttr Topaz8 = { (STRPTR) "topaz.font", 8, 0, 0 };

/* ---------------------------------------------------------------------------
 * Gadget IDs
 * ------------------------------------------------------------------------- */
enum {
	GID_STATUS = 1, GID_LPATH, GID_RPATH, GID_LLIST, GID_RLIST, GID_QUEUE, GID_INFO,
	GID_CONNECT, GID_STOP, GID_DOWNLOAD, GID_UPLOAD, GID_DELETE, GID_LPARENT, GID_RPARENT,
	GID_REFRESHL, GID_REFRESHR, GID_RUNQ, GID_STEPQ, GID_CLEARQ, GID_SITES, GID_SETTINGS, GID_QUIT,

	/* address book */
	GID_AB_LIST = 100, GID_AB_NAME, GID_AB_HOST, GID_AB_PORT, GID_AB_USER, GID_AB_PASS,
	GID_AB_TYPE, GID_AB_NEW, GID_AB_APPLY, GID_AB_DELETE, GID_AB_CONNECT, GID_AB_SAVE, GID_AB_CLOSE,

	/* settings */
	GID_GS_TRANSFER = 200, GID_GS_CONNECT, GID_GS_QUEUE, GID_GS_PARTIAL, GID_GS_SAVE, GID_GS_CANCEL
};

/* Menu IDs (user data of the items) */
enum { MN_SITES = 1, MN_SETTINGS, MN_SAVECFG, MN_ABOUT, MN_QUIT };

static struct NewMenu Menus[] = {
	{ NM_TITLE, (STRPTR) "Project",			NULL, 0, 0, NULL },
	{ NM_ITEM,  (STRPTR) "Address book...",	(STRPTR) "A", 0, 0, (APTR) MN_SITES },
	{ NM_ITEM,  (STRPTR) "Settings...",		(STRPTR) "S", 0, 0, (APTR) MN_SETTINGS },
	{ NM_ITEM,  (STRPTR) "Save config",		(STRPTR) "W", 0, 0, (APTR) MN_SAVECFG },
	{ NM_ITEM,  NM_BARLABEL,				NULL, 0, 0, NULL },
	{ NM_ITEM,  (STRPTR) "About",			NULL, 0, 0, (APTR) MN_ABOUT },
	{ NM_ITEM,  (STRPTR) "Quit",			(STRPTR) "Q", 0, 0, (APTR) MN_QUIT },
	{ NM_END,   NULL, NULL, 0, 0, NULL }
};

static STRPTR ConnTypes[] = { (STRPTR) "Active", (STRPTR) "Passive", (STRPTR) "SFTP", (STRPTR) "FTPS",
							  (STRPTR) "FTPS impl.", NULL };
static STRPTR QueueTypes[] = { (STRPTR) "Stop at error", (STRPTR) "Process all", NULL };
static STRPTR PartialTypes[] = { (STRPTR) "Delete", (STRPTR) "Keep", NULL };

/* ---------------------------------------------------------------------------
 * Windows
 * ------------------------------------------------------------------------- */
typedef struct GtWin {
	struct Window *win;
	struct Gadget *glist;
} GtWin;

static struct Screen *g_pubscreen;		/* locked on 2.0+ only */
static APTR g_vi;
static struct Menu *g_menu;
static WORD g_scr_w, g_scr_h;

static GtWin g_main, g_ab, g_gs;

/* main window gadgets */
static struct Gadget *g_status_lv, *g_path[2], *g_list_lv[2], *g_queue_lv, *g_info;
static int g_list_cols;					/* characters per file line */
static int g_list_rows, g_status_rows;

/* data shown by the main window */
static LineList g_status, g_files[2], g_queue;
static char g_marks[2][MAX_VIEW_ENTRIES];
static int g_nmarks[2];
static int g_selected[2] = { -1, -1 };
static ULONG g_click_s[2], g_click_us[2];
static int g_click_idx[2] = { -1, -1 };
static int g_status_used;
static char g_info_text[100];
static char g_title[100];

/* address book */
static struct Gadget *g_ab_lv, *g_ab_str[5], *g_ab_type;
static struct List g_ab_list;
static struct Node g_ab_nodes[MAX_HOSTS];
static int g_ab_sel = -1, g_ab_conn_type;
static bool g_ab_new;
static ULONG g_ab_click_s, g_ab_click_us;
static int g_ab_click_idx = -1;

/* settings */
static struct Gadget *g_gs_transfer, *g_gs_connect;
static int g_gs_queue, g_gs_partial;

static void close_ab(void);
static void close_gs(void);

static void set_attrs(struct Gadget *gad, struct Window *win, Tag tag1, ...)
{
	if (gad && win)
		EF_GT_SetGadgetAttrsA(gad, win, NULL, (struct TagItem *) &tag1);
}

static char *string_of(struct Gadget *gad)
{
	return (char *) ((struct StringInfo *) gad->SpecialInfo)->Buffer;
}

static LONG integer_of(struct Gadget *gad)
{
	return ((struct StringInfo *) gad->SpecialInfo)->LongInt;
}

/* Opens a window on the Workbench screen with no gadgets, the caller adds
   them below win->BorderTop */
static struct Window *open_window(WORD left, WORD top, WORD w, WORD h, char *title, ULONG idcmp)
{
	struct NewWindow nw;

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
	return OpenWindow(&nw);
}

/* Adds a gadget list created with EF_CreateContext() to an open window */
static void attach_gadgets(GtWin *w)
{
	AddGList(w->win, w->glist, (UWORD) -1, -1, NULL);
	RefreshGList(w->glist, w->win, NULL, -1);
	EF_GT_RefreshWindow(w->win, NULL);
}

static void close_gtwin(GtWin *w)
{
	struct IntuiMessage *msg;

	if (w->win) {
		ClearMenuStrip(w->win);
		while ((msg = (struct IntuiMessage *) GetMsg(w->win->UserPort)))
			ReplyMsg((struct Message *) msg);
		CloseWindow(w->win);
	}
	if (w->glist)
		EF_FreeGadgets(w->glist);
	w->win = NULL;
	w->glist = NULL;
}

static struct Gadget *mk(ULONG kind, struct Gadget *prev, WORD x, WORD y, WORD w, WORD h,
						 char *label, ULONG flags, UWORD id, Tag tag1, ...)
{
	struct NewGadget ng;

	if (!prev)
		return NULL;
	ng.ng_LeftEdge = x;
	ng.ng_TopEdge = y;
	ng.ng_Width = w;
	ng.ng_Height = h;
	ng.ng_GadgetText = (UBYTE *) label;
	ng.ng_TextAttr = &Topaz8;
	ng.ng_GadgetID = id;
	ng.ng_Flags = flags;
	ng.ng_VisualInfo = g_vi;
	ng.ng_UserData = NULL;
	return EF_CreateGadgetA(kind, prev, &ng, (struct TagItem *) &tag1);
}

/* ---------------------------------------------------------------------------
 * Main window
 * ------------------------------------------------------------------------- */
static struct List *labels_of(LineList *ll)
{
	return &ll->list;
}

static void refresh_files(int which)
{
	set_attrs(g_list_lv[which], g_main.win, GTLV_Labels, (ULONG) labels_of(&g_files[which]), TAG_DONE);
}

static void detach_files(int which)
{
	set_attrs(g_list_lv[which], g_main.win, GTLV_Labels, (ULONG) ~0, TAG_DONE);
}

static void update_title(void)
{
	if (!g_main.win)
		return;
	if (core_is_connected())
		sprintf(g_title, "MarranoFTP13GD 1.0 - connected");
	else
		sprintf(g_title, "MarranoFTP13GD 1.0 - not connected");
	SetWindowTitles(g_main.win, (UBYTE *) g_title, (UBYTE *) -1);
}

static bool create_main_gadgets(void)
{
	struct Gadget *g;
	struct Window *win = g_main.win;
	WORD x0 = win->BorderLeft + 4, y = win->BorderTop + GAP;
	WORD iw = win->Width - win->BorderLeft - win->BorderRight - 8;
	WORD ih = win->Height - win->BorderTop - win->BorderBottom - 2 * GAP;
	WORD half = (iw - 4) / 2, fixed, list_h, bw;
	int i;
	static char *row1[] = { "Connect", "Stop", "Download", "Upload", "Delete", "Parent L", "Parent R" };
	static UWORD id1[] = { GID_CONNECT, GID_STOP, GID_DOWNLOAD, GID_UPLOAD, GID_DELETE, GID_LPARENT, GID_RPARENT };
	static char *row2[] = { "Refresh L", "Refresh R", "Run queue", "Step", "Clear queue", "Sites", "Quit" };
	static UWORD id2[] = { GID_REFRESHL, GID_REFRESHR, GID_RUNQ, GID_STEPQ, GID_CLEARQ, GID_SITES, GID_QUIT };

	/* status + paths + queue + info + 2 button rows, the rest is for the lists */
	fixed = (STATUS_ROWS * FONT_H + LV_EXTRA) + GAP + ROW_H + GAP + GAP
		  + (QUEUE_ROWS * FONT_H + LV_EXTRA) + GAP + ROW_H + GAP + ROW_H + GAP + ROW_H;
	list_h = ih - fixed;
	g_list_rows = (list_h - LV_EXTRA) / FONT_H;
	if (g_list_rows < 3)
		g_list_rows = 3;
	list_h = g_list_rows * FONT_H + LV_EXTRA;
	g_status_rows = STATUS_ROWS;
	g_list_cols = (half - SCROLLER_W - 8) / FONT_W;
	if (g_list_cols > 70)
		g_list_cols = 70;

	g = EF_CreateContext(&g_main.glist);

	g = g_status_lv = mk(LISTVIEW_KIND, g, x0, y, iw, STATUS_ROWS * FONT_H + LV_EXTRA, NULL, 0, GID_STATUS,
						 GTLV_Labels, (ULONG) &g_status.list, GTLV_ReadOnly, TRUE, TAG_DONE);
	y += STATUS_ROWS * FONT_H + LV_EXTRA + GAP;

	g = g_path[GUI_LOCAL] = mk(STRING_KIND, g, x0, y, half, ROW_H, NULL, 0, GID_LPATH,
							   GTST_String, (ULONG) core_local_path(), GTST_MaxChars, 500, TAG_DONE);
	g = g_path[GUI_REMOTE] = mk(STRING_KIND, g, x0 + half + 4, y, half, ROW_H, NULL, 0, GID_RPATH,
								GTST_String, (ULONG) "", GTST_MaxChars, 500, TAG_DONE);
	y += ROW_H + GAP;

	g = g_list_lv[GUI_LOCAL] = mk(LISTVIEW_KIND, g, x0, y, half, list_h, NULL, 0, GID_LLIST,
								  GTLV_Labels, (ULONG) &g_files[GUI_LOCAL].list, TAG_DONE);
	g = g_list_lv[GUI_REMOTE] = mk(LISTVIEW_KIND, g, x0 + half + 4, y, half, list_h, NULL, 0, GID_RLIST,
								   GTLV_Labels, (ULONG) &g_files[GUI_REMOTE].list, TAG_DONE);
	y += list_h + GAP;

	g = g_queue_lv = mk(LISTVIEW_KIND, g, x0, y, iw, QUEUE_ROWS * FONT_H + LV_EXTRA, NULL, 0, GID_QUEUE,
						GTLV_Labels, (ULONG) &g_queue.list, GTLV_ReadOnly, TRUE, TAG_DONE);
	y += QUEUE_ROWS * FONT_H + LV_EXTRA + GAP;

	g = g_info = mk(TEXT_KIND, g, x0, y, iw, ROW_H, NULL, 0, GID_INFO,
					GTTX_Text, (ULONG) g_info_text, GTTX_Border, TRUE, TAG_DONE);
	y += ROW_H + GAP;

	bw = (iw - 6 * 2) / 7;
	for (i = 0; i < 7; i++)
		g = mk(BUTTON_KIND, g, x0 + i * (bw + 2), y, bw, ROW_H, row1[i], 0, id1[i], TAG_DONE);
	y += ROW_H + GAP;
	for (i = 0; i < 7; i++)
		g = mk(BUTTON_KIND, g, x0 + i * (bw + 2), y, bw, ROW_H, row2[i], 0, id2[i], TAG_DONE);

	return g != NULL;
}

/* ---------------------------------------------------------------------------
 * Address book window
 * ------------------------------------------------------------------------- */
static void ab_build_list(void)
{
	int i, n = core_host_count();

	g_ab_list.lh_Head = (struct Node *) &g_ab_list.lh_Tail;
	g_ab_list.lh_Tail = NULL;
	g_ab_list.lh_TailPred = (struct Node *) &g_ab_list.lh_Head;
	for (i = 0; i < n && i < MAX_HOSTS; i++) {
		g_ab_nodes[i].ln_Name = core_host_get(i)->entryname;
		AddTail(&g_ab_list, &g_ab_nodes[i]);
	}
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
	set_attrs(g_ab_str[0], win, GTST_String, (ULONG) h->entryname, TAG_DONE);
	set_attrs(g_ab_str[1], win, GTST_String, (ULONG) h->hostname, TAG_DONE);
	set_attrs(g_ab_str[2], win, GTST_String, (ULONG) h->port, TAG_DONE);
	set_attrs(g_ab_str[3], win, GTST_String, (ULONG) h->username, TAG_DONE);
	set_attrs(g_ab_str[4], win, GTST_String, (ULONG) h->password, TAG_DONE);
	g_ab_conn_type = h->conn_type >= CTYPE_ACTV && h->conn_type <= CTYPE_FTPS_IMPLICIT ? h->conn_type : CTYPE_ACTV;
	set_attrs(g_ab_type, win, GTCY_Active, g_ab_conn_type, TAG_DONE);
}

static void ab_refresh(int sel)
{
	if (!g_ab.win)
		return;
	set_attrs(g_ab_lv, g_ab.win, GTLV_Labels, (ULONG) ~0, TAG_DONE);
	ab_build_list();
	if (sel >= core_host_count())
		sel = core_host_count() - 1;
	g_ab_sel = sel;
	g_ab_new = false;
	set_attrs(g_ab_lv, g_ab.win, GTLV_Labels, (ULONG) &g_ab_list,
			  GTLV_Selected, sel >= 0 ? sel : ~0, TAG_DONE);
	ab_show(sel);
}

static void open_ab(void)
{
	struct Gadget *g;
	WORD w = 600, h, x0, y, lw = 220, fx, fw, bw;
	static char *labels[] = { "Name", "Host", "Port", "User", "Password" };
	static UWORD ids[] = { GID_AB_NAME, GID_AB_HOST, GID_AB_PORT, GID_AB_USER, GID_AB_PASS };
	static UWORD maxc[] = { ENTRYNAME_CONFIG_LEN - 1, HOSTNAME_CONFIG_LEN - 1, PORT_COFING_LEN - 1,
							USERNAME_CONFIG_LEN - 1, PASSWORD_CONFIG_LEN - 1 };
	static char *blabels[] = { "New", "Apply", "Delete", "Connect", "Save cfg", "Close" };
	static UWORD bids[] = { GID_AB_NEW, GID_AB_APPLY, GID_AB_DELETE, GID_AB_CONNECT, GID_AB_SAVE, GID_AB_CLOSE };
	int i;

	if (g_ab.win) {
		WindowToFront(g_ab.win);
		ActivateWindow(g_ab.win);
		return;
	}
	if (w > g_scr_w)
		w = g_scr_w;
	h = 6 * (ROW_H + GAP) + ROW_H + 2 * GAP + 26;
	if (h > g_scr_h - 12)
		h = g_scr_h - 12;
	g_ab.win = open_window((g_scr_w - w) / 2, 12, w, h, "MarranoFTP13GD address book",
						   CLOSEWINDOW | REFRESHWINDOW | GADGETUP | GADGETDOWN | MOUSEBUTTONS | INTUITICKS | MOUSEMOVE);
	if (!g_ab.win) {
		gui_status("ERROR: Cannot open the address book window");
		return;
	}
	x0 = g_ab.win->BorderLeft + 4;
	y = g_ab.win->BorderTop + GAP;
	fx = x0 + lw + 8 + 9 * FONT_W;
	fw = w - g_ab.win->BorderRight - 4 - fx;

	ab_build_list();
	g = EF_CreateContext(&g_ab.glist);
	g = g_ab_lv = mk(LISTVIEW_KIND, g, x0, y, lw, ((h - g_ab.win->BorderTop - g_ab.win->BorderBottom - ROW_H - 3 * GAP - LV_EXTRA) / FONT_H) * FONT_H + LV_EXTRA,
					 NULL, 0, GID_AB_LIST, GTLV_Labels, (ULONG) &g_ab_list, TAG_DONE);
	for (i = 0; i < 5; i++)
		g = g_ab_str[i] = mk(STRING_KIND, g, fx, y + i * (ROW_H + GAP), fw, ROW_H, labels[i], PLACETEXT_LEFT, ids[i],
							 GTST_MaxChars, maxc[i], TAG_DONE);
	g = g_ab_type = mk(CYCLE_KIND, g, fx, y + 5 * (ROW_H + GAP), 120, ROW_H, "Mode", PLACETEXT_LEFT, GID_AB_TYPE,
					   GTCY_Labels, (ULONG) ConnTypes, TAG_DONE);
	y = h - g_ab.win->BorderBottom - GAP - ROW_H;
	bw = (w - g_ab.win->BorderLeft - g_ab.win->BorderRight - 8 - 5 * 4) / 6;
	for (i = 0; i < 6; i++)
		g = mk(BUTTON_KIND, g, x0 + i * (bw + 4), y, bw, ROW_H, blabels[i], 0, bids[i], TAG_DONE);
	if (!g) {
		close_ab();
		gui_status("ERROR: Cannot create the address book gadgets");
		return;
	}
	attach_gadgets(&g_ab);
	ab_refresh(core_host_count() ? 0 : -1);
}

static void close_ab(void)
{
	close_gtwin(&g_ab);
}

static void ab_apply(void)
{
	Host h;

	caf_memset(&h, 0, sizeof(h));
	caf_strncpy(h.entryname, string_of(g_ab_str[0]), ENTRYNAME_CONFIG_LEN);
	caf_strncpy(h.hostname, string_of(g_ab_str[1]), HOSTNAME_CONFIG_LEN);
	caf_strncpy(h.port, string_of(g_ab_str[2]), PORT_COFING_LEN);
	caf_strncpy(h.username, string_of(g_ab_str[3]), USERNAME_CONFIG_LEN);
	caf_strncpy(h.password, string_of(g_ab_str[4]), PASSWORD_CONFIG_LEN);
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

static void ab_event(ULONG class, UWORD code, struct Gadget *gad, ULONG secs, ULONG micros)
{
	if (class == CLOSEWINDOW) {
		close_ab();
		return;
	}
	if (class != GADGETUP)
		return;
	switch (gad->GadgetID) {
		case GID_AB_LIST:
			if (code == g_ab_click_idx && DoubleClick(g_ab_click_s, g_ab_click_us, secs, micros)) {
				g_ab_click_idx = -1;
				ab_connect(code);
				return;
			}
			g_ab_click_idx = code;
			g_ab_click_s = secs;
			g_ab_click_us = micros;
			g_ab_sel = code;
			g_ab_new = false;
			ab_show(code);
			break;
		case GID_AB_TYPE:	g_ab_conn_type = code; break;
		case GID_AB_NEW:
			if (core_host_count() >= MAX_HOSTS) {
				gui_status("STATUS: the address book is full");
				break;
			}
			g_ab_new = true;
			set_attrs(g_ab_lv, g_ab.win, GTLV_Selected, ~0, TAG_DONE);
			ab_show(-1);
			ActivateGadget(g_ab_str[0], g_ab.win, NULL);
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
static void open_gs(void)
{
	struct Gadget *g;
	WORD w = 360, h, x0, y, fx;

	if (g_gs.win) {
		WindowToFront(g_gs.win);
		ActivateWindow(g_gs.win);
		return;
	}
	h = 5 * (ROW_H + GAP) + 2 * GAP + 14;
	g_gs.win = open_window((g_scr_w - w) / 2, 24, w, h, "MarranoFTP13GD settings",
						   CLOSEWINDOW | REFRESHWINDOW | GADGETUP | GADGETDOWN | MOUSEBUTTONS);
	if (!g_gs.win) {
		gui_status("ERROR: Cannot open the settings window");
		return;
	}
	x0 = g_gs.win->BorderLeft + 4;
	y = g_gs.win->BorderTop + GAP;
	fx = x0 + 22 * FONT_W;
	g_gs_queue = core_get_setting(SETTING_QUEUE_TYPE) ? 1 : 0;
	g_gs_partial = core_get_setting(SETTING_KEEP_PARTIAL) ? 1 : 0;

	g = EF_CreateContext(&g_gs.glist);
	g = g_gs_transfer = mk(INTEGER_KIND, g, fx, y, 80, ROW_H, "Transfer timeout (s)", PLACETEXT_LEFT, GID_GS_TRANSFER,
						   GTIN_Number, core_get_setting(SETTING_TRANSFER_TIMEOUT), GTIN_MaxChars, 4, TAG_DONE);
	y += ROW_H + GAP;
	g = g_gs_connect = mk(INTEGER_KIND, g, fx, y, 80, ROW_H, "Connect timeout (s)", PLACETEXT_LEFT, GID_GS_CONNECT,
						  GTIN_Number, core_get_setting(SETTING_CONNECT_TIMEOUT), GTIN_MaxChars, 4, TAG_DONE);
	y += ROW_H + GAP;
	g = mk(CYCLE_KIND, g, fx, y, 150, ROW_H, "Queue", PLACETEXT_LEFT, GID_GS_QUEUE,
		   GTCY_Labels, (ULONG) QueueTypes, GTCY_Active, g_gs_queue, TAG_DONE);
	y += ROW_H + GAP;
	g = mk(CYCLE_KIND, g, fx, y, 150, ROW_H, "Partial downloads", PLACETEXT_LEFT, GID_GS_PARTIAL,
		   GTCY_Labels, (ULONG) PartialTypes, GTCY_Active, g_gs_partial, TAG_DONE);
	y += ROW_H + GAP + GAP;
	g = mk(BUTTON_KIND, g, x0, y, 100, ROW_H, "Use", 0, GID_GS_SAVE, TAG_DONE);
	g = mk(BUTTON_KIND, g, w - g_gs.win->BorderRight - 4 - 100, y, 100, ROW_H, "Cancel", 0, GID_GS_CANCEL, TAG_DONE);
	if (!g) {
		close_gs();
		gui_status("ERROR: Cannot create the settings gadgets");
		return;
	}
	attach_gadgets(&g_gs);
}

static void close_gs(void)
{
	close_gtwin(&g_gs);
}

static void gs_event(ULONG class, UWORD code, struct Gadget *gad)
{
	if (class == CLOSEWINDOW) {
		close_gs();
		return;
	}
	if (class != GADGETUP)
		return;
	switch (gad->GadgetID) {
		case GID_GS_QUEUE:		g_gs_queue = code; break;
		case GID_GS_PARTIAL:	g_gs_partial = code; break;
		case GID_GS_SAVE:
			core_set_setting(SETTING_TRANSFER_TIMEOUT, integer_of(g_gs_transfer));
			core_set_setting(SETTING_CONNECT_TIMEOUT, integer_of(g_gs_connect));
			core_set_setting(SETTING_QUEUE_TYPE, g_gs_queue);
			core_set_setting(SETTING_KEEP_PARTIAL, g_gs_partial);
			close_gs();
			break;
		case GID_GS_CANCEL:		close_gs(); break;
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

static void clear_marks(int which)
{
	int i;

	if (!g_nmarks[which])
		return;
	detach_files(which);
	for (i = 0; i < g_files[which].count; i++)
		set_mark(which, i, false);
	refresh_files(which);
}

static void list_click(int which, UWORD code, ULONG secs, ULONG micros)
{
	int i = code;

	if (i == g_click_idx[which] && DoubleClick(g_click_s[which], g_click_us[which], secs, micros)) {
		/* the first click of the double click toggled the mark: undo it */
		detach_files(which);
		set_mark(which, i, !g_marks[which][i]);
		refresh_files(which);
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
	if (!gl_is_parent(&g_files[which], i)) {
		detach_files(which);
		set_mark(which, i, !g_marks[which][i]);
		refresh_files(which);
	}
}

/* ---------------------------------------------------------------------------
 * gui.h implementation
 * ------------------------------------------------------------------------- */
void gui_status(const char *text)
{
	struct Node *n;
	int top;

	if (!g_status.nodes)
		return;
	if (g_main.win)
		set_attrs(g_status_lv, g_main.win, GTLV_Labels, (ULONG) ~0, TAG_DONE);
	if (g_status_used < STATUS_MAX)
		n = &g_status.nodes[g_status_used++];
	else
		n = RemHead(&g_status.list);		/* recycle the oldest line */
	caf_strncpy(n->ln_Name, (char *) text, STATUS_LEN);
	AddTail(&g_status.list, n);
	top = g_status_used - g_status_rows;
	if (top < 0)
		top = 0;
	if (g_main.win)
		set_attrs(g_status_lv, g_main.win, GTLV_Labels, (ULONG) &g_status.list, GTLV_Top, top, TAG_DONE);
}

void gui_transfer_info(const char *text)
{
	caf_strncpy(g_info_text, (char *) text, sizeof(g_info_text) - 1);
	set_attrs(g_info, g_main.win, GTTX_Text, (ULONG) g_info_text, TAG_DONE);
}

void gui_set_connected(bool connected)
{
	update_title();
	if (!connected) {
		detach_files(GUI_REMOTE);
		ll_free(&g_files[GUI_REMOTE]);
		g_nmarks[GUI_REMOTE] = 0;
		g_selected[GUI_REMOTE] = -1;
		refresh_files(GUI_REMOTE);
	}
}

void gui_set_path(int which, const char *path)
{
	set_attrs(g_path[which], g_main.win, GTST_String, (ULONG) path, TAG_DONE);
}

void gui_set_list(int which, ViewColumn **entries, int count)
{
	detach_files(which);
	caf_memset(g_marks[which], 0, sizeof(g_marks[which]));
	g_nmarks[which] = 0;
	g_selected[which] = -1;
	g_click_idx[which] = -1;
	if (!gl_format_files(&g_files[which], entries, count, g_list_cols))
		gui_status("ERROR: not enough memory for the file list");
	refresh_files(which);
	set_attrs(g_list_lv[which], g_main.win, GTLV_Top, 0, TAG_DONE);
}

int gui_get_selection(int which, int *idx, int max)
{
	int i, n = 0;

	for (i = 0; i < g_files[which].count && n < max; i++)
		if (g_marks[which][i])
			idx[n++] = i;
	if (!n && g_selected[which] >= 0 && max > 0)
		idx[n++] = g_selected[which];
	clear_marks(which);
	return n;
}

void gui_queue_changed(void)
{
	int w = g_list_cols * 2;

	if (w > 78)
		w = 78;
	set_attrs(g_queue_lv, g_main.win, GTLV_Labels, (ULONG) ~0, TAG_DONE);
	if (!gl_format_queue(&g_queue, w))
		gui_status("ERROR: not enough memory for the queue list");
	set_attrs(g_queue_lv, g_main.win, GTLV_Labels, (ULONG) &g_queue.list, TAG_DONE);
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
	gui_status("MarranoFTP13GD 1.0 - (C) Stefano Crosara aka Suppah");
	gui_status("GadTools emulation: GTE.lib (C) Massimo Tantignone");
}

/* Returns FALSE to quit at once */
static bool main_event(ULONG class, UWORD code, struct Gadget *gad, ULONG secs, ULONG micros)
{
	switch (class) {
		case CLOSEWINDOW:
			return false;

		case MENUPICK:
			while (code != MENUNULL) {
				struct MenuItem *item = ItemAddress(g_menu, code);

				if (!item)
					break;
				switch ((ULONG) GTMENUITEM_USERDATA(item)) {
					case MN_SITES:		open_ab(); break;
					case MN_SETTINGS:	open_gs(); break;
					case MN_SAVECFG:
						gui_status(core_save_config() ? "STATUS: Configuration saved" : "STATUS: Error while saving configuration");
						break;
					case MN_ABOUT:		about(); break;
					case MN_QUIT:		core_request_quit(); break;
				}
				if (!g_main.win)
					break;
				code = item->NextSelect;
			}
			break;

		case GADGETUP:
			switch (gad->GadgetID) {
				case GID_LPATH:		core_set_local_path(string_of(g_path[GUI_LOCAL])); break;
				case GID_RPATH:		core_set_remote_path(string_of(g_path[GUI_REMOTE])); break;
				case GID_LLIST:		list_click(GUI_LOCAL, code, secs, micros); break;
				case GID_RLIST:		list_click(GUI_REMOTE, code, secs, micros); break;
				case GID_CONNECT:	core_connect_toggle(); break;
				case GID_STOP:		core_stop(); break;
				case GID_DOWNLOAD:	core_download_selected(); break;
				case GID_UPLOAD:	core_upload_selected(); break;
				case GID_DELETE:	core_delete_selected(); break;
				case GID_LPARENT:	core_local_parent(); break;
				case GID_RPARENT:	core_remote_parent(); break;
				case GID_REFRESHL:	core_set_local_path(string_of(g_path[GUI_LOCAL])); break;
				case GID_REFRESHR:	core_remote_refresh(); break;
				case GID_RUNQ:		core_queue_run(); break;
				case GID_STEPQ:		core_queue_step(); break;
				case GID_CLEARQ:	core_queue_clear(); break;
				case GID_SITES:		open_ab(); break;
				case GID_SETTINGS:	open_gs(); break;
				case GID_QUIT:		core_request_quit(); break;
			}
			break;
	}
	return true;
}

/* Gets the next message of one window, copies what is needed and replies.
   Returns FALSE when there are no more messages. */
static bool next_msg(GtWin *w, ULONG *class, UWORD *code, struct Gadget **gad, ULONG *secs, ULONG *micros)
{
	struct IntuiMessage *msg;

	for (;;) {
		if (!w->win || !(msg = EF_GT_GetIMsg(w->win->UserPort)))
			return false;
		*class = msg->Class;
		*code = msg->Code;
		*gad = (struct Gadget *) msg->IAddress;
		*secs = msg->Seconds;
		*micros = msg->Micros;
		EF_GT_ReplyIMsg(msg);
		if (*class == REFRESHWINDOW) {
			EF_GT_BeginRefresh(w->win);
			EF_GT_EndRefresh(w->win, TRUE);
			continue;
		}
		return true;
	}
}

bool gui_handle(ULONG sigs)
{
	ULONG class, secs, micros;
	UWORD code;
	struct Gadget *gad;

	(void) sigs;
	while (next_msg(&g_main, &class, &code, &gad, &secs, &micros))
		if (!main_event(class, code, gad, secs, micros))
			return false;
	while (next_msg(&g_ab, &class, &code, &gad, &secs, &micros))
		ab_event(class, code, gad, secs, micros);
	while (next_msg(&g_gs, &class, &code, &gad, &secs, &micros))
		gs_event(class, code, gad);
	return true;
}

/* ---------------------------------------------------------------------------
 * Open / close
 * ------------------------------------------------------------------------- */
bool gui_open(void)
{
	struct Screen scr;
	int i;

	IntuitionBase = (struct IntuitionBase *) OpenLibrary((UBYTE *) "intuition.library", 33);
	GfxBase = (struct GfxBase *) OpenLibrary((UBYTE *) "graphics.library", 33);
	if (!IntuitionBase || !GfxBase) {
		printf("MarranoFTP13GD needs Kickstart 1.2 or newer\n");
		return false;
	}

	ll_init(&g_status);
	ll_init(&g_files[0]);
	ll_init(&g_files[1]);
	ll_init(&g_queue);
	if (!ll_alloc(&g_status, STATUS_MAX, STATUS_LEN)) {
		printf("Not enough memory\n");
		return false;
	}
	/* status lines are added one by one, start from an empty list */
	ll_init(&g_status);
	g_status_used = 0;
	for (i = 0; i < STATUS_MAX; i++)
		g_status.nodes[i].ln_Name = g_status.text + i * (STATUS_LEN + 1);

	if (!GetScreenData((APTR) &scr, sizeof(struct Screen), WBENCHSCREEN, NULL)) {
		printf("Cannot read the Workbench screen data\n");
		return false;
	}
	g_scr_w = scr.Width;
	g_scr_h = scr.Height;

	/* gadtools 34 emulation: NULL means the Workbench screen on 1.3, on 2.0+
	   pass the (locked) default public screen */
	if (((struct Library *) SysBase)->lib_Version >= 36)
		g_pubscreen = LockPubScreen(NULL);
	g_vi = EF_GetVisualInfoA(g_pubscreen, NULL);
	if (!g_vi) {
		printf("Cannot get the visual info\n");
		return false;
	}

	g_main.win = open_window(0, scr.BarHeight + 1, g_scr_w, g_scr_h - scr.BarHeight - 1, "MarranoFTP13GD",
							 CLOSEWINDOW | REFRESHWINDOW | GADGETUP | GADGETDOWN | MOUSEBUTTONS | MENUPICK
							 | INTUITICKS | MOUSEMOVE);
	if (!g_main.win) {
		printf("Cannot open the main window\n");
		return false;
	}
	if (!create_main_gadgets()) {
		printf("Cannot create the gadgets\n");
		return false;
	}
	attach_gadgets(&g_main);

	g_menu = EF_CreateMenusA(Menus, NULL);
	if (g_menu && EF_LayoutMenusA(g_menu, g_vi, NULL))
		SetMenuStrip(g_main.win, g_menu);
	update_title();
	return true;
}

void gui_close(void)
{
	close_ab();
	close_gs();
	close_gtwin(&g_main);
	if (g_menu)
		EF_FreeMenus(g_menu);
	g_menu = NULL;
	if (g_vi)
		EF_FreeVisualInfo(g_vi);
	g_vi = NULL;
	if (g_pubscreen)
		UnlockPubScreen(NULL, g_pubscreen);
	g_pubscreen = NULL;

	/* the status nodes were allocated by ll_alloc() */
	if (g_status.text) {
		FreeMem(g_status.nodes, g_status.nodes_size);
		FreeMem(g_status.text, g_status.text_size);
		g_status.nodes = NULL;
		g_status.text = NULL;
	}
	ll_free(&g_files[0]);
	ll_free(&g_files[1]);
	ll_free(&g_queue);

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
