

#ifndef HEADERS_NL_GADTOOLS_H
#define HEADERS_NL_GADTOOLS_H


/*  New.lib/gadtools.h  $ 27/01/93 MT $  */
/*                                       */
/*  File header per l'uso di GTE.lib.    */
/*  Per usare queste funzioni devono     */
/*  essere aperte la graphics.library,   */
/*  la intuition.library e (solo sotto   */
/*  2.x+) anche la gadtools.library.     */


#include "exec/types.h"
#include "intuition/intuition.h"
#include "libraries/gadtools.h"
#include "utility/tagitem.h"


/* Sinonimi per alcune funzioni:          */
/*                                        */
/* usare i nomi originali se si predilige */
/* la coerenza, oppure i sinonimi se      */
/* si predilige la velocità di battitura  */

#define NL_GetIMsg         NL_GT_GetIMsg
#define NL_ReplyIMsg       NL_GT_ReplyIMsg
#define NL_FilterIMsg      NL_GT_FilterIMsg
#define NL_PostFilterIMsg  NL_GT_PostFilterIMsg
#define NL_RefreshWindow   NL_GT_RefreshWindow
#define NL_BeginRefresh    NL_GT_BeginRefresh
#define NL_EndRefresh      NL_GT_EndRefresh
#define NL_SetGadgetAttrsA NL_GT_SetGadgetAttrsA
#define NL_SetGadgetAttrs  NL_GT_SetGadgetAttrs
#define EF_GetIMsg         EF_GT_GetIMsg
#define EF_ReplyIMsg       EF_GT_ReplyIMsg
#define EF_FilterIMsg      EF_GT_FilterIMsg
#define EF_PostFilterIMsg  EF_GT_PostFilterIMsg
#define EF_RefreshWindow   EF_GT_RefreshWindow
#define EF_BeginRefresh    EF_GT_BeginRefresh
#define EF_EndRefresh      EF_GT_EndRefresh
#define EF_SetGadgetAttrsA EF_GT_SetGadgetAttrsA
#define EF_SetGadgetAttrs  EF_GT_SetGadgetAttrs


/* ---------------------- Funzioni di interfaccia ---------------------- */

/* Funzioni relative ai gadget */

struct Gadget *NL_CreateContext(struct Gadget **glistptr);
struct Gadget *NL_CreateGadgetA(ULONG kind, struct Gadget *gad, struct NewGadget *ng, struct TagItem *taglist);
struct Gadget *NL_CreateGadget(ULONG kind, struct Gadget *gad, struct NewGadget *ng, Tag tag1, ...);
void NL_FreeGadgets(struct Gadget *gad);
void NL_GT_SetGadgetAttrsA(struct Gadget *gad, struct Window *win, struct Requester *req, struct TagItem *taglist);
void NL_GT_SetGadgetAttrs(struct Gadget *gad, struct Window *win, struct Requester *req, Tag tag1, ...);

/* Funzioni relative ai menu */

struct Menu *NL_CreateMenusA(struct NewMenu *newmenulist, struct TagItem *taglist);
struct Menu *NL_CreateMenus(struct NewMenu *newmenulist, Tag tag1, ...);
void NL_FreeMenus(struct Menu *menustrip);
BOOL NL_LayoutMenuItemsA(struct MenuItem *firstitem, APTR vinfo, struct TagItem *taglist);
BOOL NL_LayoutMenuItems(struct MenuItem *firstitem, APTR vinfo, Tag tag1, ...);
BOOL NL_LayoutMenusA(struct Menu *firstmenu, APTR vinfo, struct TagItem *taglist);
BOOL NL_LayoutMenus(struct Menu *firstmenu, APTR vinfo, Tag tag1, ...);

/* Funzioni di uso generale */

struct IntuiMessage *NL_GT_GetIMsg(struct MsgPort *userport);
void NL_GT_ReplyIMsg(struct IntuiMessage *imsg);
struct IntuiMessage *NL_GT_FilterIMsg(struct IntuiMessage *imsg);
struct IntuiMessage *NL_GT_PostFilterIMsg(struct IntuiMessage *imsg);
APTR NL_GetVisualInfoA(struct Screen *screen, struct TagItem *taglist);
APTR NL_GetVisualInfo(struct Screen *screen, Tag tag1, ...);
void NL_FreeVisualInfo(APTR vinfo);
void NL_GT_BeginRefresh(struct Window *win);
void NL_GT_EndRefresh(struct Window *win, LONG complete);
void NL_GT_RefreshWindow(struct Window *win, struct Requester *req);
void NL_DrawBevelBoxA(struct RastPort *rport, long left, long top, long width, long height, struct TagItem *taglist);
void NL_DrawBevelBox(struct RastPort *rport, long left, long top, long width, long height, Tag tag1, ...);

/* --------------------------------------------------------------------- */


/* ---------------------- Funzioni di emulazione ----------------------- */

/* Funzioni relative ai gadget */

struct Gadget *EF_CreateContext(struct Gadget **glistptr);
struct Gadget *EF_CreateGadgetA(ULONG kind, struct Gadget *gad, struct NewGadget *ng, struct TagItem *taglist);
struct Gadget *EF_CreateGadget(ULONG kind, struct Gadget *gad, struct NewGadget *ng, Tag tag1, ...);
void EF_FreeGadgets(struct Gadget *gad);
void EF_GT_SetGadgetAttrsA(struct Gadget *gad, struct Window *win, struct Requester *req, struct TagItem *taglist);
void EF_GT_SetGadgetAttrs(struct Gadget *gad, struct Window *win, struct Requester *req, Tag tag1, ...);

/* Funzioni relative ai menu */

struct Menu *EF_CreateMenusA(struct NewMenu *newmenulist, struct TagItem *taglist);
struct Menu *EF_CreateMenus(struct NewMenu *newmenulist, Tag tag1, ...);
void EF_FreeMenus(struct Menu *menustrip);
BOOL EF_LayoutMenuItemsA(struct MenuItem *firstitem, APTR vinfo, struct TagItem *taglist);
BOOL EF_LayoutMenuItems(struct MenuItem *firstitem, APTR vinfo, Tag tag1, ...);
BOOL EF_LayoutMenusA(struct Menu *firstmenu, APTR vinfo, struct TagItem *taglist);
BOOL EF_LayoutMenus(struct Menu *firstmenu, APTR vinfo, Tag tag1, ...);

/* Funzioni di uso generale */

struct IntuiMessage *EF_GT_GetIMsg(struct MsgPort *userport);
void EF_GT_ReplyIMsg(struct IntuiMessage *imsg);
struct IntuiMessage *EF_GT_FilterIMsg(struct IntuiMessage *imsg);
struct IntuiMessage *EF_GT_PostFilterIMsg(struct IntuiMessage *imsg);
APTR EF_GetVisualInfoA(struct Screen *screen, struct TagItem *taglist);
APTR EF_GetVisualInfo(struct Screen *screen, Tag tag1, ...);
void EF_FreeVisualInfo(APTR vinfo);
void EF_GT_BeginRefresh(struct Window *win);
void EF_GT_EndRefresh(struct Window *win, LONG complete);
void EF_GT_RefreshWindow(struct Window *win, struct Requester *req);
void EF_DrawBevelBoxA(struct RastPort *rport, long left, long top, long width, long height, struct TagItem *taglist);
void EF_DrawBevelBox(struct RastPort *rport, long left, long top, long width, long height, Tag tag1, ...);

/* --------------------------------------------------------------------- */


#endif


