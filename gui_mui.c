/*
 * GUI_MUI.C - MarranoFTP user interface for MUI (AmigaOS 2.04+, AROS,
 * MorphOS). Everything about FTP lives in ftp_core.c: this file only builds
 * the windows, forwards the user actions to the core_* functions and
 * implements the gui_* functions of gui.h.
 */

#ifdef __SASC
/* AmiTCP SDK 4.3 + NDK 3.2 TimerBase clash, see ftp_core.c */
#define PROTO_TIMER_H 1
#endif

#include <exec/types.h>
#include <exec/exec.h>
#include <dos/dos.h>
#include <libraries/mui.h>
#include <libraries/gadtools.h>
#include <utility/hooks.h>
#include <stdio.h>
#include <string.h>

#ifdef __AROS__
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#endif

#ifdef __MORPHOS__
#include <ppcinline/exec.h>
#include <ppcinline/dos.h>
#include <ppcinline/intuition.h>
#include <ppcinline/muimaster.h>
#include <clib/alib_protos.h>
#endif

#ifdef __SASC
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <clib/alib_protos.h>
#endif

#include <MUI/NList_mcc.h>
#include <MUI/NListview_mcc.h>

#include <caf/caf_types.h>
#include <caf/caf_debugging.h>
#include <caf/thread_helpers.h>
#include <caf/caf_platform.h>

#include "core.h"
#include "gui.h"

#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) ((ULONG) (a)<<24 | (ULONG) (b)<<16 | (ULONG) (c)<<8 | (ULONG) (d))
#endif

/* MUI objects of the main window. The names are the ones of the original
   Connection structure, so the window definition below is unchanged. */
typedef struct MuiList { APTR ListView; } MuiList;
typedef struct MuiGui {
	APTR Window;
	MuiList lv, rv;
	APTR QueueLV;
	APTR LV_STATUS, S_LEFT_VIEW_PATH, S_RIGHT_VIEW_PATH,
		 BTN_DOWNLOAD, BTN_UPLOAD, BTN_DELETE, BTN_CLEAR_QUEUE, BTN_RUN_QUEUE,
		 BTN_STEP_QUEUE, BTN_STOP, BTN_RENAME, BTN_MAKEDIR, BTN_CONNDISC,
		 BTN_REFRESH_L, BTN_REFRESH_R, BTN_QUIT, BTN_DUMP, S_TRANSFER_INFO;
} MuiGui;

/* Address book and global settings windows */
static struct {
	APTR Window, Window_HostsFull, BTN_HostsFullOk, HostsLV, LV_HOSTS,
		 S_ENTRYNAME, S_HOSTNAME, S_PORT, RDIO_CONN_TYPE, S_USERNAME, S_PASSWORD,
		 BTN_NEW, BTN_CHANGE, BTN_SAVE, BTN_DELETE, BTN_EXIT;
	bool b_new_entry;
	int selected_host;
} gab;

static struct {
	APTR Window, S_WAITSELECT_SOCKET_TIMEOUT, S_TRANSFER_SOCKET_TIMEOUT,
		 S_CONNECTION_SOCKET_TIMEOUT, RDIO_QUEUE_TYPE, RDIO_DELETE_PARTIAL,
		 BTN_SAVE, BTN_CANCEL;
} ggs;

static MuiGui g_gui;
static ULONG g_mui_sigs;
static QueueColumn g_queue_cols[QUEUE_SIZE + 1];
static bool g_libs_open;

enum 
{
	/* Menu Ids */
	MNID_OPENCONFIG = 1,
	MNID_SAVECONFIG,
	MNID_EXPORTCONFIG,
	MNID_IMPORTCONFIG,
	MNID_ABOUT,
	MNID_QUIT,
	MNID_GLOBALSETTINGS,
	MNID_OPENADDRESSBOOK,
	
	/* Main Window */
	ID_CLICKED_CONNDISC, 
	ID_CLICKED_REFRESH_L, ID_CLICKED_REFRESH_R, 
	ID_CLICKED_STOP, ID_CLICKED_UPLOAD, 
	ID_CLICKED_DELETE, ID_CLICKED_RENAME, ID_CLICKED_MAKEDIR, 
	ID_CLICKED_QUEUE_PROCESS, ID_CLICKED_QUEUE_STEP, ID_CLICKED_DOWNLOAD,
	ID_CLICKED_CLEAR_QUEUE,
	ID_MODIFIED_LPATHBOX, ID_DBLCLICKED_LEFT_LISTVIEW,
	ID_MODIFIED_RPATHBOX, ID_DBLCLICKED_RIGHT_LISTVIEW,
	ID_CLICKED_QUIT, ID_CLICKED_DUMP,
	
	/* Address Book Window */
	ID_SITE_WINDOW_FULL_HOSTS_WINDOW_OK,
	ID_SITE_WINDOW_CLICKED_NEW,
	ID_SITE_WINDOW_CLICKED_CHANGE,
	ID_SITE_WINDOW_CLICKED_DELETE,
	ID_SITE_WINDOW_CLICKED_SAVE,
	ID_SITE_WINDOW_CLICKED_EXIT,
	ID_DBLCLICKED_SITE_WINDOW_HOSTS,
	ID_CHANGESEL_SITE_WINDOW_HOSTS,
	
	/* Global Settings Window */
	ID_GLOBAL_SETTINGS_WINDOW_SAVE,
	ID_GLOBAL_SETTINGS_WINDOW_CANCEL
};

/* Statics */
static const char *g_AddressBook_ConnectionTypes[] =
{
	"Active",
	"Passive",
	"SFTP",
	"FTPS",
	"FTPS implicit (990)",
	NULL
};

static const char *g_GlobalSettings_QueueTypes[] = 
{
	"Stop at error",
	"Process All",
	NULL
};

static const char *g_GlobalSettings_DeletePartial[] = 
{
	"Delete",
	"Keep",
	NULL
};

/* Globals */

#ifdef __MORPHOS__
struct Library *MUIMasterBase;
struct IntuitionBase *IntuitionBase;
struct DOSBase *DOSBase;
struct SysBase *SysBase;
#endif

#ifdef __AROS__
extern struct Library *MUIMasterBase;
extern struct IntuitionBase *IntuitionBase;
#endif

#ifdef __SASC
/*
 * Defined here on purpose: if they were only "extern", the SAS/C linker would
 * pull the sc.lib auto-open stubs for them, and the program would close them
 * a second time on exit.
 * SysBase and DOSBase belong to the SAS/C startup code (c.o): they must never
 * be opened, closed or zeroed by the program.
 */
struct Library *MUIMasterBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;

/*
 * Minimum stack: the shell default (4KB) is not enough, the program has lots
 * of 512 bytes buffers on the stack and MUI wants a lot of stack as well.
 * c.o swaps to a bigger stack at startup if needed.
 */
// __near: c.o reads __stack with A4 relative addressing, with DATA=FAR
// a plain global would end in the far section (slink Error 510)
long __near __stack = 32768;
#endif

Object	*app;

#define Title(t)		{ NM_TITLE, t, NULL, 0, 0, NULL }
#define Item(t,s,i)		{ NM_ITEM, t, s, 0, 0, (APTR)i }
#define ItemBar			{ NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL }
#define SubItem(t,s,i)	{ NM_SUB, t, s, 0, 0, (APTR)i }
#define SubBar			{ NM_SUB, NM_BARLABEL, NULL, 0, 0, NULL }
#define EndMenu			{ NM_END, NULL, NULL, 0, 0, NULL }
#define ItCk(t,s,i,f)	{ NM_ITEM, t, s, f, 0, (APTR)i }

static struct NewMenu Menus[] = {
	Title( "File" /* File */ ),
		Item( "Open config file",	NULL, MNID_OPENCONFIG),
		Item( "Save config file",	NULL, MNID_SAVECONFIG),
		Item( "Export config file",	NULL, MNID_EXPORTCONFIG),
		Item( "Import config file",	NULL, MNID_IMPORTCONFIG),
		ItemBar,
			Item( "About", NULL, MNID_ABOUT),
		ItemBar,
			Item( "Quit", NULL, MNID_QUIT),
	Title( "Settings" /* Settings */ ),
		Item( "Global settings",   NULL, MNID_GLOBALSETTINGS   ),
		Item( "Open address book",	 NULL, MNID_OPENADDRESSBOOK	  ),
	EndMenu
};

static void FillViewColumn(char **strings, ViewColumn *data)
{
	if (data) {
		strings[0] = data->name;
		strings[1] = data->size;
		strings[2] = data->flags;
		strings[3] = data->date;
	} else {
		strings[0] = "Name";
		strings[1] = "Size";
		strings[2] = "Flags";
		strings[3] = "Date";
	}
}

static void FillQueueColumn(char **strings, QueueColumn *data)
{
	if (data) {
		switch(data->command) {
			case CMD_DOWNLOAD:
				strings[0] = "DOWNLOAD";
				break;
			
			case CMD_UPLOAD:
				strings[0] = "UPLOAD";
				break;
			
			case CMD_REMOTE_DELETE:
				strings[0] = "REMOTE DELETE";
				break;
			
			case CMD_RENAME:
				strings[0] = "RENAME";
				break;
			
			case CMD_LOCAL_DIR_SCAN:
				strings[0] = "LOCAL DIR SCAN";
				break;
			
			case CMD_REMOTE_DIR_SCAN:
				strings[0] = "REMOTE DIR SCAN";
				break;
				
			case CMD_LOCAL_MKDIR:
				strings[0] = "LOCAL MKDIR";
				break;
				
			case CMD_REMOTE_MKDIR:
				strings[0] = "REMOTE MKDIR";
				break;
				
			case CMD_LOCAL_BASEPATH:
				strings[0] = "LOCAL BASEPATH";
				break;
				
			case CMD_REMOTE_CHDIR:
				strings[0] = "REMOTE CHDIR";
				break;
			
			default:
				strings[0] = "ERROR";
				break;
		}
		
		strings[1] = data->name;
		
		switch (data->status) {
			case STATUS_COMPLETED:
				strings[2] = "COMPLETED";
				break;
			
			case STATUS_ERROR:
				strings[2] = "ERROR";
				break;
			
			case STATUS_QUEUED:
				strings[2] = "QUEUED";
				break;
			
			default:
				strings[2] = "UNKNOWN";
				break;
		}
	} else {
		strings[0] = "Command";
		strings[1] = "Name";
		strings[2] = "Status";
	}
}

#ifdef __AROS__
AROS_UFH3(void, LeftViewDispFunc,
AROS_UFHA(struct Hook *, hook, A0),
AROS_UFHA(char **, strings, A2),
AROS_UFHA(ViewColumn *, data, A1))
{
	AROS_USERFUNC_INIT
	FillViewColumn(strings, data);
	AROS_USERFUNC_EXIT
}

AROS_UFH3(void, RightViewDispFunc,
AROS_UFHA(struct Hook *, hook, A0),
AROS_UFHA(char **, strings, A2),
AROS_UFHA(ViewColumn *, data, A1))
{
	AROS_USERFUNC_INIT
	FillViewColumn(strings, data);
	AROS_USERFUNC_EXIT
}

AROS_UFH3(void, QueueViewDispFunc,
AROS_UFHA(struct Hook *, hook, A0),
AROS_UFHA(char **, strings, A2),
AROS_UFHA(QueueColumn *, data, A1))
{
	AROS_USERFUNC_INIT
	FillQueueColumn(strings, data);
	AROS_USERFUNC_EXIT
}
#endif

#ifdef __MORPHOS__
ULONG LeftViewDispFunc(struct Hook *MyHook, char **strings, ViewColumn *data)
{
	FillViewColumn(strings, data);
	return 0;
}

ULONG RightViewDispFunc(struct Hook *MyHook, char **strings, ViewColumn *data)
{
	FillViewColumn(strings, data);
	return 0;
}

ULONG QueueViewDispFunc(struct Hook *MyHook, char **strings, QueueColumn *data)
{
	FillQueueColumn(strings, data);
	return 0;
}
#endif

#ifdef __SASC
/*
 * 68k register hooks, called directly by MUI through h_Entry: no need for
 * amiga.lib HookEntry, and they work with both PARAMETERS=STACK and =REGISTER
 */
CAF_HOOK(ULONG, LeftViewDispFunc, char **, strings, ViewColumn *, data)
{
	FillViewColumn(strings, data);
	return 0;
}

CAF_HOOK(ULONG, RightViewDispFunc, char **, strings, ViewColumn *, data)
{
	FillViewColumn(strings, data);
	return 0;
}

CAF_HOOK(ULONG, QueueViewDispFunc, char **, strings, QueueColumn *, data)
{
	FillQueueColumn(strings, data);
	return 0;
}

#define CAF_HOOK_INIT(func) { {NULL, NULL}, (HOOKFUNC) func, NULL, NULL }
#else
#define CAF_HOOK_INIT(func) { {NULL, NULL}, (HOOKFUNC) HookEntry, (HOOKFUNC) func, NULL }
#endif

struct Hook RightViewDispHook	= CAF_HOOK_INIT(RightViewDispFunc);
struct Hook LeftViewDispHook	= CAF_HOOK_INIT(LeftViewDispFunc);
struct Hook QueueDisplayHook	= CAF_HOOK_INIT(QueueViewDispFunc);

/* ---------------------------------------------------------------------------
 * Window creation
 * ------------------------------------------------------------------------- */
static bool open_libs(void);

bool gui_open(void)
{
	MuiGui *cn = &g_gui;
	APTR MUIMenu;

	if (!open_libs())
		return false;

	/* BEGIN MUI Gui Definition */
	app = ApplicationObject,
		MUIA_Application_Title, (IPTR) "MarranoFTP Plus",
		MUIA_Application_Version, (IPTR) "$VER: MarranoFTP Plus 1.0 (13.11.2026) © Stefano Crosara aka Suppah at marranosoft@gmail.com",
		MUIA_Application_Copyright, (IPTR) "© Stefano Crosara aka Suppah",
		MUIA_Application_Author, (IPTR) "Stefano Crosara",
		MUIA_Application_Description,  (IPTR) "A very 'marrano' ftp client",
		MUIA_Application_Base, (IPTR) "MarranoFTPPlus",
		MUIA_Application_SingleTask, FALSE,
		// "(IPTR) x = ..." is a cast used as lvalue: an old gcc extension that
		// SAS/C (and any ANSI compiler) refuses, the cast goes on the assignment
		MUIA_Application_Menustrip, (IPTR) (MUIMenu = MUI_MakeObject(MUIO_MenustripNM,Menus,0)),
		
		// Main FTP Browse Window(s)
		SubWindow, (IPTR) (cn->Window = WindowObject,
			MUIA_Window_Title, (IPTR) "MarranoFTP Plus 1.0 (13.11.2026) © Stefano Crosara aka Suppah at marranosoft@gmail.com",
			MUIA_Window_Width, MUIV_Window_Width_MinMax(100),
			MUIA_Window_Height, MUIV_Window_Height_MinMax(100),
			MUIA_Window_ID, MAKE_ID('M','F','T','P'),
			WindowContents, 
				VGroup,
					Child, HGroup, 
						MUIA_Weight, 25,
						Child, cn->LV_STATUS = NListviewObject, 
							MUIA_Listview_Input, FALSE, 
							MUIA_Listview_List, NListObject,
								MUIA_CycleChain, 1, 
								ReadListFrame,
								MUIA_List_ConstructHook, MUIV_NList_ConstructHook_String,
								MUIA_List_DestructHook, MUIV_NList_DestructHook_String,
								MUIA_List_AutoVisible, TRUE,
								End, // NListObject
							End, // NListviewObject
						End, // HGroup
					
					Child, HGroup,
						MUIA_Weight, 53,
						Child, VGroup,
							Child, cn->S_LEFT_VIEW_PATH = (APTR) StringObject, StringFrame, MUIA_CycleChain, 1, MUIA_String_MaxLen, 512, End,
							
							Child, cn->lv.ListView = NListviewObject,
								MUIA_NListview_NList, NListObject,
									MUIA_CycleChain, 1, 
									MUIA_NList_Format, "MAXW=50,MAXW=20,MAXW=20,MAXW=10",
									MUIA_NList_TypeSelect, MUIV_NList_TypeSelect_Line,
									MUIA_NList_MultiSelect, MUIV_NList_MultiSelect_Shifted, 
									MUIA_Font, MUIV_Font_Fixed,
									MUIA_NList_DisplayHook, &LeftViewDispHook,
									MUIA_NList_Title, TRUE,
									MUIA_NList_TitleSeparator, TRUE,
									MUIA_NList_AutoVisible, TRUE,
									MUIA_NList_EntryValueDependent, TRUE,
									MUIA_NList_MinColSortable, 0,
								End, // NListObject
							End, // NListviewObject
						End, // VGroup
						
						Child, VGroup,
							Child, cn->S_RIGHT_VIEW_PATH = (APTR) StringObject, StringFrame, MUIA_CycleChain, 1, MUIA_String_MaxLen, 512, End,
							
							Child, cn->rv.ListView = NListviewObject, 
								MUIA_NListview_NList, NListObject,
									MUIA_CycleChain, 1, 
									MUIA_NList_Format, "MAXW=50,MAXW=20,MAXW=20,MAXW=10",
									MUIA_NList_TypeSelect, MUIV_NList_TypeSelect_Line,
									MUIA_NList_MultiSelect, MUIV_NList_MultiSelect_Shifted, 
									MUIA_Font, MUIV_Font_Fixed,
									MUIA_NList_DisplayHook, &RightViewDispHook,
									MUIA_NList_Title, TRUE,
									MUIA_NList_TitleSeparator, TRUE,
									MUIA_NList_AutoVisible, TRUE,
									MUIA_NList_EntryValueDependent, TRUE,
									MUIA_NList_MinColSortable, 0,
								End, // NListObject
							End, // NListviewObject
						End, // VGroup
					End, // HGroup
					
					Child, VGroup,
						MUIA_Weight, 20,
						Child, TextObject, MUIA_Text_Contents, "Transfer Queue", End,
						
						Child, cn->QueueLV = NListviewObject, 
							MUIA_NListview_NList, NListObject,
							MUIA_CycleChain, 1, 
							MUIA_NList_Format, "MAXW=50,MAXW=20,MAXW=20",
							MUIA_NList_TypeSelect, MUIV_NList_TypeSelect_Line,
							MUIA_NList_MultiSelect, MUIV_NList_MultiSelect_Shifted, 
							MUIA_Font, MUIV_Font_Fixed,
							MUIA_NList_DisplayHook, &QueueDisplayHook,
							MUIA_NList_Title, TRUE,
							MUIA_NList_TitleSeparator, TRUE,
							MUIA_NList_AutoVisible, TRUE,
							MUIA_NList_EntryValueDependent, TRUE,
							MUIA_NList_MinColSortable, 0,
							End, // NListObject
						End, // NListviewObject
					End, // VGroup
					
					Child, cn->S_TRANSFER_INFO =	StringObject, StringFrame, 
												MUIA_InputMode, MUIV_InputMode_None,
												MUIA_String_Accept, "",
												MUIA_String_MaxLen, 512, 
					End,
					
					Child, HGroup, 
						Child, cn->BTN_DOWNLOAD = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  Download   ", End,
						End,
							
						Child, cn->BTN_UPLOAD = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  Upload  ", End,
						End,
							
						Child, cn->BTN_DELETE = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  Delete  ", End,
						End,
						
						Child, cn->BTN_STOP = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  Stop   ", End,
						End,
						
						Child, cn->BTN_RUN_QUEUE = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  Run Queue  ", End,
						End,
						
						Child, cn->BTN_STEP_QUEUE = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  Step Queue  ", End,
						End,
						
						Child, cn->BTN_CLEAR_QUEUE = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  Clear Queue  ", End,
						End,
						
						Child, cn->BTN_RENAME = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  Rename  ", End,
						End,
							
						Child, cn->BTN_MAKEDIR = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  Makedir  ", End,
						End,
						
						//
						//Child, cn->BTN_DUMP = HGroup,
							//ButtonFrame,
							//MUIA_InputMode, MUIV_InputMode_RelVerify,
							//MUIA_Background, MUII_ButtonBack,
							//Child, TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c Dump", End,
						//End,
						
						
					End,
					
					Child, HGroup,
						Child, cn->BTN_CONNDISC = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  Disconnect  ", End,
						End,
						
						Child, cn->BTN_QUIT = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  Quit   ", End,
						End,
						
						Child, cn->BTN_REFRESH_L = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  RefreshL   ", End,
						End,
							
						Child, cn->BTN_REFRESH_R = HGroup,
							ButtonFrame,
							MUIA_InputMode , MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child,	TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c  RefreshR   ", End,
						End,
					End,
				End,
			End), // WindowObject (main window)
		
		// Address Book Window
		SubWindow, gab.Window = WindowObject,
			MUIA_Window_Title, "Address Book",
//			MUIA_Window_Width, MUIV_Window_Width_MinMax(20),
//			MUIA_Window_Height, MUIV_Window_Height_MinMax(20),
			MUIA_Window_ID, MAKE_ID('S','I','T','E'),
			WindowContents,
				VGroup, 
					Child, HGroup, 
						Child, gab.LV_HOSTS = NListviewObject, 
								MUIA_Listview_List, NListObject,
									MUIA_CycleChain, 1, 
									ReadListFrame,
									MUIA_List_AutoVisible, TRUE,
								End,
						End,
					End,
					
					Child, HGroup, 
						Child, VGroup,
							Child, HGroup,
								MUIA_Weight, 70,
								Child, TextObject, MUIA_Text_Contents, "Entry name", End,
							End,
							
							Child, HGroup,
								MUIA_Weight, 70,
								Child, gab.S_ENTRYNAME = (APTR) StringObject, MUIA_CycleChain, 1, StringFrame, MUIA_String_MaxLen, 128, End,
							End,
							
							Child, HGroup,
								Child, HGroup,
									MUIA_Weight, 70,
									Child, TextObject, MUIA_Text_Contents, "Address", End,
								End,
								Child, HGroup,
									MUIA_Weight, 20,
									Child, TextObject, MUIA_Text_Contents, "Port", End,
								End,
							End,
							
							Child, HGroup, 
								Child, HGroup,
									MUIA_Weight, 70,
									Child, gab.S_HOSTNAME	 = (APTR) StringObject, MUIA_CycleChain, 1, StringFrame, MUIA_String_MaxLen, 128, End,
								End,
								Child, HGroup,
									MUIA_Weight, 20,
									Child, gab.S_PORT		 = (APTR) StringObject, MUIA_CycleChain, 1, StringFrame, MUIA_String_MaxLen, 8, End,
								End,
							End, 
							
							Child, TextObject, MUIA_Text_Contents, "Username", End,
							Child, gab.S_USERNAME	 = (APTR) StringObject, MUIA_CycleChain, 1,StringFrame, MUIA_String_MaxLen, 32, End,
							Child, TextObject, MUIA_Text_Contents, "Password", End,
							Child, gab.S_PASSWORD	 = (APTR) StringObject, MUIA_CycleChain, 1,StringFrame, MUIA_String_MaxLen, 32, End,
						End,	
					End,
					
					Child, HGroup,
						Child, VGroup,
							Child, TextObject, MUIA_Text_Contents, "Transfer type", End,
							Child, gab.RDIO_CONN_TYPE = CycleObject,
								MUIA_Cycle_Entries, g_AddressBook_ConnectionTypes,
							End,
						End,
					End,
					
					Child, HGroup, 
						Child, gab.BTN_NEW = HGroup,
							ButtonFrame,
							MUIA_InputMode, MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child, TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c New", End,
						End,
						
						Child, gab.BTN_CHANGE = HGroup,
							ButtonFrame,
							MUIA_InputMode, MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child, TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c Change", End,
						End,
						
						Child, gab.BTN_DELETE = HGroup,
							ButtonFrame,
							MUIA_InputMode, MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child, TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c Delete", End,
						End,
						
						Child, gab.BTN_EXIT = HGroup,
							ButtonFrame,
							MUIA_InputMode, MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child, TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c Exit", End,
						End,

						Child, gab.BTN_SAVE = HGroup,
							ButtonFrame,
							MUIA_InputMode, MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child, TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c Save", End,
						End,
						
					End,
				
				End,
			End,
			
		// Error Window
		SubWindow, gab.Window_HostsFull = WindowObject,
			MUIA_Window_Title, "Error!",
			WindowContents,
				VGroup, 
					Child, TextObject, MUIA_Text_Contents, "Reached maximum number of hosts", End,
					Child, gab.BTN_HostsFullOk = HGroup,
						ButtonFrame,
						MUIA_InputMode, MUIV_InputMode_RelVerify,
						MUIA_Background, MUII_ButtonBack,
						Child, TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c Exit", End,
					End,
				End,
			End,
		
		// Global Settings Window 
		SubWindow, ggs.Window = WindowObject,
			MUIA_Window_Title, "Global Settings",
			WindowContents,
				VGroup, 
					Child, TextObject, MUIA_Text_Contents, "\33cFile Transfer Timeout", End,
					Child, ggs.S_TRANSFER_SOCKET_TIMEOUT = StringObject, 
											StringFrame, MUIA_String_MaxLen, 3, End,
					
					Child, TextObject, MUIA_Text_Contents, "\33cConnection Timeout", End,
					Child, ggs.S_CONNECTION_SOCKET_TIMEOUT = StringObject, 
											StringFrame, MUIA_String_MaxLen, 3, End,
					
					Child, TextObject, MUIA_Text_Contents, "\33cQueue Processing", End,
					Child, ggs.RDIO_QUEUE_TYPE = RadioObject,
						MUIA_Group_Horiz, TRUE,
						MUIA_Radio_Entries, g_GlobalSettings_QueueTypes,
					End,
					
					Child, TextObject, MUIA_Text_Contents, "\33cPartial files", End,
					Child, ggs.RDIO_DELETE_PARTIAL = RadioObject,
						MUIA_Group_Horiz, TRUE,
						MUIA_Radio_Entries, g_GlobalSettings_DeletePartial,
					End,
					
					Child, HGroup,
						Child, ggs.BTN_SAVE = HGroup,
							ButtonFrame,
							MUIA_InputMode, MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child, TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c Save", End,
						End,
						
						Child, ggs.BTN_CANCEL = HGroup,
							ButtonFrame,
							MUIA_InputMode, MUIV_InputMode_RelVerify,
							MUIA_Background, MUII_ButtonBack,
							Child, TextObject, MUIA_CycleChain, 1, MUIA_Text_Contents, "\33c Cancel", End,
						End,
					End,
				End,
			End,
		End; // Application Object
	/* END MUI Gui Definition */
	
	if (!app) {
		DebugOutput("Cannot create application object.\n");
		printf("Cannot create application object.\n");
		return false;
	}
	
	// Main Window
	DoMethod(cn->Window, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
	DoMethod(cn->BTN_CONNDISC, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_CONNDISC);
	DoMethod(cn->BTN_STOP, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_STOP);
	DoMethod(cn->BTN_RUN_QUEUE, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_QUEUE_PROCESS);
	DoMethod(cn->BTN_STEP_QUEUE, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_QUEUE_STEP);
	DoMethod(cn->BTN_CLEAR_QUEUE, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_CLEAR_QUEUE);
	DoMethod(cn->BTN_DOWNLOAD, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_DOWNLOAD);
	DoMethod(cn->BTN_UPLOAD, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_UPLOAD);
	DoMethod(cn->BTN_DELETE, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_DELETE);
	DoMethod(cn->BTN_RENAME, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_RENAME);
	DoMethod(cn->BTN_MAKEDIR, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_MAKEDIR);
	DoMethod(cn->BTN_QUIT, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_QUIT);
	DoMethod(cn->BTN_REFRESH_L, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_REFRESH_L);
	DoMethod(cn->BTN_REFRESH_R, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CLICKED_REFRESH_R);
	
	DoMethod(cn->S_LEFT_VIEW_PATH, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_MODIFIED_LPATHBOX);
	DoMethod(cn->S_RIGHT_VIEW_PATH, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_MODIFIED_RPATHBOX); 
	
	DoMethod(cn->lv.ListView, MUIM_Notify, MUIA_NList_DoubleClick , MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_DBLCLICKED_LEFT_LISTVIEW);
	DoMethod(cn->rv.ListView, MUIM_Notify, MUIA_NList_DoubleClick , MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_DBLCLICKED_RIGHT_LISTVIEW);
	
	// Address book window
	DoMethod(gab.Window, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, app, 2, MUIM_Application_ReturnID, ID_SITE_WINDOW_CLICKED_EXIT);
	DoMethod(gab.BTN_NEW, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_SITE_WINDOW_CLICKED_NEW);
	DoMethod(gab.BTN_CHANGE, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_SITE_WINDOW_CLICKED_CHANGE);
	DoMethod(gab.BTN_DELETE, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_SITE_WINDOW_CLICKED_DELETE);
	DoMethod(gab.BTN_SAVE, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_SITE_WINDOW_CLICKED_SAVE);
	DoMethod(gab.BTN_EXIT, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_SITE_WINDOW_CLICKED_EXIT);
	DoMethod(gab.BTN_HostsFullOk, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_SITE_WINDOW_FULL_HOSTS_WINDOW_OK);
	DoMethod(gab.LV_HOSTS, MUIM_Notify, MUIA_NList_DoubleClick, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_DBLCLICKED_SITE_WINDOW_HOSTS);
	DoMethod(gab.LV_HOSTS, MUIM_Notify, MUIA_NList_SelectChange, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_CHANGESEL_SITE_WINDOW_HOSTS);
	
	// Global settings window
	DoMethod(ggs.Window, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, app, 2, MUIM_Application_ReturnID, ID_GLOBAL_SETTINGS_WINDOW_CANCEL);
	DoMethod(ggs.BTN_SAVE, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_GLOBAL_SETTINGS_WINDOW_SAVE);
	DoMethod(ggs.BTN_CANCEL, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_GLOBAL_SETTINGS_WINDOW_CANCEL);
	

	{
		IPTR iresult = 0;

		set(cn->Window, MUIA_Window_Open, TRUE);
		get(cn->Window, MUIA_Window_Open, &iresult);
		if (!iresult) {
			printf("Cannot open the main window.\n");
			return false;
		}
	}
	gui_handle(0);			/* first NewInput: gets the signal mask */
	return true;
}

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */
static void MUI_GetSet(APTR object, ULONG attrib, ULONG state)
{
	IPTR itmp;

	get(object, attrib, &itmp);
	if (itmp != state)
		set(object, attrib, state);
}

static char *string_of(APTR obj)
{
	IPTR itmp = 0;

	get(obj, MUIA_String_Contents, &itmp);
	return itmp ? (char *) itmp : "";
}

/* ---------------------------------------------------------------------------
 * Address book window
 * ------------------------------------------------------------------------- */
static void EvalSiteWindowStatus(void)
{
	BOOL bState = (core_host_count() || gab.b_new_entry) ? FALSE : TRUE;

	MUI_GetSet(gab.BTN_SAVE, MUIA_Disabled, bState);
	MUI_GetSet(gab.BTN_DELETE, MUIA_Disabled, bState);
	MUI_GetSet(gab.S_ENTRYNAME, MUIA_Disabled, bState);
	MUI_GetSet(gab.S_HOSTNAME, MUIA_Disabled, bState);
	MUI_GetSet(gab.S_PORT, MUIA_Disabled, bState);
	MUI_GetSet(gab.S_USERNAME, MUIA_Disabled, bState);
	MUI_GetSet(gab.S_PASSWORD, MUIA_Disabled, bState);
	MUI_GetSet(gab.RDIO_CONN_TYPE, MUIA_Disabled, bState);
}

static void UpdateSiteWindowListBox(int active)
{
	int i;

	set(gab.LV_HOSTS, MUIA_NList_Quiet, TRUE);
	DoMethod(gab.LV_HOSTS, MUIM_NList_Clear);
	for (i = 0; i < core_host_count(); i++)
		DoMethod(gab.LV_HOSTS, MUIM_NList_InsertSingle, (IPTR) core_host_get(i)->entryname, MUIV_NList_Insert_Bottom);
	set(gab.LV_HOSTS, MUIA_NList_Active, active);
	set(gab.LV_HOSTS, MUIA_NList_Quiet, FALSE);
	EvalSiteWindowStatus();
}

static void UpdateSiteWindowContents(int active)
{
	Host *hst = core_host_get(active);

	if (!hst)
		return;
	set(gab.S_ENTRYNAME, MUIA_String_Contents, (IPTR) hst->entryname);
	set(gab.S_HOSTNAME, MUIA_String_Contents, (IPTR) hst->hostname);
	set(gab.S_PORT, MUIA_String_Contents, (IPTR) hst->port);
	set(gab.S_USERNAME, MUIA_String_Contents, (IPTR) hst->username);
	set(gab.S_PASSWORD, MUIA_String_Contents, (IPTR) hst->password);
	set(gab.RDIO_CONN_TYPE, MUIA_Cycle_Active, hst->conn_type);
}

static void UpdateSiteWindow(int active)
{
	UpdateSiteWindowListBox(active);
	UpdateSiteWindowContents(active);
	EvalSiteWindowStatus();
}

static void OnSiteWindowNew(void)
{
	if (core_host_count() + 1 >= MAX_HOSTS) {
		set(gab.Window_HostsFull, MUIA_Window_Open, TRUE);
		return;
	}
	gab.b_new_entry = true;
	set(gab.S_ENTRYNAME, MUIA_String_Contents, (IPTR) "");
	set(gab.S_HOSTNAME, MUIA_String_Contents, (IPTR) "");
	set(gab.S_PORT, MUIA_String_Contents, (IPTR) "21");
	set(gab.S_USERNAME, MUIA_String_Contents, (IPTR) "");
	set(gab.S_PASSWORD, MUIA_String_Contents, (IPTR) "");
	set(gab.RDIO_CONN_TYPE, MUIA_Cycle_Active, 0);
	set(gab.Window, MUIA_Window_ActiveObject, gab.S_ENTRYNAME);
	EvalSiteWindowStatus();
}

static void OnSiteWindowChange(void)
{
	IPTR itmp = 0;
	Host h;

	caf_memset(&h, 0, sizeof(Host));
	caf_strncpy(h.entryname, string_of(gab.S_ENTRYNAME), ENTRYNAME_CONFIG_LEN);
	caf_strncpy(h.hostname, string_of(gab.S_HOSTNAME), HOSTNAME_CONFIG_LEN);
	caf_strncpy(h.port, string_of(gab.S_PORT), PORT_COFING_LEN);
	caf_strncpy(h.username, string_of(gab.S_USERNAME), USERNAME_CONFIG_LEN);
	caf_strncpy(h.password, string_of(gab.S_PASSWORD), PASSWORD_CONFIG_LEN);
	get(gab.RDIO_CONN_TYPE, MUIA_Cycle_Active, &itmp);
	h.conn_type = (int) itmp;		/* CTYPE_ACTV, CTYPE_PASV, CTYPE_SFTP */

	if (gab.b_new_entry) {
		int idx = core_host_add(&h);
		if (idx < 0)
			return;
		gab.selected_host = idx;
	} else {
		/* selected_host is -1 (MUIV_NList_Active_Off) when nothing is selected */
		if (!core_host_get(gab.selected_host))
			return;
		core_host_set(gab.selected_host, &h);
	}
	gab.b_new_entry = false;
	UpdateSiteWindowListBox(gab.selected_host);
}

static void OnSiteWindowDelete(void)
{
	IPTR itmp;

	if (gab.b_new_entry) {
		gab.b_new_entry = false;
		UpdateSiteWindowContents(0);
		EvalSiteWindowStatus();
		return;
	}
	get(gab.LV_HOSTS, MUIA_NList_Active, &itmp);
	if ((LONG) itmp >= 0 && (LONG) itmp < core_host_count()) {
		core_host_delete((LONG) itmp);
		UpdateSiteWindow(0);
	}
}

static void OnSiteWindowHostsLVDBLClick(void)
{
	IPTR itmp;

	get(gab.LV_HOSTS, MUIA_NList_Active, &itmp);
	if ((LONG) itmp >= 0 && (LONG) itmp < core_host_count()) {
		set(gab.Window, MUIA_Window_Open, FALSE);
		core_connect_host((LONG) itmp);
	}
}

static void OnSiteWindowHostsSelectChange(void)
{
	IPTR itmp;

	get(gab.LV_HOSTS, MUIA_NList_Active, &itmp);
	gab.selected_host = (LONG) itmp;
	UpdateSiteWindowContents((LONG) itmp);
}

/* ---------------------------------------------------------------------------
 * Global settings window
 * ------------------------------------------------------------------------- */
static void EvalGlobalSettingsWindow(void)
{
	char temp[32];

	sprintf(temp, "%d", core_get_setting(SETTING_TRANSFER_TIMEOUT));
	set(ggs.S_TRANSFER_SOCKET_TIMEOUT, MUIA_String_Contents, temp);
	sprintf(temp, "%d", core_get_setting(SETTING_CONNECT_TIMEOUT));
	set(ggs.S_CONNECTION_SOCKET_TIMEOUT, MUIA_String_Contents, temp);
	set(ggs.RDIO_QUEUE_TYPE, MUIA_Radio_Active, core_get_setting(SETTING_QUEUE_TYPE));
	set(ggs.RDIO_DELETE_PARTIAL, MUIA_Radio_Active, core_get_setting(SETTING_KEEP_PARTIAL));
}

static void OnGlobalSettingsSave(void)
{
	IPTR itmp;

	set(ggs.Window, MUIA_Window_Open, FALSE);
	core_set_setting(SETTING_TRANSFER_TIMEOUT, caf_atol(string_of(ggs.S_TRANSFER_SOCKET_TIMEOUT)));
	core_set_setting(SETTING_CONNECT_TIMEOUT, caf_atol(string_of(ggs.S_CONNECTION_SOCKET_TIMEOUT)));
	get(ggs.RDIO_QUEUE_TYPE, MUIA_Radio_Active, &itmp);
	core_set_setting(SETTING_QUEUE_TYPE, (int) itmp);
	get(ggs.RDIO_DELETE_PARTIAL, MUIA_Radio_Active, &itmp);
	core_set_setting(SETTING_KEEP_PARTIAL, (int) itmp);
}

/* ---------------------------------------------------------------------------
 * gui.h implementation
 * ------------------------------------------------------------------------- */
void gui_status(const char *text)
{
	DoMethod(g_gui.LV_STATUS, MUIM_NList_InsertSingle, (IPTR) text, MUIV_NList_Insert_Bottom);
	DoMethod(g_gui.LV_STATUS, MUIM_NList_Jump, MUIV_NList_Jump_Bottom);
}

void gui_transfer_info(const char *text)
{
	set(g_gui.S_TRANSFER_INFO, MUIA_String_Contents, (IPTR) text);
}

void gui_set_connected(bool connected)
{
	set(g_gui.BTN_CONNDISC, MUIA_Text_Contents, connected ? "\33c  Disconnect  " : "\33c  Connect  ");
}

void gui_set_path(int which, const char *path)
{
	set(which == GUI_LOCAL ? g_gui.S_LEFT_VIEW_PATH : g_gui.S_RIGHT_VIEW_PATH, MUIA_String_Contents, (IPTR) path);
}

void gui_set_list(int which, ViewColumn **entries, int count)
{
	APTR lv = which == GUI_LOCAL ? g_gui.lv.ListView : g_gui.rv.ListView;
	int i;

	set(lv, MUIA_NList_Quiet, TRUE);
	DoMethod(lv, MUIM_NList_Clear);
	for (i = 0; i < count; i++)
		DoMethod(lv, MUIM_NList_InsertSingle, (IPTR) entries[i], MUIV_NList_Insert_Bottom);
	set(lv, MUIA_NList_Quiet, FALSE);
}

int gui_get_selection(int which, int *idx, int max)
{
	APTR lv = which == GUI_LOCAL ? g_gui.lv.ListView : g_gui.rv.ListView;
	LONG id = MUIV_NList_NextSelected_Start;
	int n = 0;

	while (n < max) {
		DoMethod(lv, MUIM_NList_NextSelected, &id);
		if (id == MUIV_NList_NextSelected_End)
			break;
		idx[n++] = id;
	}
	return n;
}

void gui_queue_changed(void)
{
	int i, n = 0;

	set(g_gui.QueueLV, MUIA_NList_Quiet, TRUE);
	DoMethod(g_gui.QueueLV, MUIM_NList_Clear);
	for (i = core_queue_first(); i < core_queue_used() && n < QUEUE_SIZE; i++, n++) {
		QueueItem *q = core_queue_item(i);
		g_queue_cols[n].command = q->command;
		g_queue_cols[n].name = q->data;
		g_queue_cols[n].status = STATUS_QUEUED;
		DoMethod(g_gui.QueueLV, MUIM_NList_InsertSingle, (IPTR) &g_queue_cols[n], MUIV_NList_Insert_Bottom);
	}
	set(g_gui.QueueLV, MUIA_NList_Quiet, FALSE);
}

void gui_hosts_changed(void)
{
	gab.b_new_entry = false;
	gab.selected_host = 0;
	UpdateSiteWindow(0);
}

ULONG gui_signals(void)
{
	return g_mui_sigs;
}

/* Dispatches one MUI return ID. Returns FALSE to quit at once. */
static bool handle_id(LONG id)
{
	MuiGui *cn = &g_gui;
	IPTR itmp;

	switch (id) {
		case MUIV_Application_ReturnID_Quit:	/* close gadget */
			return false;
		case ID_CLICKED_CONNDISC:		core_connect_toggle(); break;
		case ID_CLICKED_STOP:			core_stop(); break;
		case ID_MODIFIED_LPATHBOX:
		case ID_CLICKED_REFRESH_L:		core_set_local_path(string_of(cn->S_LEFT_VIEW_PATH)); break;
		case ID_MODIFIED_RPATHBOX:		core_set_remote_path(string_of(cn->S_RIGHT_VIEW_PATH)); break;
		case ID_CLICKED_REFRESH_R:		core_remote_refresh(); break;
		case ID_DBLCLICKED_LEFT_LISTVIEW:
			get(cn->lv.ListView, MUIA_NList_Active, &itmp);
			core_local_open((LONG) itmp);
			break;
		case ID_DBLCLICKED_RIGHT_LISTVIEW:
			get(cn->rv.ListView, MUIA_NList_Active, &itmp);
			core_remote_open((LONG) itmp);
			break;
		case ID_CLICKED_QUEUE_PROCESS:	core_queue_run(); break;
		case ID_CLICKED_QUEUE_STEP:		core_queue_step(); break;
		case ID_CLICKED_CLEAR_QUEUE:	core_queue_clear(); break;
		case ID_CLICKED_DOWNLOAD:		core_download_selected(); break;
		case ID_CLICKED_UPLOAD:			core_upload_selected(); break;
		case ID_CLICKED_DELETE:			core_delete_selected(); break;
		case MNID_QUIT:
		case ID_CLICKED_QUIT:			core_request_quit(); break;
		case MNID_SAVECONFIG:
			gui_status(core_save_config() ? "STATUS: Configuration saved" : "STATUS: Error while saving configuration");
			break;

		case ID_SITE_WINDOW_CLICKED_NEW:		OnSiteWindowNew(); break;
		case ID_SITE_WINDOW_CLICKED_CHANGE:		OnSiteWindowChange(); break;
		case ID_SITE_WINDOW_CLICKED_SAVE:
			gui_status(core_save_config() ? "STATUS: Configuration saved" : "STATUS: Error while saving configuration");
			break;
		case ID_SITE_WINDOW_CLICKED_DELETE:		OnSiteWindowDelete(); break;
		case ID_SITE_WINDOW_CLICKED_EXIT:		set(gab.Window, MUIA_Window_Open, FALSE); break;
		case ID_DBLCLICKED_SITE_WINDOW_HOSTS:	OnSiteWindowHostsLVDBLClick(); break;
		case ID_CHANGESEL_SITE_WINDOW_HOSTS:	OnSiteWindowHostsSelectChange(); break;
		case ID_SITE_WINDOW_FULL_HOSTS_WINDOW_OK: set(gab.Window_HostsFull, MUIA_Window_Open, FALSE); break;

		case MNID_OPENADDRESSBOOK:
			set(gab.Window, MUIA_Window_Open, TRUE);
			get(gab.Window, MUIA_Window_Open, &itmp);
			if (!itmp)
				gui_status("ERROR: Cannot open address book window");
			else {
				gab.b_new_entry = false;
				gab.selected_host = 0;
				UpdateSiteWindow(0);
			}
			break;

		case MNID_GLOBALSETTINGS:
			EvalGlobalSettingsWindow();
			set(ggs.Window, MUIA_Window_Open, TRUE);
			break;
		case ID_GLOBAL_SETTINGS_WINDOW_SAVE:	OnGlobalSettingsSave(); break;
		case ID_GLOBAL_SETTINGS_WINDOW_CANCEL:	set(ggs.Window, MUIA_Window_Open, FALSE); break;
	}
	return true;
}

bool gui_handle(ULONG sigs)
{
	ULONG s = sigs;
	LONG id;

	for (;;) {
		id = DoMethod(app, MUIM_Application_NewInput, &s);
		if (id == 0)
			break;
		if (!handle_id(id))
			return false;
	}
	g_mui_sigs = s;
	return true;
}

/* ---------------------------------------------------------------------------
 * Libraries, window creation
 * ------------------------------------------------------------------------- */
static bool open_libs(void)
{
	IntuitionBase = (struct IntuitionBase *) OpenLibrary((STRPTR) "intuition.library", 0);
	MUIMasterBase = OpenLibrary((STRPTR) MUIMASTER_NAME, 0);
	if (!IntuitionBase || !MUIMasterBase) {
		printf("Failed to open %s\n", IntuitionBase ? "muimaster.library" : "intuition.library");
		return false;
	}
	g_libs_open = true;
	return true;
}

static void close_libs(void)
{
	if (MUIMasterBase)
		CloseLibrary(MUIMasterBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *) IntuitionBase);
	MUIMasterBase = 0;
	IntuitionBase = 0;
}

void gui_close(void)
{
	if (app)
		MUI_DisposeObject(app);
	app = 0;
	close_libs();
}

int main(int argc, char *argv[])
{
	#ifdef __SASC
	/* MUI needs Kickstart 2.0+ (MarranoFTP13 is the OS 1.3 version) */
	if (SysBase->LibNode.lib_Version < 37) {
		printf("MarranoFTP requires AmigaOS 2.04 (Kickstart 37) or newer,\n"
			   "use MarranoFTP13 on older systems\n");
		return RETURN_FAIL;
	}
	#endif

	if (core_init() && gui_open()) {
		core_start();
		core_run();
	}
	gui_close();
	core_cleanup();
	return RETURN_OK;
}
