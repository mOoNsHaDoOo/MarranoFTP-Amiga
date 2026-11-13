/*
 * FTP_CORE.C - MarranoFTP core: FTP protocol, sockets, transfer queue,
 * configuration. No user interface code in here: see core.h and gui.h.
 *
 * Only OS 1.3 compatible system calls are used, the same file builds the MUI
 * version (gui_mui.c) and the OS 1.3 GadTools version (gui_gt.c).
 */

#ifdef __SASC
/* AmiTCP SDK 4.3 + NDK 3.2: AmiTCP <sys/time.h> includes <proto/timer.h>,
   both declare TimerBase with different types (SAS/C Error 72) */
#define PROTO_TIMER_H 1
#endif

#include <exec/types.h>
#include <exec/exec.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __AROS__
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>
#endif

#ifdef __MORPHOS__
#include <ppcinline/exec.h>
#include <ppcinline/dos.h>
#include <ppcinline/socket.h>
#include <net/socketbasetags.h>
#include <clib/alib_protos.h>
#endif

#ifdef __SASC
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>
#include <clib/alib_protos.h>
#endif

#include <caf/caf_types.h>
#include <caf/caf_debugging.h>
#include <caf/networking_helpers.h>
#include <caf/thread_helpers.h>
#include <caf/errors.h>
#include <caf/caf_platform.h>

#include "core.h"
#include "gui.h"

/* ---------------------------------------------------------------------------
 * Globals
 * ------------------------------------------------------------------------- */
struct Library		*SocketBase;
struct timerequest	*g_TimerIO;
struct MsgPort		*g_TimerMP;
static bool			g_timer_open;

Connection g_Connection;
AddressBook g_AddressBook;
GlobalSettings g_GlobalSettings;

static char *STR_CANT_SEND = "STATUS: cannot send command, waiting for ftp response to previous command";
static char *STR_CANT_SEND_NOT_CONNECTED = "STATUS: cannot send command, not connected";

#define STACK_BOTTOM (7)
#define STACK_CURRENT (0)
static int recv_command_stack[STACK_BOTTOM+1];
static char sent_command_stack[STACK_BOTTOM+1][512];

static TimerVal g_listen_timer, g_last_packet_transfered_timer, g_trying_connect_timer,
				g_transfer_start_timer, g_gui_update_timer;
static char g_temp[1024];
static char g_config_filename[512];
static char g_QueueLocalPath[512];		/* local directory while a queue runs */
static bool g_quit_now;					/* the main loop must end */

/* Temporary list used to put the directories before the files */
static ViewColumn *g_files_tmp[MAX_VIEW_ENTRIES];

/* ---------------------------------------------------------------------------
 * Prototypes
 * ------------------------------------------------------------------------- */
bool RefreshLocalView(Connection *cn);
bool FtpConnect(Connection *cn);
int FtpDisconnect(Connection *cn);
bool OpenListenSocket(Connection *cn);
int PasvConnection(Connection *cn);
bool CloseListenSocket(Connection *cn);
bool CloseDataSocket(Connection *cn);
int SetTransferMode(Connection *cn, char mode);
int SetTransferPort(Connection *cn);
int SendLISTCMD(Connection *cn);
int SendPWDCMD(Connection *cn);
int SendCWDCMD(Connection *cn, char *path);
int SendRETRCMD(Connection *cn, char *filename);
int SendSTORCMD(Connection *cn, char *filename);
int SendMKDCMD(Connection *cn, char *pathname);
int SendPASVCMD(Connection *cn);
int SendFtpCommand(Connection *cn, char *CMD, bool b_block);
int GetFtpCommand(struct buffer *buffer, Connection *cn);
bool CheckConnect(Connection *cn);
bool CheckSocket(SOCKET socket);
void InvalidateSocket(SOCKET *socket);
bool GetConnectionInfo(ClientInfo *ci);
bool CheckSockForErrors(Connection *cn, SOCKET *socket);
void CleanSockAndFlags(Connection *cn);
void RecvStackPush(int command);
void SentStackPush(char *command);
int GetCommandDigit(int command, int digit);
void ProcessCommand(Connection *cn, int command);
bool ProcessError(Connection *cn, int command);
void ProcessDirectoryData(Connection *cn);
bool IsLastSentCmd(char *str);
void ClearTransferFlags(Connection *cn);
bool AbortFileTransfer(Connection *cn);
int SockConnect(CSOCK *csock);
void DisplaySockErrs(Connection *cn, CSOCK *csock);
void AcknowledgeConnection(Connection *cn);
int GatherIpPortNumber(Connection *cn);
void AfterRETR(Connection *cn);
void AfterSTOR(Connection *cn);
void AfterMKD(Connection *cn);
void AfterCWD(Connection *cn);
void AfterDELE(Connection *cn);
void GetPathFromRawData(char *text);
int GetFtpCommandValue(char *buffer);
void SetConnected(Connection *cn, bool b_con);
void FormatFtpListing(Connection *cn, buffer *buffer, bool b_last_data);
char *IpToStr(int ip, char sepchars);
int InitiateListTransfer(Connection *cn);
int InitiateFileTransfer(Connection *cn, char *filename, bool b_download);
int InitiateFileDeletion(Connection *cn, char *filename);
bool LocalMakedir(Connection *cn, char *pathname);
void RemoteMakedir(Connection *cn, char *pathname);
void LocalBasePath(Connection *cn, char *pathname);
void RemoteChdir(Connection *cn, char *pathname);
bool LocalDirScan(Connection *cn, char *pathname, char *basepath, int base_start);
void RemoteDirScan(Connection *cn, char *pathname);
void HandleDownload(Connection *cn);
void HandleUpload(Connection *cn);
void StartTimer(TimerVal *tv);
void UpdateTimer(TimerVal *tv);
int TimeInSecs(TimerVal *tv);
ULONG ElapsedMs(TimerVal *tv);
void QueueAdvance(Connection *cn);
bool QueuePush(Connection *cn, int command, char *data, char *c_arg1, int i_arg1, bool b_local_cmd);
void *QueuePushAddStr(Connection *cn, buffer *buf, char *string, int command, char *c_arg1, int i_arg1, bool b_local_cmd);
bool QueuePop(Connection *cn);
void QueueClear(Connection *cn);
void QueueProcess(Connection *cn);
void QueuePostProcess(Connection *cn);
void DateStampToStr(char *dest, struct DateStamp *ds);
void DecodeBitFlags(char *dest, LONG flags);
void BufferFlush(buffer *buf);
void BufferOpen(buffer *buf, bool b_bufclear);
char *BufferAddStr(buffer *buf, char *string);
void *BufferAddStruct(buffer *buf, void *struc, int size);
void BufferClose(buffer *buf);
void *AlignPtr(void *ptr, int align_size, int *align_amount);
int MemReadString(char *dest, char **ptr, int len, int maxlen);
int MemReadDword(char **ptr);
static void LoadConfig(Connection *cn);

/* ---------------------------------------------------------------------------
 * Status window helpers
 * ------------------------------------------------------------------------- */
static void status_command(char *cmd)
{
	char temp[520];

	caf_strncpy(temp, "COMMAND: ", sizeof(temp));
	caf_strncat(temp, cmd, sizeof(temp));
	gui_status(temp);
}

/* The lines of the last server answer (GetFtpCommand() splits them) */
static void status_answer(Connection *cn)
{
	char temp[520];
	int i;

	for (i = 0; i < cn->num_lines; i++) {
		caf_strncpy(temp, "ANSWER: ", sizeof(temp));
		caf_strncat(temp, &cn->cmd_buffer.ptr[cn->lines_start[i]], sizeof(temp));
		gui_status(temp);
	}
}

/* ---------------------------------------------------------------------------
 * OS 1.3 replacements
 * ------------------------------------------------------------------------- */

/* AllocDosObject(DOS_FIB) is dos V36: AllocMem() memory is longword aligned
   as a FileInfoBlock requires */
static struct FileInfoBlock *AllocFIB(void)
{
	return (struct FileInfoBlock *) AllocMem(sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
}

static void FreeFIB(struct FileInfoBlock *fib)
{
	if (fib)
		FreeMem(fib, sizeof(struct FileInfoBlock));
}

/* GetSysTime() is timer.device V36: TR_GETSYSTIME works on every Kickstart */
static void SysTime(ULONG *secs, ULONG *micro)
{
	if (!g_timer_open) {
		*secs = *micro = 0;
		return;
	}
	g_TimerIO->tr_node.io_Command = TR_GETSYSTIME;
	DoIO((struct IORequest *) g_TimerIO);
	*secs = g_TimerIO->tr_time.tv_secs;
	*micro = g_TimerIO->tr_time.tv_micro;
}

void StartTimer(TimerVal *tv)
{
	SysTime(&tv->start_s, &tv->start_us);
	tv->cur_s = tv->start_s;
	tv->cur_us = tv->start_us;
}

void UpdateTimer(TimerVal *tv)
{
	SysTime(&tv->cur_s, &tv->cur_us);
}

/* Milliseconds between StartTimer() and the last UpdateTimer() */
ULONG ElapsedMs(TimerVal *tv)
{
	LONG s = (LONG) (tv->cur_s - tv->start_s);
	LONG us = (LONG) tv->cur_us - (LONG) tv->start_us;

	if (s < 0)
		return 0;
	return (ULONG) s * 1000UL + us / 1000;
}

/* Seconds from StartTimer() to now */
int TimeInSecs(TimerVal *tv)
{
	ULONG s, us;

	SysTime(&s, &us);
	return (int) (s - tv->start_s);
}

/* DateToStr() is dos V36: "dd-Mmm-yy" (FORMAT_DOS) from the DateStamp days */
void DateStampToStr(char *dest, struct DateStamp *ds)
{
	static const char *months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
									  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	static const UBYTE mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	LONG days = ds->ds_Days;		/* days since 1-Jan-1978 */
	int year = 1978, month = 0, dim;

	if (days < 0)
		days = 0;
	for (;;) {
		int ylen = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365;
		if (days < ylen)
			break;
		days -= ylen;
		year++;
	}
	for (month = 0; month < 12; month++) {
		dim = mdays[month];
		if (month == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
			dim = 29;
		if (days < dim)
			break;
		days -= dim;
	}
	sprintf(dest, "%02ld-%s-%02d", (long) days + 1, months[month], year % 100);
}

/* ---------------------------------------------------------------------------
 * File lists. The GUIs show entries[] as they are: ".." first, then the
 * directories, then the files.
 * ------------------------------------------------------------------------- */
static void view_reset(FileView *v)
{
	BufferOpen(&v->ListBuffer, true);
	v->count = 0;
}

static ViewColumn *view_add(FileView *v, char *name, char *size, char *flags, char *date, bool is_dir)
{
	ViewColumn vc;

	vc.name = BufferAddStr(&v->ListBuffer, name);
	vc.size = BufferAddStr(&v->ListBuffer, size);
	vc.flags = BufferAddStr(&v->ListBuffer, flags);
	vc.date = BufferAddStr(&v->ListBuffer, date);
	vc.is_dir = is_dir;
	if (!vc.name || !vc.size || !vc.flags || !vc.date)
		return 0;
	return (ViewColumn *) BufferAddStruct(&v->ListBuffer, &vc, sizeof(ViewColumn));
}

/* Directories go straight into entries[], files wait in g_files_tmp[] */
static bool view_collect(FileView *v, ViewColumn *vc, int *nfiles)
{
	if (vc->is_dir) {
		if (v->count >= MAX_VIEW_ENTRIES - 1)
			return false;
		v->entries[v->count++] = vc;
	} else {
		if (*nfiles >= MAX_VIEW_ENTRIES - 1)
			return false;
		g_files_tmp[(*nfiles)++] = vc;
	}
	return true;
}

static void view_finish(FileView *v, int nfiles, int which)
{
	int i;

	for (i = 0; i < nfiles && v->count < MAX_VIEW_ENTRIES; i++)
		v->entries[v->count++] = g_files_tmp[i];
	BufferClose(&v->ListBuffer);
	gui_set_list(which, v->entries, v->count);
}

/* Local directory listing of cn->lv.CurrentPath */
bool RefreshLocalView(Connection *cn)
{
	struct FileInfoBlock *fib;
	FileView *lv = &cn->lv;
	ViewColumn *vc;
	char size[32], flags[32], date[32], temp[600];
	int nfiles = 0, lastchar;
	bool b_ret = true;
	BPTR lock;

	fib = AllocFIB();
	if (!fib) {
		gui_status("LOCAL: out of memory, cannot retrieve directory listing");
		return false;
	}

	lock = Lock(lv->CurrentPath, ACCESS_READ);
	if (!lock) {
		snprintf(temp, sizeof(temp), "LOCAL: cannot lock '%s'", lv->CurrentPath);
		gui_status(temp);
		FreeFIB(fib);
		return false;
	}

	if (Examine(lock, fib) == FALSE) {
		gui_status("LOCAL: Examine() failed, cannot get directory listing");
		b_ret = false;
	} else if (fib->fib_DirEntryType < 0) {
		snprintf(temp, sizeof(temp), "LOCAL: '%s' is not a directory", lv->CurrentPath);
		gui_status(temp);
		b_ret = false;
	} else {
		/* The path always ends with ':' or '/' */
		lastchar = caf_getlastchar(lv->CurrentPath);
		if (lastchar != '/' && lastchar != ':')
			caf_strncat(lv->CurrentPath, "/", sizeof(lv->CurrentPath));
		gui_set_path(GUI_LOCAL, lv->CurrentPath);

		view_reset(lv);
		vc = view_add(lv, "..", "<DIR>", "RWED", "N/A", true);
		if (vc)
			lv->entries[lv->count++] = vc;

		while (ExNext(lock, fib) != DOSFALSE) {
			bool is_dir = fib->fib_DirEntryType > 0;
			sprintf(size, "%ld", (long) fib->fib_Size);
			DecodeBitFlags(flags, fib->fib_Protection);
			DateStampToStr(date, &fib->fib_Date);
			vc = view_add(lv, fib->fib_FileName, is_dir ? "<DIR>" : size, flags, date, is_dir);
			if (!vc || !view_collect(lv, vc, &nfiles)) {
				gui_status("ERROR: Local listview buffer full, cannot continue directory scan");
				break;
			}
		}
		view_finish(lv, nfiles, GUI_LOCAL);
	}

	UnLock(lock);
	FreeFIB(fib);
	return b_ret;
}

/* Remote directory listing: parse the LIST data (UNIX "ls -l" format) */
/* ---------------------------------------------------------------------------
 * LIST parsing. Servers answer in different formats, all of them are
 * handled here:
 *   Unix  : drwxr-xr-x 2 user group 4096 Nov 13 10:00 name    (9 fields)
 *   Unix  : -rw-r--r-- 1 user 1234 Nov 13 10:00 name          (no group)
 *   DOS   : 11-13-26  03:00PM       <DIR>          name        (IIS)
 *   DOS   : 11-13-2026  15:00       1234 name
 * "total N" lines, "." and ".." are skipped; for symbolic links
 * ("name -> target") only the name is kept and they are shown as directories
 * (on web hostings they usually point to directories).
 * ------------------------------------------------------------------------- */
static bool is_blank(char c)
{
	return (bool) (c == ' ' || c == '\t');
}

/* Offset of field n (1 based) of line, -1 if the line has fewer fields */
static int field_start(const char *line, int n)
{
	int i = 0, f = 0;

	for (;;) {
		while (is_blank(line[i]))
			i++;
		if (!line[i])
			return -1;
		if (++f == n)
			return i;
		while (line[i] && !is_blank(line[i]))
			i++;
	}
}

static bool all_digits(const char *s)
{
	if (!*s)
		return false;
	for (; *s; s++)
		if (*s < '0' || *s > '9')
			return false;
	return true;
}

static bool is_month(const char *s)
{
	static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
	int i;

	if (strlen(s) != 3)
		return false;
	for (i = 0; i < 36; i += 3)
		if (!strnicmp(s, &months[i], 3))
			return true;
	return false;
}

/* Unix permission string: "drwxr-xr-x", "-rw-r--r--", "lrwxrwxrwx", ... */
static bool is_unix_flags(const char *s)
{
	return (bool) (strlen(s) >= 10 && strchr("-dlbcps", s[0]) != NULL
				   && strchr("r-", s[1]) != NULL && strchr("w-", s[2]) != NULL);
}

/* DOS date: 11-13-26 or 11-13-2026 */
static bool is_dos_date(const char *s)
{
	int n = strlen(s);

	return (bool) ((n == 8 || n == 10) && s[2] == '-' && s[5] == '-'
				   && s[0] >= '0' && s[0] <= '9' && s[3] >= '0' && s[3] <= '9');
}

void ProcessDirectoryData(Connection *cn)
{
	char f[8][32], temp[512], name[512];
	ClientInfo *ci = &cn->ci;
	FileView *rv = &cn->rv;
	ViewColumn *vc;
	int i, k, pos, nfiles = 0, skipped = 0;
	char *line, *first_unknown = NULL, *flags, *size, *arrow;
	bool is_dir;

	if (ci->b_directory_processed == true)
		return;
	if (ci->b_file_transfer != false || ci->b_transfered != true)
		return;

	FormatFtpListing(cn, &cn->transfer_buffer, true);
	view_reset(rv);
	vc = view_add(rv, "..", "<DIR>", "RWED", "N/A", true);
	if (vc)
		rv->entries[rv->count++] = vc;

	for (i = 0; i < cn->num_lines; i++) {
		line = &cn->transfer_buffer.ptr[cn->lines_start[i]];
		for (k = 0; k < 8; k++)
			f[k][0] = 0;
		sscanf(line, "%31s %31s %31s %31s %31s %31s %31s %31s", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
		if (!f[0][0] || !stricmp(f[0], "total"))
			continue;

		pos = -1;
		flags = size = "";
		is_dir = false;
		temp[0] = 0;
		if (is_unix_flags(f[0])) {
			flags = f[0];
			is_dir = (bool) (f[0][0] == 'd' || f[0][0] == 'l');
			if (all_digits(f[4]) && is_month(f[5])) {			/* with group */
				pos = field_start(line, 9);
				size = f[4];
				snprintf(temp, sizeof(temp), "%s %s %s", f[5], f[6], f[7]);
			} else if (all_digits(f[3]) && is_month(f[4])) {	/* without group */
				pos = field_start(line, 8);
				size = f[3];
				snprintf(temp, sizeof(temp), "%s %s %s", f[4], f[5], f[6]);
			}
		} else if (is_dos_date(f[0])) {
			is_dir = (bool) !stricmp(f[2], "<DIR>");
			size = f[2];
			pos = field_start(line, 4);
			snprintf(temp, sizeof(temp), "%s %s", f[0], f[1]);
		}
		if (pos < 0) {
			if (!first_unknown)
				first_unknown = line;
			skipped++;
			continue;
		}

		caf_strncpy(name, &line[pos], sizeof(name));
		if (flags[0] == 'l' && (arrow = strstr(name, " -> ")) != NULL)
			*arrow = 0;									/* symbolic link: name only */
		k = strlen(name);
		while (k > 0 && is_blank(name[k - 1]))
			name[--k] = 0;
		if (!name[0] || !strcmp(name, ".") || !strcmp(name, ".."))
			continue;

		vc = view_add(rv, name, is_dir ? "<DIR>" : size, flags, temp, is_dir);
		if (!vc || !view_collect(rv, vc, &nfiles)) {
			gui_status("ERROR: Remote listview buffer full, cannot continue directory scan");
			break;
		}
	}
	view_finish(rv, nfiles, GUI_REMOTE);
	ci->b_directory_processed = true;

	/* lines in a format we do not know: show the first one, so the problem
	   can be seen without a debug build */
	if (first_unknown) {
		snprintf(temp, sizeof(temp), "STATUS: %d LIST line(s) in an unknown format, first: %.200s", skipped, first_unknown);
		gui_status(temp);
	}
}

void SetConnected(Connection *cn, bool b_con)
{
	cn->ci.b_connected = b_con;
	if (b_con)
		SentStackPush("CONN");
	gui_set_connected(b_con);
}


/* ---------------------------------------------------------------------------
 * Protocol, sockets, queue (from the original main.c)
 * ------------------------------------------------------------------------- */

void HandleDownload(Connection *cn)
{
	int res = 0, local_errno;
	ClientInfo *ci = &cn->ci;
	char temp[512];
	
	#ifdef DEBUG
	FILE *fp;
	#endif
	
	/******************************************* DOWNLOAD ****************************************/
	// Transfer socket event happened while we are transfering, 
	// we have received data
	if (ci->b_file_transfer == false) {
		#ifdef DEBUG
		DebugOutput("MainLoop(): DIRECTORY TRANSFER\n");
		#endif
		
		// Keep always 1 byte free for the terminator used by FormatFtpListing()
		if (ci->bytes_transfered+RECV_CHUNK > cn->transfer_buffer.size-1) {
			#ifdef DEBUG
			DebugOutput("HandleDownload(): directory listing bigger than the transfer buffer, sending ABORT\n");
			#endif
			AbortFileTransfer(cn);
			// SendFtpCommand(cn, "ABOR", false);
			
			// The old code called recv() anyway, writing up to 64KB past the
			// end of the transfer buffer (heap corruption on huge listings)
			gui_status("ERROR: Directory listing too big, transfer aborted");
			if (ci->transfer_socket != INVALID_SOCKET) {
				CloseDataSocket(cn);
				InvalidateSocket(&ci->transfer_socket);
			}
			ClearTransferFlags(cn);
			return;
		}
		
		res = recv(ci->transfer_socket, &cn->transfer_buffer.ptr[ci->bytes_transfered], RECV_CHUNK, 0);
		if (res > 0)
			cn->transfer_buffer.ptr[ci->bytes_transfered+res] = 0;
	} else {
		#ifdef DEBUG
		DebugOutput("MainLoop(): FILE TRANSFER\n");
		#endif
		
		// If we are transfering a file, we do not need a big buffer, stuff will be
		// written to disk
		res = recv(ci->transfer_socket, cn->transfer_buffer.ptr, RECV_CHUNK, 0);
	}
	
	if (res > 0) {
		#ifdef DEBUG
		snprintf(temp, 511, "MainLoop(): recv() ha ricevuto '%d' bytes\n", res);
		DebugOutput(temp);
		#endif
		cn->ci.b_last_data = false;
		StartTimer(&g_last_packet_transfered_timer);
		if (ci->b_file_transfer && ci->filehandle) {
			if (Write(ci->filehandle, cn->transfer_buffer.ptr, res) != res) {
				local_errno = IoErr();
				ci->b_file_transfer = false;
				Close(ci->filehandle);			// the handle was leaked
				ci->filehandle = 0;
				AbortFileTransfer(cn);
				// SendFtpCommand(cn, "ABOR", false);
				sprintf(temp, "STATUS: Transfer failed, '%d' error during file write", local_errno);
				gui_status(temp);
				cn->ci.b_last_data = true;
			}
		}
		
		#ifdef DEBUG
		fp = fopen("ram:dump.txt", "wb");
		if (fp) {
			fwrite(cn->transfer_buffer.ptr, 1, res, fp);
			fclose(fp);
		}
		#endif
		
		ci->bytes_transfered += res;
	} else {
		cn->ci.b_last_data = true;
		if (res == SOCKET_ERROR) {
			#ifdef DEBUG
			DebugOutput("MainLoop(): res == SOCKET_ERROR, closing transfer socket\n");
			#endif
		} else if (res == 0) {
			#ifdef DEBUG
			DebugOutput("MainLoop(): res is 0\n");
			#endif
			StartTimer(&g_last_packet_transfered_timer);
		} else
			gui_status("STATUS: I SHOULD NOT BE HERE!");
	}
	
	if (cn->ci.b_last_data) {
		CloseDataSocket(cn);
		gui_status("STATUS: Remote closed transfer socket");
		cn->ci.b_transfered = true;
		ClearTransferFlags(cn);
		#ifdef DEBUG
		snprintf(temp, 511, "MainLoop(): recv_command_stack[STACK_CURRENT] = %d\n", recv_command_stack[STACK_CURRENT]);
		DebugOutput(temp);
		#endif
		if (IsLastSentCmd("LIST")) {
			// Last sent was directory listing
			if (GetCommandDigit(recv_command_stack[STACK_CURRENT], 1) == 2) {
				ProcessDirectoryData(cn);
				#ifdef DEBUG
				DebugOutput("MainLoop(): called ProcessDirectoryData() from g_b_last_data if()\n");
				#endif
			}
		} else if (IsLastSentCmd("RETR")) {
			// Last sent was RETR
			if (GetCommandDigit(recv_command_stack[STACK_CURRENT], 1) == 2)
				AfterRETR(cn);
		}
	}
}

void HandleUpload(Connection *cn)
{
	int res = 0, local_errno;
	ClientInfo *ci = &cn->ci;
	char temp[512];
	
	/******************************************* UPLOAD ****************************************/
	// Transfer socket event happened, we can send data
	
	/* UPLOAD */
	local_errno = 0;
	if (ci->file_readed == 0) {
		ci->file_readed = Read(ci->filehandle, cn->transfer_buffer.ptr, RECV_CHUNK);
		if (ci->file_readed == 0)
			ci->b_eof = true;

		local_errno = IoErr();
		ci->file_sent = 0;
	}
	
	if (ci->file_readed == -1) {
		ci->b_file_transfer = false;
		if (ci->filehandle)
			Close(ci->filehandle);
		ci->filehandle = 0;
		
		AbortFileTransfer(cn);
		// SendFtpCommand(cn, "ABOR", false);
		sprintf(temp, "ERROR: Transfer failed, '%d' error during file read", local_errno);
		gui_status(temp);
		res = -1;
	} else {
		if (ci->file_readed) {
			res = send(ci->transfer_socket, &cn->transfer_buffer.ptr[ci->file_sent], ci->file_readed, 0);
			if (res > 0) {
				ci->file_readed -= res;
				ci->file_sent += res;
				ci->bytes_transfered += res;
				StartTimer(&g_last_packet_transfered_timer);
			} else {
				/* SOCKET ERROR */
			}
		}
	}
	
	if (ci->b_eof == true || res == 0) {
		// EOF reached
		if (ci->filehandle)
			Close(ci->filehandle);
		ci->filehandle = 0;

		ci->b_file_transfer = false;
		CloseDataSocket(cn);
		cn->ci.b_transfered = true;
	} else if (res < 0) {
		if (ci->filehandle)
			Close(ci->filehandle);
		ci->filehandle = 0;

		ci->b_file_transfer = false;
		CloseDataSocket(cn);
	}
}

void DisplaySockErrs(Connection *cn, CSOCK *csock)
{
	char temp[512];
	
	switch (csock->csockerr) {
		case CSOCKERR_SETNONBLOCKFAILED:
			// Was gui_status(...): the listview object
			// was used as a Connection pointer -> crash
			gui_status("ERROR: SetBlocking() failed, cannot change socket status");
			break;
				
		case CSOCKERR_SOCKCREATFAILED:
			gui_status("ERROR: Socket creation error");
			break;
				
		case CSOCKERR_BINDFAILED:
			gui_status("ERROR: Socket bind() failed");
			break;
				
		case CSOCKERR_WRONGPARAMS:
			gui_status("ERROR: Please double click a site from the address book");
			break;
				
		case CSOCKERR_NATIVE:
			// bsdsocket error codes are BSD ones, not the C library ones
			snprintf(temp, 511, "ERROR: socket error %d '%s'", csock->sockerr, GetErrnoDesc(csock->sockerr));
			gui_status(temp);
			break;
	}
}

bool FtpConnect(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	bool b_retflag = false;
	HostInfo *hi = &cn->hi;
	struct hostent *ent;
	char temp[512];
	CSOCK csock;
	int ret;
	
	if (cn->ci.b_connected == true) {
		#ifdef DEBUG
		DebugOutput("FtpConnect(): Already connected!\n");
		#endif
		return false;
	}
	
	// Resolve site hostname
	ent = gethostbyname(hi->hostname);
	if (ent) {
		CopyMem(ent->h_addr, &cn->hi.ip_i, sizeof(int));
	} else {
		cn->hi.ip_i = inet_addr(hi->hostname);
	}
		
	// 
	ret = cn->hi.ip_i;
	
	#ifdef CPU_LITTLE_ENDIAN
	BSwapDWORD(&ret, &ret);
	#endif
	
	if (ret != 0) {
		snprintf(temp, 511, "STATUS: Connecting to %s (%s)", hi->hostname, IpToStr(ret, '.'));
		gui_status(temp);
	}
	
	csock.ip_i = cn->hi.ip_i;
	csock.port = cn->hi.port;
	ret = SockConnect(&csock);
	ci->cmd_socket = csock.socket;		// INVALID_SOCKET on error
	if (ret != CSOCKSTATUS_ERROR) {
		// If we already connected, we need to get ConnectionInfo here
		if (ret == CSOCKSTATUS_EARLYCONNECT) {
			AcknowledgeConnection(cn);
		} else {
			cn->ci.b_connecting = true;
			StartTimer(&g_trying_connect_timer);	// was never started
		}
		
		ci->cmd_socket = csock.socket;
		b_retflag = true;
	} else {
		DisplaySockErrs(cn, &csock);
	}
	
	return b_retflag;
}

int SockConnect(CSOCK *csock)
{
	int ret, status, sock = 1;
	// "socket" as variable name hid the socket() function: fine with the
	// AROS/MorphOS inline macros, a compile error with SAS/C pragmas
	struct sockaddr_in sockaddr;
	
	csock->socket = INVALID_SOCKET;
	csock->csockerr = 0;
	csock->sockerr	= 0;
	
	if (csock->ip_i == 0 || csock->port == 0) {
		csock->csockerr = CSOCKERR_WRONGPARAMS;
		return CSOCKSTATUS_ERROR;
	}
	
	#ifdef DEBUG
	DebugOutput("SockConnect()\n");
	#endif
	
	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock != INVALID_SOCKET)
	{
		#ifdef DEBUG
		printf("SockConnect(): socket = %d\n", sock);
		DebugOutput("SockConnect(): Socket created\n");
		#endif
		csock->socket = sock;
		
		/* server address  */
		sockaddr.sin_family = AF_INET;
		sockaddr.sin_addr.s_addr = INADDR_ANY;
		sockaddr.sin_port = 0;
		
		status = bind(sock, (struct sockaddr *) &sockaddr, sizeof(struct sockaddr_in));
		if (status != INVALID_SOCKET)
		{
			#ifdef DEBUG
			DebugOutput("SockConnect(): Socket binded\n");
			#endif
			
			if (SetBlocking(sock, false) == false) {
				// If SetBlocking fails, we can't continue because later
				// code is made for non blocking sockets
				csock->csockerr = CSOCKERR_SETNONBLOCKFAILED;
				InvalidateSocket(&csock->socket);
				return CSOCKSTATUS_ERROR;
			}
			
			#ifdef DEBUG
			DebugOutput("SockConnect(): Socket set to non blocking\n");
			printf("SockConnect() connecting to %s\n", IpToStr(csock->ip_i, '.'));
			#endif
			
			sockaddr.sin_addr.s_addr = csock->ip_i;
			sockaddr.sin_family = AF_INET;
			sockaddr.sin_port = htons(csock->port);
			
			#ifdef DEBUG
			DebugOutput("SockConnect(): Trying to connect..\n");
			#endif
			
			ret = connect(sock, (struct sockaddr *) &sockaddr, sizeof(struct sockaddr_in));
			if (ret == -1) {
				int local_errno = errno;
				
				#ifdef DEBUG
				printf("SockConnect(): connect() = %d\n", ret);
				printf("SockConnect(): Errno = %d\n", local_errno);
				#endif
				
				#if defined(__AROS__)
				// ASD: Should not local_errno be EINPROGRESS ?...
				if (local_errno == 0 || local_errno == EINPROGRESS)
					return CSOCKSTATUS_PENDING;
				#elif defined(__MORPHOS__)
				if (local_errno == EINPROGRESS)
					return CSOCKSTATUS_PENDING;
				#else
				// AmigaOS 3.x: errno is the BSD one (see SetErrnoPtr() in OpenLibs())
				if (local_errno == CAF_EINPROGRESS || local_errno == CAF_EWOULDBLOCK)
					return CSOCKSTATUS_PENDING;
				#endif
				
				// Was "sockerr = ret" (always -1) and the socket was leaked
				csock->csockerr = CSOCKERR_NATIVE;
				csock->sockerr	= local_errno;
				InvalidateSocket(&csock->socket);
				return CSOCKSTATUS_ERROR;
			} else if (ret == 0) {
				// Connect already connected us to the ftp server
				#ifdef DEBUG
				DebugOutput("SockConnect(): Already accepted a connection\n");
				#endif
				
				csock->socket = sock;
				return CSOCKSTATUS_EARLYCONNECT;
			} else {
				csock->csockerr = CSOCKERR_NATIVE;
				csock->sockerr	= errno;
				InvalidateSocket(&csock->socket);
				return CSOCKSTATUS_ERROR;
			}
		} else {
			csock->csockerr = CSOCKERR_BINDFAILED;
			InvalidateSocket(&csock->socket);
		}
	} else
		csock->csockerr = CSOCKERR_SOCKCREATFAILED;
	
	return CSOCKSTATUS_ERROR;
}

bool CheckSocket(SOCKET socket)
{
	int ret, optlen;
	#ifdef DEBUGLV2
	char temp[512];
	#endif
	
	// Let's check SO_ERROR
	optlen = sizeof(ret);
	if (getsockopt (socket, SOL_SOCKET, SO_ERROR, (void *) &ret, (void *) &optlen) != SOCKET_ERROR) {
		if (ret == 0) {
			#ifdef DEBUGLV2
			DebugOutput("CheckSocket(): getsockopt() no error\n");
			#endif
			// No error
			return true;
		}
		
		#ifdef DEBUGLV2
		snprintf(temp, 511, "\nCheckSocket(): Error code(ret) = %d Error code(hex(ret)) %08x string = '%s'\n", ret, ret, GetErrnoDesc(ret));
		DebugOutput(temp);
		#endif
		return true;
	} 
	#ifdef DEBUG
	else
		DebugOutput("CheckSocket(): getsockopt() failed\n");
	#endif
	
	// Errors
	return false;
}

bool CheckConnect(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	HostInfo   *hi = &cn->hi;
	bool b_retflag = false;
	int ret, optlen;
	char temp[512];
	
	// Socket has returned Writeable, but we still do not known if connect worked
	// we need to check SO_ERROR as well
	optlen = sizeof(ret);
	getsockopt (ci->cmd_socket, SOL_SOCKET, SO_ERROR, (void *) &ret, (void *) &optlen);
	if (ret == 0) {
		snprintf(temp, 511, "STATUS: Connected to %s", hi->hostname);
		gui_status(temp);
		b_retflag = true;
	} else {
		snprintf(temp, 511, "STATUS: Connection failed, error: '%s'", GetErrnoDesc(ret));
		gui_status(temp);
		#ifdef DEBUG
		DebugOutput("CheckConnect(): Connection failed, ");
		DebugOutput(GetErrnoDesc(ret));
		DebugOutput("\n");
		#endif
	}
	
	cn->ci.b_connecting = false;
	SetConnected(cn, b_retflag);
	return b_retflag;
}

void FormatFtpListing(Connection *cn, buffer *buffer, bool b_last_data)
{
	ClientInfo *ci = &cn->ci;
	int i, start_of_line = 0;
	char *dest = buffer->ptr;
	#ifdef DEBUG
	char temp[512];
	#endif
	
	// First line always start at 0, of course
	cn->lines_start[0] = 0;
	cn->num_lines = 0;
	
	// i is checked before reading dest[i]
	for (i = 0; i < ci->bytes_transfered && dest[i] != 0; i++) {
		if (dest[i] == '\r') {
			dest[i] = 0;
			continue;
		}
		
		if (dest[i] == '\n') {
			// Terminate this line
			dest[i] = 0;
			
			// Add the start of the line in the lines database
			// (lines_start[] has 16384 entries, it was never checked)
			if (cn->num_lines >= (int)(sizeof(cn->lines_start)/sizeof(cn->lines_start[0])))
				break;
			
			cn->num_lines++;
			cn->lines_start[cn->num_lines-1] = start_of_line;
			start_of_line = 1+i;
		}
	}
	
	/* last line without a final newline (some servers): keep it as well */
	if (start_of_line < i && cn->num_lines < (int)(sizeof(cn->lines_start)/sizeof(cn->lines_start[0]))) {
		if (i < buffer->size)
			dest[i] = 0;
		cn->num_lines++;
		cn->lines_start[cn->num_lines-1] = start_of_line;
	}
	
	#ifdef DEBUG
	snprintf(temp, 511, "FormatFtpListing(): parsed %d bytes", i);
	DebugOutput(temp);
	#endif
}

void GetPathFromRawData(char *text)
{
	char *dest = g_temp;
	bool b_copy = false;
	int i, n = 0;
	
	// g_temp is 1024 bytes, the server answer can be up to 4KB
	for (i = 0; text[i] != 0; i++) {
		if (b_copy && text[i] != '"' && n < (int) sizeof(g_temp)-1) {
			*dest++ = text[i];
			n++;
		}
		
		if (text[i] == '"') {
			if (b_copy == false)
				b_copy = true;
			else
				b_copy = false;
		}
	}
	
	*dest++ = 0;
}

int FtpDisconnect(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	// Disconnecting...
	if (ci->b_transfering) {
		// If we are transfering a file, let's abort it
		AbortFileTransfer(cn);
		// SendFtpCommand(cn, "ABOR", false);
		ci->b_request_disconn = true;
	} else {
		// Otherwise, just quit
		SendFtpCommand(cn, "QUIT", false);
	}
	
	#ifdef DEBUG
	DebugOutput("FtpDisconnect(): disconnected\n");
	#endif
	
	return 0;
}

void InvalidateSocket(int *socket)
{
	#ifdef DEBUG
	char temp[512];
	sprintf(temp, "InvalidateSocket(): Closing socket %d\n", *socket);
	DebugOutput(temp);
	#endif
	
	if (*socket != INVALID_SOCKET) {
		shutdown(*socket, 0);
		CloseSocket(*socket);
	}
	
	*socket = INVALID_SOCKET;
}

int SetTransferMode(Connection *cn, char mode)
{
	char cmd[512], t2[3];
	int answer;
	
	t2[0] = mode;	t2[1] = 0;
	caf_strncpy(cmd, "TYPE ", 511);
	caf_strncat(cmd, t2, 511);
	answer = SendFtpCommand(cn, cmd, false);
	return answer;
}

int SetTransferPort(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	int answer, port;
	char cmd[512];
	
	port = ci->port;
	snprintf(cmd, 511, "PORT %s,%02d,%02d", ci->ip_s, (port >> 8)&0xFF, port & 0xFF);
	answer = SendFtpCommand(cn, cmd, false);
	ci->b_passive = false;
	return answer;
}

int SendLISTCMD(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	ci->b_file_transfer = false;
	cn->ci.b_directory_processed = false;
	cn->ci.b_last_data = false;
	#ifdef DEBUG
	DebugOutput("SendLISTCMD(): b_directory_processed = false\n");
	#endif
	cn->rv.count = 0;
	gui_set_list(GUI_REMOTE, cn->rv.entries, 0);
	return SendFtpCommand(cn, "LIST", false);
}

int SendRETRCMD(Connection *cn, char *filename)
{
	ClientInfo *ci = &cn->ci;
	char temp[512];
	IPTR itemp;
	int chr;
	
	// Open the file on disk, in the local directory shown by the GUI
	itemp = (IPTR) cn->lv.CurrentPath;
	
	chr = caf_getlastchar((char *) itemp);
	if (chr == '/' || chr == ':')
		snprintf(temp, 511, "%s%s", (char *) itemp, filename);
	else
		snprintf(temp, 511, "%s/%s",(char *) itemp, filename);
	
	ci->filehandle = Open(temp, MODE_NEWFILE);
	if (ci->filehandle == 0) {
		ci->b_file_transfer = false;
		cn->ci.b_last_data = false;
		gui_status("ERROR: Cannot create the local file");
		return 0;
	}
	
	ci->b_eof = false;
	ci->b_file_transfer = true;
	cn->ci.b_last_data = false;
	snprintf(temp, 511, "RETR %s", filename);
	#ifdef DEBUGLV2
	DebugOutput(temp);
	DebugOutput("SendRETRCMD\n");
	#endif
	return SendFtpCommand(cn, temp, false);
}

int SendSTORCMD(Connection *cn, char *filename)
{
	char temp[512], temp2[512];
	ClientInfo *ci = &cn->ci;
	int chr, ioerr;
	IPTR itemp;
	
	// Open the file on disk
	// While a queue runs the local directory is the one of the queue
	// (LocalBasePath()), otherwise the one shown by the GUI
	if (cn->ci.b_processing_queue == true)
		itemp = (IPTR) g_QueueLocalPath;
	else
		itemp = (IPTR) cn->lv.CurrentPath;
	
	chr = caf_getlastchar((char *) itemp);
	if (chr == '/' || chr == ':')
		snprintf(temp, 511, "%s%s", (char *) itemp, filename);
	else
		snprintf(temp, 511, "%s/%s",(char *) itemp, filename);
	
	ci->file_readed = ci->file_size = ci->file_sent = 0;
	ci->filehandle = Open(temp, MODE_OLDFILE);
	if (ci->filehandle == 0) {
		ioerr = IoErr();
		ci->b_file_transfer = false;
		/* Fault() is dos V36: the error number is printed instead */
		snprintf(temp2, 511, "ERROR: Cannot open local file '%s', Open() failed, error %d", temp, ioerr);
		gui_status(temp2);
		CloseListenSocket(cn);
		CloseDataSocket(cn);
		return 0;
	}
	
	ci->b_eof = false;
	ci->b_file_transfer = true;
	ci->b_file_download = false;
	cn->ci.b_last_data = false;
	snprintf(temp, 511, "STOR %s", filename);
	#ifdef DEBUGLV2
	DebugOutput(temp);
	DebugOutput("SendSTORCMD\n");
	#endif
	return SendFtpCommand(cn, temp, false);
}

int SendPWDCMD(Connection *cn)
{
	return SendFtpCommand(cn, "PWD", false);
}

int SendCWDCMD(Connection *cn, char *path)
{
	char temp[511];
	
	snprintf(temp, 511, "CWD %s", path);
	return SendFtpCommand(cn, temp, false);
}

int GetFtpCommandValue(char *buffer)
{
	char command[32];
	int slen, numba;
	
	slen = caf_strlen(buffer);
	command[0] = 0;
	
	// The buffer string must be at lest four bytes
	if (slen < 4)
		// Cannot get the command, string too short
		return 0;
	
	// Copy the command to the receiving string
	command[0] = buffer[0];
	command[1] = buffer[1];
	command[2] = buffer[2];
	command[3] = buffer[3];
	command[4] = 0;
	
	numba = caf_atol(command);
	if (command[3] == '-')
		numba = -numba;
	
	return numba;
}

int GetFtpCommand(struct buffer *buffer, Connection *cn)
{
	bool need_break_after_newline = false, done = false, b_closed = false;
	int readed = 0, linestart = 0, command = 0, ret = -1;
	ClientInfo *ci = &cn->ci;
	
	// First line always start at 0, of course
	cn->lines_start[0] = 0;
	cn->num_lines = 1;
	
	caf_memset(buffer->ptr, 0, buffer->size);
	
//	while ((ret = ReadSocketNB(&buffer->ptr[readed], ci->cmd_socket, 1, g_GlobalSettings.socket_timeout, 0)) == true) {
	while (CanReadSocket(ci->cmd_socket, 0, 0) == SOCKET_SELECT_OK) {
		ret = recv(ci->cmd_socket, &buffer->ptr[readed], 1, 0);
		if (ret <= 0) {
			// Connection closed or error: the old code went on looping
			// (the socket stays readable) until the buffer was full
			buffer->ptr[readed] = 0;
			b_closed = true;
			break;
		}
		
		if (need_break_after_newline == false) {
			command = GetFtpCommandValue(&buffer->ptr[linestart]);
			if (command > 0) {
				// We found a valid command, go out
				need_break_after_newline = true;
			}
		}
		
		if (buffer->ptr[readed] == '\n') {
			// Terminate this line
			buffer->ptr[readed] = 0;
			
			// We found a new line
			if (need_break_after_newline == false) {
				// If the server is still sending data, we add each line in the array
				linestart = 1+readed;
				if (cn->num_lines < (int)(sizeof(cn->lines_start)/sizeof(cn->lines_start[0]))) {
					cn->num_lines++;
					cn->lines_start[cn->num_lines-1] = linestart;
				}
			} else {
				// Server finished sending data
				done = true;
				break;
			}
		}
		
		// Skip carriage return
		if (buffer->ptr[readed] == '\r')
			continue;
		
		readed++;
		if (readed == buffer->size-3)
			break;
	}
	
	if (b_closed) {
		// Command socket has been closed, let's go away
		CleanSockAndFlags(cn);
		gui_status("STATUS: closed control connection");
	}
	
	#ifdef DEBUG
	if (!readed) {
		DebugOutput("GetFtpCommand(): Readed = 0\n");
	}
	
	if (!done) {
		DebugOutput("GetFtpCommand(): Valid command not found\n");
		DebugOutput(buffer->ptr);
	} 
	#endif
	
	return command;
}

int SendFtpCommand(Connection *cn, char *CMD, bool b_block)
{
	ClientInfo *ci = &cn->ci;
	int to_write, written, ret;
	char temp[520];
	
	if (strcmp(CMD, "ABOR")) {
		if (cn->ci.b_can_send_command == false) {
			if (cn->ci.b_connected == true) {
				gui_status(STR_CANT_SEND);
				return ANSWER_PENDING;
			} else {
				gui_status(STR_CANT_SEND_NOT_CONNECTED);
				return NOT_CONNECTED;
			}
		}
	}
	
	// Send Command (the "\r\n" was cut away by the 511 limit for long commands)
	caf_strncpy(temp, CMD, 511);
	status_command(temp);
	caf_strncat(temp, "\r\n", sizeof(temp));
	
	to_write = caf_strlen(temp); 
	written = 0;
	while ((to_write > written) && (CanWriteSocket(ci->cmd_socket, 0, 0) == SOCKET_SELECT_OK)) {
		// send() returning -1 made "written" go backwards (&temp[-1])
		ret = send(ci->cmd_socket, &temp[written], to_write-written, 0);
		if (ret <= 0)
			break;
		written += ret;
	}
	
	SentStackPush(CMD);
	
	// Each command needs to wait for server response
	// ci->b_can_send_command = false;
	return 0;
}

bool ProcessError(Connection *cn, int command)
{
	switch (command) {
		case SELECT_ERROR:
			gui_status("STATUS: cannot send command, select() failed");
			return true;
			
		case ANSWER_PENDING:
			gui_status(STR_CANT_SEND);
			return true;
			
			// General FTP Error
	}
	
	return false;
}

void ProcessCommand(Connection *cn, int command)
{
	ClientInfo *ci = &cn->ci;
	HostInfo *hi = &cn->hi;
	char temp[512], digit;
	bool b_goon;
	int ret;
	
	digit = GetCommandDigit(command, 1);
	switch (digit) {
		case 1:
/*********************************************	DIGIT 1 *****************************************/
			// Positive Preliminary reply, expect another reply before sending commands
			cn->ci.b_can_send_command = false;
			if (IsLastSentCmd("LIST")) {
			} else if (IsLastSentCmd("RETR")) {
			}
			break;
			
		case 2:
/*********************************************	DIGIT 2 *****************************************/
			// Positive Completion reply, we can send commands
			cn->ci.b_can_send_command = true;
			if (IsLastSentCmd("CONN")) {
				caf_strncpy(temp, "user ", 511);	
				caf_strncat(temp, hi->username, 511);
				SendFtpCommand(cn, temp, false);
			} else if (IsLastSentCmd("pass")) {
				cn->ci.b_pass_accepted = true;
				SendPWDCMD(cn);
			} else if (IsLastSentCmd("PWD")) {
				GetPathFromRawData(cn->cmd_buffer.ptr);
				caf_strncpy(cn->rv.CurrentPath, g_temp, sizeof(cn->rv.CurrentPath));
				gui_set_path(GUI_REMOTE, cn->rv.CurrentPath);
				InitiateListTransfer(cn);
			} else if (IsLastSentCmd("LIST") && cn->ci.b_last_data == true) {
				#ifdef DEBUG
				DebugOutput("ProcessCommand(): ProcessDirectoryData() called from 226 case\n");
				#endif
				ProcessDirectoryData(cn);
			} else if (IsLastSentCmd("ABOR") && cn->ci.b_request_disconn == true) {
				SendFtpCommand(cn, "QUIT", false);
			} else if (IsLastSentCmd("STOR")) {
				AfterSTOR(cn);
			} else if (IsLastSentCmd("RETR")) {
				AfterRETR(cn);
			} else if (IsLastSentCmd("DELE")) {
				AfterDELE(cn);
			} else if (IsLastSentCmd("CWD") || IsLastSentCmd("CDUP")) {
				AfterCWD(cn);
			} else if (IsLastSentCmd("MKD")) {
				AfterMKD(cn);
			} else if (IsLastSentCmd("ABOR") && cn->ci.b_request_disconn == true) {
				SendFtpCommand(cn, "QUIT", false);
			} else if (IsLastSentCmd("QUIT")) {
				CleanSockAndFlags(cn);
				if (cn->ci.b_request_quit)
					g_quit_now = true;
			} else if (IsLastSentCmd("PORT") || IsLastSentCmd("PASV")) {
				b_goon = true;
				if (hi->conn_type == CTYPE_PASV) {
					#ifdef DEBUG
					printf("conn_type = CTYPE_PASV\n");
					#endif
					
					// Gets detail for passive connection
					if (GatherIpPortNumber(cn) != 0) {
						// Tries to connect to specified ip - port
						// ASD
						ret = PasvConnection(cn);
						if (ret == CSOCKSTATUS_EARLYCONNECT) {
							// connect() connected at first call
							ci->transfer_socket = cn->hi.pasvsettings.socket;
							ci->b_waitingfordataport = true;
							StartTimer(&g_listen_timer);
							b_goon = true;
							#ifdef DEBUG
							printf("PasvConnection(): CSOCKSTATUS_EARLYCONNECT\n");
							#endif
						} else if (ret == CSOCKSTATUS_PENDING) {
							// connect() is in progress, pending...
							// we have to wait the socket in the select() loop
							ci->transfer_socket = cn->hi.pasvsettings.socket;
							ci->b_waitingfordataport = true;
							StartTimer(&g_listen_timer);
							b_goon = true;
							#ifdef DEBUG
							printf("PasvConnection(): CSOCKSTATUS_PENDING\n");
							#endif
						} else if (ret == CSOCKSTATUS_ERROR) {
							// connect() failed
							DisplaySockErrs(cn, &cn->hi.pasvsettings);
							b_goon = false;
						}
					} else
						b_goon = false;
				}
				
				if (b_goon) {
					if (cn->ci.b_request_list)
						SendLISTCMD(cn);
					else if (cn->ci.b_request_download) {
						if (ci->b_file_download)
							SendRETRCMD(cn, ci->filename);
						else {
							SendSTORCMD(cn, ci->filename);
						}
					}
				}
			} else if (IsLastSentCmd("TYPE")) {
				if (hi->conn_type == CTYPE_ACTV) {
					// Active transfer mode
					SetTransferPort(cn);
				} else {
					// Passive transfer mode
					SendPASVCMD(cn);
				}
			}
			break;
			
		case 3:
			// Positive Intermediate reply
			cn->ci.b_can_send_command = true;
/*********************************************	DIGIT 3 *****************************************/
			if (IsLastSentCmd("user")) {
				cn->ci.b_user_accepted = true;
				caf_strncpy(temp, "pass ", 511);	caf_strncat(temp, hi->password, 511);
				SendFtpCommand(cn, temp, false);
			}
			break;
			
		case 4:
			// Transient Negative Completion reply;
			cn->ci.b_can_send_command = true;
/********************************************* DIGIT 4 *****************************************/
			if (IsLastSentCmd("RETR") || IsLastSentCmd("LIST") || IsLastSentCmd("STOR")) {
				if (ci->port != 0)
					snprintf(temp, 511, "STATUS: Server error encountered, Transfer failed, closed port %d", ci->port); 
				else
					caf_strncpy(temp, "STATUS: Server error encountered, Transfer failed", 512); 
				
				gui_status(temp);
				CloseListenSocket(cn);
				CloseDataSocket(cn);
			} else if (IsLastSentCmd("DELE")) {
			}
			break;
			
		case 5:
			// Permanent Negative Completion reply
			cn->ci.b_can_send_command = true;
/********************************************* DIGIT 5 *****************************************/
			if (IsLastSentCmd("RETR") || IsLastSentCmd("LIST") || IsLastSentCmd("STOR")) {
				snprintf(temp, 511, "STATUS: Server error encountered, file unavailable, closed port %d", ci->port); 
				gui_status(temp);
				CloseListenSocket(cn);
				CloseDataSocket(cn);
			} else if (IsLastSentCmd("DELE")) {
				if (ci->b_processing_queue) {
				}
			}
			break;
	}
	
	#ifdef DEBUGLV2
	sprintf(temp, "ProcessCommand(): CMD(%d) = %s digit = %d\n", command, (ci->b_can_send_command) ? "true" : "false", digit);
	DebugOutput(temp);
	#endif
}

bool OpenListenSocket(Connection *cn)
{
	int status, sock = INVALID_SOCKET, ret;
	struct sockaddr_in sockaddr;
	ClientInfo *ci = &cn->ci;
	bool b_ret = false;
	char temp[512];
	u_long opt;
	
	if (cn->ci.b_waitingfordataport == true) {
		gui_status("WARNING: Already listening for connection...");
		return false;
	}
	
	cn->ci.b_transfering = false;
	ci->bytes_transfered = 0;
	InvalidateSocket(&ci->listen_socket);
	InvalidateSocket(&ci->transfer_socket);
	#ifdef DEBUG
	DebugOutput("OpenListenSocket(): Creating listen socket\n");
	#endif
	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock != INVALID_SOCKET)
	{	
		#ifdef DEBUG
		DebugOutput("OpenListenSocket(): Socket created\n");
		#endif
		
		/* server address  */
		sockaddr.sin_family = AF_INET;
		sockaddr.sin_addr.s_addr = INADDR_ANY;
		sockaddr.sin_port = 0;
		
		status = bind(sock, (struct sockaddr *) &sockaddr, sizeof(struct sockaddr_in));
		if (status != INVALID_SOCKET)
		{
			#ifdef DEBUG
			DebugOutput("OpenListenSocket(): Socket binded\n");
			#endif
			
			// Get the port of the socket 
			status = sizeof(struct sockaddr);
			getsockname(sock, (struct sockaddr *) &sockaddr, (void *) &status);
			ci->port = htons(sockaddr.sin_port);
			
			if (SetBlocking(sock, false) == false) {
				// If SetBlocking fails, we can't continue because later
				// code is made for non blocking sockets
				#ifdef DEBUG
				DebugOutput("OpenListenSocket(): SetBlocking() failed\n");
				#endif
				// Was CleanUp(): it freed every buffer and closed all the
				// libraries while the program was still running -> crash
				CloseSocket(sock);
				ci->listen_socket = INVALID_SOCKET;
				ci->port = 0;
				gui_status("ERROR: SetBlocking() failed on the listen socket");
				return false;
			}
			
			opt = 1;
			if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
				#ifdef DEBUG
				DebugOutput("OpenListenSocket(): Cannot set addr to reuse\n");
				#endif
			} else {
				#ifdef DEBUG
				DebugOutput("OpenListenSocket(): Addr set to reuse\n");
				#endif
			}
			
			// AmiTCP 4.x (4.3BSD) headers have no SO_REUSEPORT
			#if !defined(__AROS__) && defined(SO_REUSEPORT)
			opt = 1;
			if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
				#ifdef DEBUG
				DebugOutput("OpenListenSocket(): Cannot set port to reuse\n");
				#endif
			} else {
				#ifdef DEBUG
				DebugOutput("OpenListenSocket(): Port set to reuse\n");
				#endif
			}
			#endif
			
			// Setting the socket to listen state
			ret = listen(sock, 1);
			if (ret == 0) {
				cn->ci.b_waitingfordataport = true;
				snprintf(temp, 511, "STATUS: Listening on port %d", ci->port);
				gui_status(temp);
				#ifdef DEBUG
				DebugOutput("OpenListenSocket(): Socket listening()....\n");
				#endif
				b_ret = true;
				StartTimer(&g_listen_timer);
			} else {
				cn->ci.b_waitingfordataport = false;
				CloseSocket(sock);
				sock = INVALID_SOCKET;
				#ifdef DEBUG
				DebugOutput("OpenListenSocket(): Listen() failed on data socket\n");
				#endif
				snprintf(temp, 511, "STATUS: failed listening on port %d", ci->port);
				ci->port = 0;
				gui_status(temp);
			}
		}
	}
	
	ci->listen_socket = sock;
	return b_ret;
}

int PasvConnection(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	cn->ci.b_transfering = false;
	ci->bytes_transfered = 0;
	InvalidateSocket(&ci->listen_socket);
	InvalidateSocket(&ci->transfer_socket);
	#ifdef DEBUG
	printf("PasvConnection(): Connecting to %s port %d\n", IpToStr(cn->hi.pasvsettings.ip_i, '.'), cn->hi.pasvsettings.port);
	#endif
	return SockConnect(&cn->hi.pasvsettings);
}

bool CloseListenSocket(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	#ifdef DEBUG
	DebugOutput("CloseListenSocket()\n");
	#endif
	
	InvalidateSocket(&ci->listen_socket);
	cn->ci.b_waitingfordataport = false;
	return true;
}

bool CloseDataSocket(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	// Is there an opened file? close it..
	if (ci->filehandle) {
		Close(ci->filehandle);
		ci->filehandle = 0;
	}
	
	// The data socket was closed only in active mode: in passive mode it stayed
	// open, so at the end of an upload the server never saw the EOF and never
	// sent "226" for STOR (the client then waited forever), and every passive
	// LIST/RETR leaked a socket. Close it in both modes; the flags are reset
	// even if it was already closed (AbortFileTransfer() closes it first).
	if (ci->transfer_socket != INVALID_SOCKET)
		InvalidateSocket(&ci->transfer_socket);
	#ifdef DEBUG
	else
		DebugOutput("CloseDataSocket(): socket already closed\n");
	#endif
	
	cn->ci.b_transfering = false;
	ci->port = ci->file_readed = ci->file_size = ci->file_sent = 0;
	
	//	
	#ifdef DEBUG
	DebugOutput("CloseDataSocket(): closed socket\n");
	#endif
	
	return true;	
}

char *IpToStr(int ip, char sepchar)
{
	static char temp[512];
	
	snprintf(temp, 511, "%d%c%d%c%d%c%d", ((ip>>24)&0xFF), sepchar, ((ip>>16)&0xFF), sepchar, ((ip>>8)&0xFF), sepchar, ip&0xFF);
	return temp;
}

bool GetConnectionInfo(ClientInfo *ci)
{
	struct sockaddr_in sockaddr;
	int status;
	
	// Get the port of the socket 
	status = sizeof(struct sockaddr);
	getsockname(ci->cmd_socket, (struct sockaddr *) &sockaddr, (void *) &status);
	ci->port = htons(sockaddr.sin_port);
	
	// Get the IP of the cmd socket because the stack won't return the ip information
	// of an unconnected socket (MSDN)
	status = sizeof(struct sockaddr);
	getsockname(ci->cmd_socket, (struct sockaddr *) &sockaddr, (void *) &status);
	ci->ip_i = htonl(sockaddr.sin_addr.s_addr);
	caf_strncpy(ci->ip_s, IpToStr(ci->ip_i, ','), 31);
	
	return true;
}

bool CheckSockForErrors(Connection *cn, SOCKET *socket)
{
	ClientInfo *ci = &cn->ci;
	bool b_ret = false;
	int ret = 0, optlen;
	
	optlen = sizeof(ret);		// was uninitialized
	getsockopt (*socket, SOL_SOCKET, SO_ERROR, (void *) &ret, (void *) &optlen);
	if (ret != 0) {
		// Something wrong.. whops
		printf("\nError code(getsockopt()) = %d string = '%s'\n", ret, GetErrnoDesc(ret));
		
		if (*socket == ci->cmd_socket) {
			// Cmd socket is closed? hell!
			CleanSockAndFlags(cn);
		} else if (*socket == ci->listen_socket) {
			// Error on listen socket, better close it
			CloseListenSocket(cn);
		} else if (*socket == ci->transfer_socket) {
			// Error on data socket
			// CloseDataSocket(cn);
		}
	} else
		// No errors
		b_ret = true;
	
	return b_ret;
}

void CleanSockAndFlags(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	InvalidateSocket(&ci->cmd_socket);
	InvalidateSocket(&ci->listen_socket);
	InvalidateSocket(&ci->transfer_socket);
	ci->port = ci->ip_i = 0; ci->ip_s[0] = ci->bytes_transfered = 0;
	SetConnected(cn, false);
	
	cn->ci.b_file_transfer =
	cn->ci.b_file_download =
	cn->ci.b_connected =
	cn->ci.b_connecting =
	cn->ci.b_waitingfordataport =
	cn->ci.b_transfer_error =
	cn->ci.b_transfering =
	cn->ci.b_transfered =
	cn->ci.b_can_send_command =
	cn->ci.b_directory_processed =
	cn->ci.b_user_accepted =
	cn->ci.b_pass_accepted =
	cn->ci.b_request_list =
	cn->ci.b_request_download =
	cn->ci.b_request_upload =
	cn->ci.b_request_conn =
	cn->ci.b_last_data =
	cn->ci.b_passive =
	cn->ci.b_processing_queue = false;

//	cn->ci.b_request_disconn =
//	cn->ci.b_request_quit =
	
	gui_status("STATUS: Disconnected");
}

void RecvStackPush(int command)
{
	#ifdef DEBUGLV2
	char temp[512];
	#endif
	int i;
	
	// Older entries move towards the bottom, the current one is at index 0
	// (the old loop shifted in the wrong direction, losing the history)
	for (i = STACK_BOTTOM; i > STACK_CURRENT; i--)
		recv_command_stack[i] = recv_command_stack[i-1];
	
	recv_command_stack[STACK_CURRENT] = command;
	#ifdef DEBUGLV2
	snprintf(temp, 511, "RecvStackPush(): added cmd %d to stack\n", command);
	DebugOutput(temp);
	#endif
}

void SentStackPush(char *CMD)
{
	char temp[512];
	int i;
	
	temp[0] = 0;
	sscanf(CMD, "%511s", temp);
	#ifdef DEBUGLV2
	DebugOutput("SentStackPush(): Putting '");
	DebugOutput(temp);
	DebugOutput("' into the sent stack\n");
	#endif
	for (i = STACK_BOTTOM; i > STACK_CURRENT; i--)
		caf_strncpy(sent_command_stack[i], sent_command_stack[i-1], sizeof(sent_command_stack[0]));
	
	caf_strncpy(sent_command_stack[STACK_CURRENT], temp, sizeof(sent_command_stack[0]));
}

int GetCommandDigit(int command, int digit)
{
	int numbas[3];
	
	// digit 0 read numbas[3], out of the array
	if (digit < 1 || digit > 3) {
		#ifdef DEBUG
		DebugOutput("GetCommandDigit(): Wrong digit requested at GetCommandDigit() function\n");
		#endif
		return 0;
	}
	
	numbas[0] = command % 10;
	command /= 10;
	numbas[1] = command % 10;
	command /= 10;
	numbas[2] = command % 10;
	return numbas[3-digit];
}

bool IsLastSentCmd(char *str)
{
	#ifdef DEBUGLV2
	DebugOutput("IsLastSentCmd(");
	DebugOutput(str);
	DebugOutput("), sent_command_stack[STACK_CURRENT] = '"); 
	DebugOutput(sent_command_stack[STACK_CURRENT]);
	DebugOutput("'\n");
	#endif
	return (strncmp(sent_command_stack[STACK_CURRENT], str, 511) == 0) ? true: false;
}

void ClearTransferFlags(Connection *cn)
{
	cn->ci.b_request_list = false;
	cn->ci.b_request_download = false;
	cn->ci.b_request_upload = false;
	cn->ci.b_transfering = false;
}

bool AbortFileTransfer(Connection *cn)
{
	if (cn->ci.b_transfering) {
		if (cn->ci.b_passive == false)
		   SendFtpCommand(cn, "ABOR", false);
		else {
			InvalidateSocket(&cn->ci.transfer_socket);
			cn->ci.b_transfer_error = false;
			cn->ci.b_transfering = false;
			gui_status("STATUS: Transfer aborted by user");
			SentStackPush("ABOR");
			CloseDataSocket(cn);
		}
	}
	
	#ifdef DEBUG
	printf("WARNING: AbortFileTransfer() was called but no file transfer is in progress\n");
	#endif
	
	return true;
}

void QueueAdvance(Connection *cn)
{
	if (cn->ci.b_queue_step == true) {
		if (cn->ci.queue_step > cn->ci.queue_to_step)
			return;
		
		cn->ci.queue_step++;
		gui_queue_changed();
	}
	
	while (cn->queue_cur < cn->queue_used) {
		gui_queue_changed();
		QueueProcess(cn);
		
		if (QueuePop(cn) == false) {
			QueuePostProcess(cn);
			return;
		}
		
		if (cn->ci.b_queue_step == true)
			return;
		
		// Eat every local command and stop to non local commands
		if (cn->QueuedItems[cn->queue_cur-1].b_local_cmd == false)
			break;
	}
}

bool QueuePush(Connection *cn, int command, char *data, char *c_arg1, int i_arg1, bool b_local_cmd)
{
	// Was checking queue_cur: queue_used could go past QueuedItems[]
	if (cn->queue_used+1 >= QUEUE_SIZE) {
		gui_status("ERROR: Queue items stack is full, aborting operation");
		return false;
	}
	
	cn->QueuedItems[cn->queue_used].command = command;
	cn->QueuedItems[cn->queue_used].data	= data;
	cn->QueuedItems[cn->queue_used].c_arg1	= c_arg1;
	cn->QueuedItems[cn->queue_used].i_arg1	= i_arg1;
	cn->QueuedItems[cn->queue_used].b_local_cmd = b_local_cmd;
	cn->queue_used++;
	
	return true;
}

void *QueuePushAddStr(Connection *cn, buffer *buf, char *string, int command, char *c_arg1, int i_arg1, bool b_local_cmd)
{
	void *ptr;
	
	// Adds the current dir to the queue, change dir
	ptr = BufferAddStr(buf, string);
	if (ptr == 0) {
		gui_status("ERROR: Queue buffer is full, cannot continue with directory scanning");
		return 0;
	}
	
	if (QueuePush(cn, command, ptr, c_arg1, i_arg1, b_local_cmd) == false) {
		gui_status("ERROR: QueuePush() failed, queue items buffer is full, cannot continue with directory scanning");
		return 0;
	}
	
	return ptr;
}

bool QueuePop(Connection *cn)
{
	if (cn->queue_cur+1 >= cn->queue_used)
		return false;
	
	cn->queue_cur++;
	return true;
}

void QueueClear(Connection *cn)
{
	cn->ci.b_processing_queue = false;
	cn->queue_cur = 0;
	cn->queue_used = 0;
	cn->last_queue_update = 0;
	cn->ci.queue_step = 0;
	cn->ci.queue_to_step = 1;
}

void QueueProcess(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	if (ci->b_connected == false) {
		gui_status("ERROR: Not connected");
		return;
	}
	
	switch (cn->QueuedItems[cn->queue_cur].command) {
		case	CMD_DOWNLOAD:
				InitiateFileTransfer(cn, cn->QueuedItems[cn->queue_cur].data, true);
				break;
				
		case	CMD_UPLOAD:
				InitiateFileTransfer(cn, cn->QueuedItems[cn->queue_cur].data, false);
				break;
				
		case	CMD_REMOTE_DELETE:
				InitiateFileDeletion(cn, cn->QueuedItems[cn->queue_cur].data);
				break;
				
		case	CMD_LOCAL_MKDIR:
				LocalMakedir(cn, cn->QueuedItems[cn->queue_cur].data);
				break;
				
		case	CMD_REMOTE_MKDIR:
				RemoteMakedir(cn, cn->QueuedItems[cn->queue_cur].data);
				break;
				
		case	CMD_LOCAL_BASEPATH:
				LocalBasePath(cn, cn->QueuedItems[cn->queue_cur].data);
				break;
				
		case	CMD_LOCAL_DIR_SCAN:
				LocalDirScan(cn, cn->QueuedItems[cn->queue_cur].data, cn->QueuedItems[cn->queue_cur].data, caf_strlen(cn->QueuedItems[cn->queue_cur].data));
				break;
				
		case	CMD_REMOTE_DIR_SCAN:
				// ASD
				RemoteDirScan(cn, cn->QueuedItems[cn->queue_cur].data);
				break;
				
		case	CMD_REMOTE_CHDIR:
				RemoteChdir(cn, cn->QueuedItems[cn->queue_cur].data);
				break;
	}
}

void QueuePostProcess(Connection *cn)
{
	// La coda e' stata tutta processata
	RefreshLocalView(cn);
	QueueClear(cn);
	gui_queue_changed();
	RefreshLocalView(cn);
	InitiateListTransfer(cn);
}

void DecodeBitFlags(char *dest, LONG flags)
{
	char temp[32];
	int i = 0;
	
	temp[i++] = (flags & FIBF_READ) == 0 ? 'R': ' ';
	temp[i++] = (flags & FIBF_WRITE) == 0 ? 'W': ' ';
	temp[i++] = (flags & FIBF_EXECUTE) == 0 ? 'E': ' ';
	temp[i++] = (flags & FIBF_DELETE) == 0 ? 'D': ' ';
	
	/* Not supported in aros ?
	temp[i++] = (1-(flags & FIBF_ARCHIVE))*'A';
	temp[i++] = (1-(flags & FIBF_PURE))*'P';
	temp[i++] = (1-(flags & FIBF_SCRIPT))*'S';
	*/
	
	temp[i++] = 0;
	caf_strncpy(dest, temp, 512);
}

void BufferFlush(buffer *buf)
{
	buf->curpos = 0;
}

void BufferOpen(buffer *buf, bool b_clear)
{
	if (b_clear == true)
		BufferFlush(buf);
	
	buf->b_opened = true;
}

char *BufferAddStr(buffer *buf, char *string)
{
	char *ret_ptr, copied;
	int slen, pad = 0;
	
	if (buf->b_opened == false) {
		// Returned buf->ptr: the caller then overwrote the start of the buffer
		printf("BufferAddStr() buffer is closed, cannot write\n");
		return 0;	
	}
	
	slen = caf_strlen(string);
	ret_ptr = &buf->ptr[buf->curpos];
	ret_ptr = AlignPtr(ret_ptr, 4, &pad);
	
	if (buf->curpos+slen+pad+16 >= buf->size) {
		printf("ERROR: Buffer full in BufferAddStr()\n");
		return 0;
	}
	copied = caf_strcpy(ret_ptr, string);
	buf->curpos += slen+1+pad;
	
	return ret_ptr;
}

void *BufferAddStruct(buffer *buf, void *struc, int size)
{
	char *ret_ptr;
	int pad;
	
	ret_ptr = &buf->ptr[buf->curpos];
	ret_ptr = AlignPtr(ret_ptr, 4, &pad);
	if (buf->curpos+size+pad+16 >= buf->size) {
		printf("ERROR: Buffer full in BufferAddStruct()\n");
		return 0;
	}
	
	memcpy(ret_ptr, struc, size);
	buf->curpos += size+pad;
	
	return ret_ptr;
}

void BufferClose(buffer *buf)
{
	buf->b_opened = false;
}

void *AlignPtr(void *ptr, int align_size, int *align_amount)
{
	char *ret_ptr = ptr;
	int pad = 0;
	
	pad = ((int)ret_ptr) & (align_size-1);
	if (pad) {
		ret_ptr += align_size-pad;
		pad = align_size-pad;
	}
	
	if (align_amount)
		*align_amount = pad;
	
	return ret_ptr;
}

int InitiateListTransfer(Connection *cn)
{
	HostInfo *hi = &cn->hi;
	
	if (cn->ci.b_can_send_command) {
		if (hi->conn_type == CTYPE_ACTV) {
			/* LIST, ACTIVE */
			if (OpenListenSocket(cn)) {
				SetTransferMode(cn, 'A');
				cn->ci.b_request_list = true;
				return NO_ERRORS;
			} else
				gui_status("STATUS: OpenListenSocket() failed, cannot listen on a socket, cannot get directory listing");
		} else {
			/* LIST, PASSIVE */
			// ASD
			SetTransferMode(cn, 'A');
			cn->ci.b_request_list = true;
		}
	} else {
		if (cn->ci.b_connected == true) {
			gui_status(STR_CANT_SEND);
			return ANSWER_PENDING;
		} else {
			gui_status(STR_CANT_SEND_NOT_CONNECTED);
			return NOT_CONNECTED;
		}
	}
	
	return FLOW_CONTROL_ERROR;
}

int InitiateFileTransfer(Connection *cn, char *filename, bool b_download)
{
	ClientInfo *ci = &cn->ci;
	HostInfo *hi = &cn->hi;
	
	if (cn->ci.b_can_send_command) {
		if (hi->conn_type == CTYPE_ACTV) {
			/* DOWNLOAD, ACTIVE */
			if (OpenListenSocket(cn)) {
				SetTransferMode(cn, 'I');
				cn->ci.b_request_download = true;
				cn->ci.b_file_download = b_download;
				strncpy(ci->filename, filename, 255);
				return NO_ERRORS;
			} else {
				gui_status("STATUS: OpenListenSocket() failed, cannot listen on a socket, cannot request a file transfer");
				ci->b_processing_queue = false;
			}
		} else {
			/* DOWNLOAD, PASSIVE */
			SetTransferMode(cn, 'I');
			cn->ci.b_request_download = true;
			cn->ci.b_file_download = b_download;
			strncpy(ci->filename, filename, 255);
			return NO_ERRORS;
		}
	} else {
		if (cn->ci.b_connected == true) {
			gui_status(STR_CANT_SEND);
			return ANSWER_PENDING;
		} else {
			gui_status(STR_CANT_SEND_NOT_CONNECTED);
			return NOT_CONNECTED;
		}
	}
	
	return FLOW_CONTROL_ERROR;
}

int InitiateFileDeletion(Connection *cn, char *filename)
{
	char temp[512];
	
	snprintf(temp, 511, "DELE %s", filename);
	SendFtpCommand(cn, temp, false);
	return ANSWER_PENDING;
}

void AcknowledgeConnection(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	// Command socket event happened, during connect() process it
	if (CheckConnect(cn) == true) {
		// We are connected
		GetConnectionInfo(ci);
	} else
		CleanSockAndFlags(cn);
}

int GatherIpPortNumber(Connection *cn)
{
	int i, port_start = 0, comma_counter = 0, port_comma = 0, temp_counter = 0;
	char *text = cn->cmd_buffer.ptr, temp[512];
	HostInfo *hi = &cn->hi;
	bool b_copy = false;
	
	for (i = 0; text[i] != 0 && i < 511; i++) {
		// Exit at closed paranthesis
		if (text[i] == ')')
			break;
		
		if (b_copy) {
			// Substitutes comma with point, for inet_addr
			if (text[i] != ',')
				temp[temp_counter++] = text[i];
			else {
				comma_counter++;
				
				if (port_start && port_comma == 0)
					port_comma = temp_counter;
				
				if (comma_counter == 4) {
					// Port data starts here+1
					port_start = temp_counter;
				}
				
				temp[temp_counter++] = '.';
			}
		}
		
		if (text[i] == '(')
			b_copy = true;
	}
	temp[temp_counter++] = 0;
	
	if (port_start) {
		temp[port_start] = 0;
		temp[port_comma] = 0;
		hi->pasvsettings.ip_i = inet_addr(temp);
		hi->pasvsettings.port = atol(&temp[port_start+1]) << 8;
		hi->pasvsettings.port |= atol(&temp[port_comma+1]);
	} else {
		gui_status("ERROR: Cannot get passive connection information");
	}
	
	return port_start;
}

int SendMKDCMD(Connection *cn, char *pathname)
{
	char temp[512];
	
	snprintf(temp, 511, "MKD %s", pathname);
	SendFtpCommand(cn, temp, false);
	return ANSWER_PENDING;
}

int SendPASVCMD(Connection *cn)
{
	SendFtpCommand(cn, "PASV", false);
	cn->ci.b_passive = true;
	return ANSWER_PENDING;
}

void AfterRETR(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	if (ci->b_transfering == true)
		return;
	
	// DOWNLOADED
	if (g_GlobalSettings.SettingsArray[SETTING_LOCAL_REFRESH_AFTER_RETR] == OPT_REFRESH_AFTER_RETR)
		RefreshLocalView(cn);
	
	if (ci->b_processing_queue) {
		QueueAdvance(cn);
	} else
		if (g_GlobalSettings.SettingsArray[SETTING_LOCAL_REFRESH_AFTER_RETR] == OPT_NO_REFRESH_AFTER_RETR)
			// Refresh Local Directory
			RefreshLocalView(cn);
}

void AfterSTOR(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	// UPLOADED
	if (ci->b_processing_queue) {
		QueueAdvance(cn);
	} else
		// Refresh Remote Directory
		InitiateListTransfer(cn);
}

void AfterDELE(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	// DELETED
	if (ci->b_processing_queue) {
		QueueAdvance(cn);
	} else
		// Refresh the remote view
		InitiateListTransfer(cn);
}

void AfterCWD(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	if (ci->b_processing_queue) {
		QueueAdvance(cn);
	} else {
		// Refresh the remote view
		SendPWDCMD(cn);
	}
}

void AfterMKD(Connection *cn)
{
	ClientInfo *ci = &cn->ci;
	
	if (ci->b_processing_queue) {
		QueueAdvance(cn);
	} else {
		// Refresh the remote view 
		InitiateListTransfer(cn);
	}
}

bool LocalMakedir(Connection *cn, char *pathname)
{
	BPTR lock;
	
	lock = CreateDir(pathname);
	if (lock != 0) {
		UnLock(lock);
		return true;
	}
	
	return false;
}

void RemoteMakedir(Connection *cn, char *pathname)
{
	SendMKDCMD(cn, pathname);
}

bool LocalDirScan(Connection *cn, char *pathname, char *basepath, int base_start)
{
	struct FileInfoBlock *fib = 0;
	char temp[512], curdir[512];
	bool b_ret = true;
	BPTR lock;
	char *ptr;
	
	// NOTES on the old version:
	//  - fib was never freed (missing on every exit path)
	//  - the lock was not released when the directory stack was full
	//  - sprintf() on 512 bytes buffers with 2x512 bytes inputs
	fib = AllocFIB();		/* AllocDosObject() is dos V36 */
	if (!fib) {
		gui_status("LOCAL: out of memory, cannot proceed with the directory scan");
		return false;
	}
	
	BufferOpen(&cn->temp_buffer, false);
	BufferOpen(&cn->queue_buffer, false);
	caf_strncpy(curdir, pathname, 512);
	for (;;) {
		lock = Lock(curdir, ACCESS_READ);
		if (lock == 0) {
			snprintf(temp, 511, "Locking of '%s' failed\n", curdir);
			gui_status(temp);
			break;
		}
		
		ptr = QueuePushAddStr(cn, &cn->queue_buffer, curdir, CMD_LOCAL_BASEPATH, 0, 0, true);
		if (ptr == 0) {
			b_ret = false;
			break;
		}
		
		// Changes remote directory
		snprintf(temp, 511, "%s/%s", cn->QueueBasePath, &curdir[base_start+1]);
		ptr = QueuePushAddStr(cn, &cn->queue_buffer, temp, CMD_REMOTE_CHDIR, 0, 0, false);
		if (ptr == 0) {
			b_ret = false;
			break;
		}
		
		if (Examine(lock, fib) != FALSE) {
			while ((ExNext(lock, fib) != DOSFALSE)) {
				if (fib->fib_DirEntryType == ST_USERDIR) {
					// Adds the directory to the walk stack
					snprintf(temp, 511, "%s/%s", curdir, fib->fib_FileName);
					ptr = BufferAddStr(&cn->temp_buffer, temp);
					if (ptr == 0) {
						gui_status("ERROR: The temporary buffer is full, cannot continue with directory scanning");
						b_ret = false;
						break;
					}
					
					if (cn->stack_cur <= 1) {
						gui_status("ERROR: Maximum number of stack entries reached, file queuing stopped");
						b_ret = false;
						break;
					}
					cn->stack_cur--;
					cn->StackStack[cn->stack_cur].data = ptr;
					
					// Adds the directory to the queue, createdir
					snprintf(temp, 511, "%s/%s/%s", cn->QueueBasePath, &curdir[base_start+1], fib->fib_FileName);
					ptr = QueuePushAddStr(cn, &cn->queue_buffer, temp, CMD_REMOTE_MKDIR, 0, 0, false);
					if (ptr == 0) {
						b_ret = false;
						break;
					}
				} else {
					// Adds the file to the queue
					ptr = QueuePushAddStr(cn, &cn->queue_buffer, fib->fib_FileName, CMD_UPLOAD, 0, 0, false);
					if (ptr == 0) {
						b_ret = false;
						break;
					}
				}
			}
		}
		
		UnLock(lock);
		lock = 0;
		
		if (b_ret == false)
			break;
		
		if (cn->stack_cur >= STACK_SIZE) {
			#ifdef DEBUG
			DebugOutput("LocalDirScan(): STACK IS ZERO, EXITING");
			#endif
			break;
		}
		
		caf_strncpy(curdir, cn->StackStack[cn->stack_cur].data, 512);
		cn->stack_cur++;
	}
	
	if (lock)
		UnLock(lock);
	
	FreeFIB(fib);
	
	//BufferClose(&cn->temp_buffer);
	//BufferClose(&cn->queue_buffer);
	return b_ret;
}

void RemoteDirScan(Connection *cn, char *pathname)
{
	
}

int MemReadDword(char **ptr)
{
	// unsigned: with plain (signed) char a byte >= 0x80 was sign extended
	unsigned char *p = (unsigned char *) *ptr;
	int tmp;
	
	tmp = (p[0] | p[1]<<8 | p[2]<<16 | p[3]<<24);
	*ptr += 4;
	return tmp;
}

int MemReadString(char *dest, char **ptr, int len, int maxlen)
{
	int rc;
	
	// len is "string length + 1" as read from the file
	if (len < 1 || len > 65536) {
		// Corrupted file
		dest[0] = 0;
		return 0;
	}
	
	if (len > maxlen) {
		#ifdef DEBUG
		DebugOutput("MemReadString(): len > maxlen!\n");
		#endif
		// Truncate, but always skip the whole string: the old code returned
		// without moving *ptr and every following field was read misaligned
		caf_strncpy(dest, *ptr, maxlen);
		*ptr += len-1;
		return maxlen-1;
	}
	
	rc = caf_strncpy(dest, *ptr, len);
	*ptr += len-1;
	return rc;
}

void LocalBasePath(Connection *cn, char *path)
{
	// The queue works in its own local directory: the one shown by the GUI
	// (cn->lv.CurrentPath) does not change while the queue runs
	caf_strncpy(g_QueueLocalPath, path, sizeof(g_QueueLocalPath));
}

void RemoteChdir(Connection *cn, char *path)
{
	SendCWDCMD(cn, path);
}

/* ---------------------------------------------------------------------------
 * Configuration file: IFF FORM MFTP with a GLOB chunk (settings) and one HOST
 * chunk per address book entry, values little endian (same format as the
 * versions based on iffparse.library, which is not available on OS 1.3).
 * ------------------------------------------------------------------------- */
#define CFG_ID(a,b,c,d) (((ULONG)(a)<<24)|((ULONG)(b)<<16)|((ULONG)(c)<<8)|(ULONG)(d))

static ULONG get_be32(const UBYTE *p) { return ((ULONG)p[0]<<24)|((ULONG)p[1]<<16)|((ULONG)p[2]<<8)|p[3]; }
static LONG get_le32(const UBYTE *p) { return (LONG)(((ULONG)p[3]<<24)|((ULONG)p[2]<<16)|((ULONG)p[1]<<8)|p[0]); }
static void put_be32(UBYTE *p, ULONG v) { p[0]=(UBYTE)(v>>24); p[1]=(UBYTE)(v>>16); p[2]=(UBYTE)(v>>8); p[3]=(UBYTE)v; }
static void put_le32(UBYTE *p, ULONG v) { p[3]=(UBYTE)(v>>24); p[2]=(UBYTE)(v>>16); p[1]=(UBYTE)(v>>8); p[0]=(UBYTE)v; }

/* Configuration file locations, in order */
static const char *cfg_name(int i)
{
	static const char *names[] = { "MARRANO:marranoftp.moo", "PROGDIR:marranoftp.moo", "S:marranoftp.moo" };
	if (i == 1 && DOSBase->dl_lib.lib_Version < 36)
		return "";			/* no PROGDIR: before OS 2.0 */
	return i < 3 ? names[i] : 0;
}

static BPTR cfg_open(LONG mode)
{
	struct Process *me = (struct Process *) FindTask(NULL);
	APTR oldwin = me->pr_WindowPtr;
	const char *name;
	BPTR fh = 0;
	int i;

	me->pr_WindowPtr = (APTR) -1;		/* no "insert volume MARRANO:" requesters */
	for (i = 0; (name = cfg_name(i)) != 0; i++) {
		if (!*name)
			continue;
		if ((fh = Open((STRPTR) name, mode)) != 0) {
			caf_strncpy(g_config_filename, (char *) name, sizeof(g_config_filename));
			break;
		}
	}
	me->pr_WindowPtr = oldwin;
	return fh;
}

/* Reads a length-prefixed string of a HOST chunk */
static const UBYTE *cfg_string(const UBYTE *p, const UBYTE *end, char *dest, int maxlen)
{
	LONG len;

	dest[0] = 0;
	if (!p || p + 4 > end)
		return 0;
	len = get_le32(p);
	p += 4;
	if (len < 0 || p + len > end)
		return 0;
	caf_strncpy(dest, (char *) p, (len + 1 < maxlen) ? len + 1 : maxlen);
	return p + len;
}

static void LoadConfig(Connection *cn)
{
	const UBYTE *hosts[MAX_HOSTS];
	UBYTE *mem = 0, *p, *end;
	LONG size = 0, glob_ver = -1;
	int nhosts = 0, i;
	char temp[640];
	BPTR fh;

	fh = cfg_open(MODE_OLDFILE);
	if (!fh) {
		gui_status("ERROR: Cannot load configuration file (MARRANO:, PROGDIR: or S:marranoftp.moo)");
		caf_strncpy(g_config_filename, "MARRANO:marranoftp.moo", sizeof(g_config_filename));
		return;
	}
	Seek(fh, 0, OFFSET_END);
	size = Seek(fh, 0, OFFSET_BEGINNING);
	if (size > 12 && size < 1024 * 1024 && (mem = (UBYTE *) AllocMem(size, MEMF_PUBLIC)) != 0) {
		if (Read(fh, mem, size) != size) {
			FreeMem(mem, size);
			mem = 0;
		}
	}
	Close(fh);
	if (!mem) {
		gui_status("ERROR: Cannot read the configuration file");
		return;
	}

	if (get_be32(mem) != CFG_ID('F','O','R','M') || get_be32(mem + 8) != CFG_ID('M','F','T','P')) {
		gui_status("ERROR: The configuration file is not a MarranoFTP IFF file");
		FreeMem(mem, size);
		return;
	}

	end = mem + size;
	for (p = mem + 12; p + 8 <= end; ) {
		ULONG id = get_be32(p), len = get_be32(p + 4);
		const UBYTE *data = p + 8;
		if (data + len > end)
			break;
		if (id == CFG_ID('G','L','O','B') && len >= 16) {
			LONG v = get_le32(data);
			if (v == CONFIG_VERSION1) {
				LONG n = get_le32(data + 4);
				glob_ver = 2;
				for (i = 0; i < n && i < NUM_OF_SETTINGS && 8 + 4 * (i + 1) <= (LONG) len; i++)
					g_GlobalSettings.SettingsArray[i] = get_le32(data + 8 + 4 * i);
			} else {
				/* first version: transfer timeout, records, connect timeout, queue type */
				glob_ver = 1;
				g_GlobalSettings.SettingsArray[SETTING_TRANSFER_TIMEOUT] = v;
				g_GlobalSettings.SettingsArray[SETTING_CONNECT_TIMEOUT] = get_le32(data + 8);
				g_GlobalSettings.SettingsArray[SETTING_QUEUE_TYPE] = get_le32(data + 12);
			}
		} else if (id == CFG_ID('H','O','S','T') && nhosts < MAX_HOSTS) {
			hosts[nhosts++] = p;
		}
		p += 8 + len + (len & 1);
	}

	/* The hosts are saved from the last to the first */
	g_AddressBook.used_hosts = 0;
	for (i = nhosts - 1; i >= 0; i--) {
		Host *h = &g_AddressBook.Hosts[g_AddressBook.used_hosts];
		const UBYTE *q = hosts[i] + 8, *qend = q + get_be32(hosts[i] + 4);
		caf_memset(h, 0, sizeof(Host));
		q = cfg_string(q, qend, h->entryname, ENTRYNAME_CONFIG_LEN);
		q = cfg_string(q, qend, h->hostname, HOSTNAME_CONFIG_LEN);
		q = cfg_string(q, qend, h->port, PORT_COFING_LEN);
		q = cfg_string(q, qend, h->username, USERNAME_CONFIG_LEN);
		q = cfg_string(q, qend, h->password, PASSWORD_CONFIG_LEN);
		if (q && q + 4 <= qend)
			h->conn_type = get_le32(q) ? CTYPE_PASV : CTYPE_ACTV;
		g_AddressBook.used_hosts++;
	}
	FreeMem(mem, size);

	snprintf(temp, sizeof(temp), glob_ver == 1 ? "STATUS: Old configuration format loaded from %s" :
			 "STATUS: Configuration loaded from %s", g_config_filename);
	gui_status(temp);
	gui_hosts_changed();
}

static UBYTE *cfg_put_string(UBYTE *p, const char *s)
{
	ULONG len = caf_strlen((char *) s);

	put_le32(p, len);
	CopyMem((APTR) s, p + 4, len);
	return p + 4 + len;
}

bool core_save_config(void)
{
	ULONG size = 12 + 8 + 8 + 4 * NUM_OF_SETTINGS, len;
	UBYTE *mem, *p, *chunk;
	bool ok = false;
	int i;
	BPTR fh;

	for (i = 0; i < g_AddressBook.used_hosts; i++) {
		Host *h = &g_AddressBook.Hosts[i];
		len = 5 * 4 + caf_strlen(h->entryname) + caf_strlen(h->hostname) + caf_strlen(h->port)
			+ caf_strlen(h->username) + caf_strlen(h->password) + 4;
		size += 8 + len + (len & 1);
	}

	if (!(mem = (UBYTE *) AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR)))
		return false;

	put_be32(mem, CFG_ID('F','O','R','M'));
	put_be32(mem + 4, size - 8);
	put_be32(mem + 8, CFG_ID('M','F','T','P'));
	p = mem + 12;
	put_be32(p, CFG_ID('G','L','O','B'));
	put_be32(p + 4, 8 + 4 * NUM_OF_SETTINGS);
	put_le32(p + 8, CONFIG_VERSION1);
	put_le32(p + 12, NUM_OF_SETTINGS);
	for (i = 0; i < NUM_OF_SETTINGS; i++)
		put_le32(p + 16 + 4 * i, g_GlobalSettings.SettingsArray[i]);
	p += 16 + 4 * NUM_OF_SETTINGS;

	/* From the last to the first, as the iffparse.library versions did */
	for (i = g_AddressBook.used_hosts - 1; i >= 0; i--) {
		Host *h = &g_AddressBook.Hosts[i];
		chunk = p;
		p += 8;
		p = cfg_put_string(p, h->entryname);
		p = cfg_put_string(p, h->hostname);
		p = cfg_put_string(p, h->port);
		p = cfg_put_string(p, h->username);
		p = cfg_put_string(p, h->password);
		put_le32(p, h->conn_type);
		p += 4;
		len = p - chunk - 8;
		put_be32(chunk, CFG_ID('H','O','S','T'));
		put_be32(chunk + 4, len);
		if (len & 1)
			p++;
	}

	fh = cfg_open(MODE_NEWFILE);
	if (fh) {
		ok = Write(fh, mem, size) == (LONG) size;
		Close(fh);
	}
	FreeMem(mem, size);
	return ok;
}

/* ---------------------------------------------------------------------------
 * Life cycle
 * ------------------------------------------------------------------------- */
static void *alloc_buffer(buffer *b, int size)
{
	caf_memset(b, 0, sizeof(buffer));
	b->size = size;
	b->ptr = (char *) AllocMem(size, MEMF_PUBLIC);
	return b->ptr;
}

static void free_buffer(buffer *b)
{
	if (b->ptr)
		FreeMem(b->ptr, b->size);
	b->ptr = 0;
}

bool core_init(void)
{
	Connection *cn = &g_Connection;
	ClientInfo *ci = &cn->ci;

	caf_memset(cn, 0, sizeof(Connection));
	caf_memset(&g_AddressBook, 0, sizeof(AddressBook));
	caf_memset(sent_command_stack, 0, sizeof(sent_command_stack));
	caf_memset(recv_command_stack, 0, sizeof(recv_command_stack));
	ci->cmd_socket = ci->listen_socket = ci->transfer_socket = cn->hi.pasvsettings.socket = INVALID_SOCKET;

	if (!alloc_buffer(&cn->cmd_buffer, COMMAND_BUFFER_SIZE) ||
		!alloc_buffer(&cn->transfer_buffer, TRANSFER_BUFFER_SIZE) ||
		!alloc_buffer(&cn->queue_buffer, QUEUE_BUFFER_SIZE) ||
		!alloc_buffer(&cn->temp_buffer, TEMP_BUFFER_SIZE) ||
		!alloc_buffer(&cn->lv.ListBuffer, LIST_BUFFER_SIZE) ||
		!alloc_buffer(&cn->rv.ListBuffer, LIST_BUFFER_SIZE)) {
		printf("MarranoFTP: not enough memory for the buffers\n");
		return false;
	}

	SocketBase = OpenLibrary((STRPTR) "bsdsocket.library", 0);
	if (!SocketBase) {
		printf("Cannot open bsdsocket.library (start the TCP/IP stack first)\n");
		return false;
	}
	#ifdef __SASC
	// bsdsocket.library opened by hand: tell it where errno is
	SetErrnoPtr(&errno, sizeof(errno));
	#endif

	/* CreateMsgPort()/CreateIORequest() are exec V36: amiga.lib versions */
	g_TimerMP = CreatePort(0, 0);
	if (g_TimerMP)
		g_TimerIO = (struct timerequest *) CreateExtIO(g_TimerMP, sizeof(struct timerequest));
	if (!g_TimerIO || OpenDevice((STRPTR) TIMERNAME, UNIT_VBLANK, (struct IORequest *) g_TimerIO, 0)) {
		printf("Cannot open timer.device\n");
		return false;
	}
	g_timer_open = true;

	g_GlobalSettings.SettingsArray[SETTING_SOCKET_TIMEOUT] = 2;
	g_GlobalSettings.SettingsArray[SETTING_TRANSFER_TIMEOUT] = 5;
	g_GlobalSettings.SettingsArray[SETTING_CONNECT_TIMEOUT] = 10;
	g_GlobalSettings.SettingsArray[SETTING_QUEUE_TYPE] = OPT_QUEUE_STOP_ON_ERROR;
	g_GlobalSettings.SettingsArray[SETTING_KEEP_PARTIAL] = OPT_DOWNLOAD_DELETE_PARTIAL;
	g_GlobalSettings.SettingsArray[SETTING_LOCAL_REFRESH_AFTER_RETR] = OPT_NO_REFRESH_AFTER_RETR;
	QueueClear(cn);
	caf_strncpy(g_config_filename, "MARRANO:marranoftp.moo", sizeof(g_config_filename));
	caf_strncpy(cn->lv.CurrentPath, "RAM:", sizeof(cn->lv.CurrentPath));
	return true;
}

void core_start(void)
{
	Connection *cn = &g_Connection;

	LoadConfig(cn);
	gui_set_connected(false);
	RefreshLocalView(cn);
	gui_queue_changed();
}

void core_cleanup(void)
{
	Connection *cn = &g_Connection;
	ClientInfo *ci = &cn->ci;

	if (ci->filehandle) {
		Close(ci->filehandle);
		ci->filehandle = 0;
	}
	if (SocketBase) {
		InvalidateSocket(&ci->cmd_socket);
		InvalidateSocket(&ci->listen_socket);
		InvalidateSocket(&ci->transfer_socket);
		CloseLibrary(SocketBase);
		SocketBase = 0;
	}
	if (g_timer_open)
		CloseDevice((struct IORequest *) g_TimerIO);
	g_timer_open = false;
	if (g_TimerIO)
		DeleteExtIO((struct IORequest *) g_TimerIO);
	if (g_TimerMP)
		DeletePort(g_TimerMP);
	g_TimerIO = 0;
	g_TimerMP = 0;

	free_buffer(&cn->cmd_buffer);
	free_buffer(&cn->transfer_buffer);
	free_buffer(&cn->queue_buffer);
	free_buffer(&cn->temp_buffer);
	free_buffer(&cn->lv.ListBuffer);
	free_buffer(&cn->rv.ListBuffer);
}

/* ---------------------------------------------------------------------------
 * Main loop: waits on the sockets (WaitSelect) and on the GUI signals
 * ------------------------------------------------------------------------- */
void core_run(void)
{
	Connection *cn = &g_Connection;
	ClientInfo *ci = &cn->ci;
	int command, sel_sock, ret, local_errno, optlen;
	fd_set readfd, writefd, exceptfd;
	SOCKET highest_sock = INVALID_SOCKET;
	ULONG sigs = 0, sigmask = 0;
	struct sockaddr sockaddr;
	struct timeval timeout;
	char temp[512];

	g_quit_now = false;
	for (;;) {
		sigs = 0;
				sigmask = gui_signals() | SIGBREAKF_CTRL_C;
				if (ci->cmd_socket == INVALID_SOCKET && cn->ci.b_connecting == false && cn->ci.b_connected == false) {
					#ifdef DEBUGLV3
					DebugOutput("MainLoop(): Wait()\n");
					#endif
					sigs = Wait(sigmask);
				} else {
					// WaitSelect.....
					timeout.tv_secs = g_GlobalSettings.SettingsArray[SETTING_SOCKET_TIMEOUT]; timeout.tv_micro = 0;
					highest_sock = (ci->listen_socket > ci->cmd_socket) ? ci->listen_socket : ci->cmd_socket;
					highest_sock = (ci->transfer_socket > highest_sock) ? ci->transfer_socket : highest_sock;
					
					// Zero them fd sets					
					FD_ZERO(&readfd); FD_ZERO(&writefd); FD_ZERO(&exceptfd);
					
					if (cn->ci.b_connected == true) {
						// We are connected, set the command socket in the readfd
						// we wait for ftp commands and answers as well
						FD_SET(ci->cmd_socket, &readfd);
						
						if (cn->ci.b_waitingfordataport == true) {
							if (cn->ci.b_passive == false) {
								// Active data connection, we are listening
								FD_SET(ci->listen_socket, &readfd);
							} else {
								// Passive data connection, we are connecting
								
								// if we are waiting for data port in active mode,
								// a connect() has been issued, wait for writefd in the
								// socket
								FD_SET(ci->transfer_socket, &writefd);
								#ifdef DEBUG
								DebugOutput("MainLoop(): Setting ci->transfer_socket in writefd (PASV)\n");
								#endif
							}
						}
						
						if (cn->ci.b_transfering == true) {
							if (cn->ci.b_file_transfer) {
								/* FILE TRANSFER */
								if (ci->b_file_download)
									// We are downloading, let's set the transfer socket in readfd
									FD_SET(ci->transfer_socket, &readfd);
								else
									// We are uploading, let's set the transfer socket in writefd
									FD_SET(ci->transfer_socket, &writefd);
							} else {
								/* LIST */
								
								// We are downloading, let's set the transfer socket in readfd
								FD_SET(ci->transfer_socket, &readfd);
							}
						}
					} else if (cn->ci.b_connecting == true) {
						// If we are going to connect, let's test the write and except fd
						FD_SET(ci->cmd_socket, &writefd);
						FD_SET(ci->cmd_socket, &exceptfd);
					} else {
						DebugOutput("ERROR! code should not reach this stage\n");
					}
					
					#ifdef DEBUGLV3
					DebugOutput("MainLoop(): WaitSelect()\n");
					#endif
					sel_sock = WaitSelect(highest_sock+1, &readfd, &writefd, &exceptfd, &timeout, &sigmask);
					sigs = sigmask;
					
					if (sel_sock > 0) {
						if (cn->ci.b_transfering == true) {
							// We are transfering stuph, or are we transfering stuph?
							if (CheckSocket(ci->transfer_socket) == false) {
								CloseDataSocket(cn);
								gui_status("STATUS: Error on transfer socket");
							} else {
								if (FD_ISSET(ci->transfer_socket, &readfd)) {
									// Downloading...
									#ifdef DEBUG
									printf("Calling HandleDownload()\n");
									#endif
									HandleDownload(cn);
								} else if (FD_ISSET(ci->transfer_socket, &writefd) && ci->b_file_transfer && ci->b_file_download == false) {
									// Uploading...
									#ifdef DEBUG
									printf("Calling HandleUpload()\n");
									#endif
									HandleUpload(cn);
								}
							}
							
							if (TimeInSecs(&g_last_packet_transfered_timer) > g_GlobalSettings.SettingsArray[SETTING_TRANSFER_TIMEOUT]) {
								// Houston Houston, we have a problem! last packet sent is
								// very old.. maybe a connection problem ? let's close the
								// transfer socket
								if (ci->b_file_download) {
									AbortFileTransfer(cn);
									// SendFtpCommand(cn, "ABOR", false);
									gui_status("STATUS: Closing transfer socket, data timeout, no more data was received in a timely fashion");
								} else
									gui_status("STATUS: Closing transfer socket, data timeout, no more data was sent in a timely fashion");
								
								CloseDataSocket(cn);
							}
						}
						
						if (cn->ci.b_connecting == true) {
							// We are trying to connect
							if (FD_ISSET(ci->cmd_socket, &writefd)) {
								AcknowledgeConnection(cn);
							} else if (FD_ISSET(ci->cmd_socket, &exceptfd)) {
								// Command socket event happened, during connect(), error!
								CleanSockAndFlags(cn);
								gui_status("STATUS: exceptfd was set, socket error");
							} else if(TimeInSecs(&g_trying_connect_timer) >= 6) {
								gui_status("STATUS: Connect timed out");
								CleanSockAndFlags(cn);
							}
						}
						
						// This code has to be before the latter code, transfered byte
						// should be updated before the data is sent to listviews
						if (cn->ci.b_transfering == true || cn->ci.b_transfered == true) {
							ULONG elapsed_ms, kbs;
							UpdateTimer(&g_gui_update_timer);
							if (ElapsedMs(&g_gui_update_timer) >= 1000 || cn->ci.b_transfered == true) {
								UpdateTimer(&g_transfer_start_timer);
								/* integer KB/s: no floating point (no math libraries on OS 1.3) */
								elapsed_ms = ElapsedMs(&g_transfer_start_timer);
								kbs = elapsed_ms ? ((ULONG) cn->ci.bytes_transfered / 1024UL) * 1000UL / elapsed_ms : 0;
								
								if (cn->ci.b_transfering) {
									// We are transfering stuph..
									snprintf(temp, 511, "Transfering ... %d bytes %d KB/Sec", cn->ci.bytes_transfered, (int)kbs);
								} else {
									// We have transfered stuph..
									// (was "else if (b_transfered)": temp could stay uninitialized)
									snprintf(temp, 511, "Transfered ... %d bytes %d KB/Sec", cn->ci.bytes_transfered, (int)kbs);
								}
								
								gui_transfer_info(temp);
								StartTimer(&g_gui_update_timer);
							}
						}
						
						if (cn->ci.b_connected == true) {
							// We are connected, we can expect stuph from cmd socket
							if (FD_ISSET(ci->cmd_socket, &readfd)) {
								// Command socket event happened, after connect(), must be ftp stuph
								command = GetFtpCommand(&cn->cmd_buffer, cn);
								status_answer(cn);
								
								RecvStackPush(command);
								ProcessCommand(cn, command);
							}
						}
						
/************************************** WAITING FOR DATA PORT ************************************/
						if (cn->ci.b_waitingfordataport == true) {
							// We are waiting for a data port, let's check if it's
							// active or passive connection
							
							#ifdef DEBUG
							DebugOutput("MainLoop(): b_waitingfordataport\n");
							#endif
							
							if (cn->ci.b_passive) {
								#ifdef DEBUG
								snprintf(temp, 511, "MainLoop(): ci.b_passive = true, ci->transfer_socket (PASV) = %d\n", ci->transfer_socket);
								DebugOutput(temp);
								#endif
								
								if (FD_ISSET(ci->transfer_socket, &writefd)) {
									// PASSIVE DATA PORT
									
									// Transfer socket connected in passive mode
									// Previously issued connect() worked
									#ifdef DEBUG
									DebugOutput("MainLoop(): FD_ISSET passive data port\n");
									#endif
									
									snprintf(temp, 511, "STATUS: Connected to server's data port %d", cn->hi.pasvsettings.port);
									StartTimer(&g_transfer_start_timer);
									StartTimer(&g_last_packet_transfered_timer);
									gui_status(temp);
									cn->ci.b_transfering = true;
									cn->ci.b_transfered = false;
									cn->ci.b_waitingfordataport = false;
									
									// Never during an upload: these 2 bytes went on the data
									// connection before the file and ended up at the start of
									// every file uploaded in passive mode (found on AmigaOS 3.1:
									// a 20000 bytes file became 20002 bytes, "\n\n" + file)
									if (!(cn->ci.b_file_transfer && cn->ci.b_file_download == false)) {
										// ASD, this send has to be fixed ?
										// should i wait for write flag in socket?
										send(ci->transfer_socket, "\n\n", 2, 0);
									}
								}
							} else {
/********************************************* ACCEPT *****************************************/
								if (FD_ISSET(ci->listen_socket, &readfd)) {
									// ACTIVE DATA PORT
									
									// Data socket event happened, we are listening, so some1 connected
									command = sizeof(sockaddr);
									
									ci->transfer_socket = accept(ci->listen_socket, &sockaddr, (void *) &command);
									if (ci->transfer_socket == INVALID_SOCKET) {
										snprintf(temp, 511, "STATUS: Unable to accept an incoming connection on port %d", ci->port);
										gui_status(temp);
									} else {
										snprintf(temp, 511, "STATUS: Accepted an incoming connection on port %d", ci->port);
										StartTimer(&g_transfer_start_timer);
										StartTimer(&g_last_packet_transfered_timer);
										gui_status(temp);
										cn->ci.b_transfering = true;
										cn->ci.b_transfered = false;
									}
									
									cn->ci.b_waitingfordataport = false;
									CloseListenSocket(cn);
								}
							}
						}
					} else if (sel_sock < 0) {
						local_errno = errno;
						#ifdef DEBUGLV3
						DebugOutput("\nMainLoop(): sel_sock < 0\n");
						#endif
						
						optlen = sizeof(ret);
						getsockopt (ci->cmd_socket, SOL_SOCKET, SO_ERROR, (void *) &ret, (void *) &optlen);
						#ifdef DEBUGLV3
						printf("\nMainLoop(): Error code(ret) = %d string = '%s'\n", ret, GetErrnoDesc(ret));
						printf("\nMainLoop(): Error code(errno) = %d string = '%s'\n", local_errno, GetErrnoDesc(local_errno));
						printf("\nMainLoop(): Error code(sel_sock) = %d string = '%s'\n", sel_sock, GetErrnoDesc(sel_sock));
						#endif
					} else if (sel_sock == 0) {
						#ifdef DEBUGLV3
						printf("select() timeout expired, WaitSelect() sigmask = %ld sigmask = %08x\n", sigs, sigs);
						#endif
					}
				}
				
				if (sigs & SIGBREAKF_CTRL_C) {
					#ifdef DEBUG
					DebugOutput("MainLoop(): SIGBREAKF_CTRL_C\n");
					#endif
					break;
				}

		if (cn->ci.b_waitingfordataport && TimeInSecs(&g_listen_timer) > g_GlobalSettings.SettingsArray[SETTING_CONNECT_TIMEOUT]) {
			CloseListenSocket(cn);
			gui_status("STATUS: No connection happened in a timely fashion, closed listen socket");
		}

		if (g_quit_now)
			break;
		if (!gui_handle(sigs))
			break;
		if (g_quit_now)
			break;
	}

	if (!core_save_config())
		gui_status("ERROR: cannot save the configuration file");
}

/* ---------------------------------------------------------------------------
 * Actions requested by the GUI
 * ------------------------------------------------------------------------- */
bool core_is_connected(void) { return g_Connection.ci.b_connected; }
const char *core_local_path(void) { return g_Connection.lv.CurrentPath; }
const char *core_remote_path(void) { return g_Connection.rv.CurrentPath; }
int core_queue_first(void) { return g_Connection.queue_cur; }
int core_queue_used(void) { return g_Connection.queue_used; }
QueueItem *core_queue_item(int index) { return &g_Connection.QueuedItems[index]; }

void core_connect_toggle(void)
{
	Connection *cn = &g_Connection;

	if (cn->ci.b_connected == true)
		FtpDisconnect(cn);
	else if (cn->ci.b_connecting == false)
		FtpConnect(cn);
	else
		gui_status("STATUS: Already trying to connect");
}

void core_connect_host(int index)
{
	Connection *cn = &g_Connection;
	HostInfo *hi = &cn->hi;
	Host *hst;

	if (index < 0 || index >= g_AddressBook.used_hosts)
		return;
	hst = &g_AddressBook.Hosts[index];
	caf_strncpy(hi->hostname, hst->hostname, HOSTNAME_CONFIG_LEN);
	caf_strncpy(hi->ip_s, hst->hostname, HOSTNAME_CONFIG_LEN);
	hi->port = caf_atol(hst->port);
	caf_strncpy(hi->username, hst->username, USERNAME_CONFIG_LEN);
	caf_strncpy(hi->password, hst->password, PASSWORD_CONFIG_LEN);
	hi->conn_type = hst->conn_type;

	if (cn->ci.b_connected)
		gui_status("ERROR: Disconnect first!");
	else
		FtpConnect(cn);
}

void core_stop(void)
{
	Connection *cn = &g_Connection;

	cn->ci.b_processing_queue = false;
	if (cn->ci.b_transfering) {
		AbortFileTransfer(cn);
	} else if (cn->ci.b_connecting) {
		InvalidateSocket(&cn->ci.cmd_socket);
		cn->ci.b_connecting = false;
		gui_status("STATUS: Connection aborted");
	} else
		gui_status("STATUS: Nothing to abort");
}

void core_request_quit(void)
{
	Connection *cn = &g_Connection;

	if (cn->ci.b_connected == false) {
		g_quit_now = true;
	} else if (cn->ci.b_transfering) {
		AbortFileTransfer(cn);
		cn->ci.b_request_disconn = true;
		cn->ci.b_request_quit = true;
	} else if (cn->ci.b_can_send_command) {
		SendFtpCommand(cn, "QUIT", false);
		cn->ci.b_request_quit = true;
	} else {
		/* the server has not answered the previous command yet */
		CleanSockAndFlags(cn);
		g_quit_now = true;
	}
}

void core_local_refresh(void)
{
	if (RefreshLocalView(&g_Connection))
		gui_status("STATUS: Refreshed local view");
}

void core_remote_refresh(void)
{
	InitiateListTransfer(&g_Connection);
}

void core_set_local_path(const char *path)
{
	Connection *cn = &g_Connection;
	char old[512];

	caf_strncpy(old, cn->lv.CurrentPath, sizeof(old));
	caf_strncpy(cn->lv.CurrentPath, (char *) path, sizeof(cn->lv.CurrentPath));
	if (!RefreshLocalView(cn)) {
		caf_strncpy(cn->lv.CurrentPath, old, sizeof(cn->lv.CurrentPath));
		gui_set_path(GUI_LOCAL, cn->lv.CurrentPath);
	} else
		gui_status("STATUS: Refreshed local view");
}

void core_set_remote_path(const char *path)
{
	Connection *cn = &g_Connection;

	if (cn->ci.b_can_send_command)
		SendCWDCMD(cn, (char *) path);
	else
		gui_status(cn->ci.b_connected ? STR_CANT_SEND : STR_CANT_SEND_NOT_CONNECTED);
}

/* Parent of a local path, without locks (ParentDir()+NameFromLock() is V36):
   "DH0:a/b/" -> "DH0:a/", "DH0:a/" -> "DH0:" */
void core_local_parent(void)
{
	Connection *cn = &g_Connection;
	char path[512];
	int len, i;

	caf_strncpy(path, cn->lv.CurrentPath, sizeof(path));
	len = caf_strlen(path);
	if (len && path[len - 1] == '/')
		path[--len] = 0;
	for (i = len - 1; i >= 0; i--)
		if (path[i] == '/' || path[i] == ':')
			break;
	if (i < 0 || (path[i] == ':' && i == len - 1))
		return;			/* already a root */
	path[i + 1] = 0;
	core_set_local_path(path);
}

void core_remote_parent(void)
{
	Connection *cn = &g_Connection;

	if (cn->ci.b_can_send_command)
		SendFtpCommand(cn, "CDUP", false);
	else
		gui_status(cn->ci.b_connected ? STR_CANT_SEND : STR_CANT_SEND_NOT_CONNECTED);
}

void core_local_open(int index)
{
	Connection *cn = &g_Connection;
	ViewColumn *vc;
	char temp[600];

	if (index < 0 || index >= cn->lv.count)
		return;
	vc = cn->lv.entries[index];
	if (vc->is_dir) {
		if (!strcmp(vc->name, ".."))
			core_local_parent();
		else {
			snprintf(temp, 511, "%s%s", cn->lv.CurrentPath, vc->name);
			core_set_local_path(temp);
		}
	} else
		InitiateFileTransfer(cn, vc->name, false);		/* upload */
}

void core_remote_open(int index)
{
	Connection *cn = &g_Connection;
	ViewColumn *vc;
	char temp[600];
	int chr;

	if (index < 0 || index >= cn->rv.count)
		return;
	vc = cn->rv.entries[index];
	if (vc->is_dir) {
		if (!strcmp(vc->name, ".."))
			core_remote_parent();
		else {
			chr = caf_getlastchar(cn->rv.CurrentPath);
			if (chr == ':' || chr == '/')
				snprintf(temp, 511, "CWD %s%s", cn->rv.CurrentPath, vc->name);
			else
				snprintf(temp, 511, "CWD %s/%s", cn->rv.CurrentPath, vc->name);
			SendFtpCommand(cn, temp, false);
		}
	} else
		InitiateFileTransfer(cn, vc->name, true);		/* download */
}

static void set_queue_base_path(Connection *cn)
{
	caf_strncpy(cn->QueueBasePath, cn->rv.CurrentPath, sizeof(cn->QueueBasePath));
	if (caf_getlastchar(cn->QueueBasePath) == '/')
		cn->QueueBasePath[caf_strlen(cn->QueueBasePath) - 1] = 0;
}

static int g_sel[MAX_VIEW_ENTRIES];

void core_download_selected(void)
{
	Connection *cn = &g_Connection;
	ViewColumn *vc;
	int n, i;

	if (cn->ci.b_connected == false) {
		gui_status("ERROR: Not connected");
		return;
	}
	set_queue_base_path(cn);

	n = gui_get_selection(GUI_REMOTE, g_sel, MAX_VIEW_ENTRIES);
	if (n == 0) {
		gui_status("STATUS: Select the remote files to download");
		return;
	}
	if (n == 1) {
		core_remote_open(g_sel[0]);		/* no need to queue */
		return;
	}

	QueueClear(cn);
	BufferOpen(&cn->queue_buffer, true);
	for (i = 0; i < n; i++) {
		if (g_sel[i] < 0 || g_sel[i] >= cn->rv.count)
			continue;
		vc = cn->rv.entries[g_sel[i]];
		if (!strcmp(vc->name, ".."))
			continue;
		if (!QueuePushAddStr(cn, &cn->queue_buffer, vc->name, vc->is_dir ? CMD_REMOTE_DIR_SCAN : CMD_DOWNLOAD, 0, 0, false))
			break;
		if (vc->is_dir && !QueuePushAddStr(cn, &cn->queue_buffer, vc->name, CMD_LOCAL_MKDIR, 0, 0, true))
			break;
	}
	BufferClose(&cn->queue_buffer);
	gui_queue_changed();
}

void core_upload_selected(void)
{
	Connection *cn = &g_Connection;
	bool b_firstfile = true, b_done_dirs = false;
	char temp[600], local_path[512];
	FileView *lv = &cn->lv;
	ViewColumn *vc;
	int n, i, offset;

	set_queue_base_path(cn);
	caf_strncpy(local_path, lv->CurrentPath, sizeof(local_path));

	n = gui_get_selection(GUI_LOCAL, g_sel, MAX_VIEW_ENTRIES);
	if (n == 0) {
		gui_status("STATUS: Select the local files to upload");
		return;
	}
	if (n == 1 && g_sel[0] >= 0 && g_sel[0] < lv->count && !lv->entries[g_sel[0]]->is_dir) {
		InitiateFileTransfer(cn, lv->entries[g_sel[0]]->name, false);
		return;
	}

	BufferFlush(&cn->temp_buffer);
	cn->stack_cur = STACK_SIZE;
	QueueClear(cn);
	BufferOpen(&cn->queue_buffer, true);

	for (i = 0; i < n; i++) {
		if (g_sel[i] < 0 || g_sel[i] >= lv->count)
			continue;
		vc = lv->entries[g_sel[i]];
		if (vc->is_dir) {
			b_done_dirs = true;
			if (!strcmp(vc->name, ".."))
				continue;

			/* remote makedir */
			snprintf(temp, 511, "%s/%s", cn->QueueBasePath, vc->name);
			if (!QueuePushAddStr(cn, &cn->queue_buffer, temp, CMD_REMOTE_MKDIR, 0, 0, false))
				break;

			/* local directory scan: LocalDirScan() takes the relative path from
			   &curdir[base_start+1], base_start is the index of the separator */
			caf_strncpy(temp, lv->CurrentPath, 511);
			offset = caf_strlen(temp) - 1;
			caf_strncat(temp, vc->name, 512);
			if (LocalDirScan(cn, temp, lv->CurrentPath, offset) == false)
				break;
		} else {
			if (b_firstfile && b_done_dirs) {
				/* after the directories, back to the starting paths */
				b_firstfile = false;
				if (!QueuePushAddStr(cn, &cn->queue_buffer, local_path, CMD_LOCAL_BASEPATH, 0, 0, true))
					break;
				if (cn->QueueBasePath[0] == 0)
					caf_strncpy(cn->QueueBasePath, "/", 512);
				if (!QueuePushAddStr(cn, &cn->queue_buffer, cn->QueueBasePath, CMD_REMOTE_CHDIR, 0, 0, false))
					break;
			}
			if (!QueuePushAddStr(cn, &cn->queue_buffer, vc->name, CMD_UPLOAD, 0, 0, false))
				break;
		}
	}
	/* the files of the queue are read from this directory (LocalBasePath()) */
	caf_strncpy(g_QueueLocalPath, local_path, sizeof(g_QueueLocalPath));
	BufferClose(&cn->queue_buffer);
	gui_queue_changed();
}

void core_delete_selected(void)
{
	Connection *cn = &g_Connection;
	ViewColumn *vc;
	char *ptr;
	int n, i;

	n = gui_get_selection(GUI_REMOTE, g_sel, MAX_VIEW_ENTRIES);
	if (n == 0) {
		gui_status("STATUS: Select the remote files to delete");
		return;
	}
	if (n == 1) {
		if (g_sel[0] >= 0 && g_sel[0] < cn->rv.count && strcmp(cn->rv.entries[g_sel[0]]->name, ".."))
			InitiateFileDeletion(cn, cn->rv.entries[g_sel[0]]->name);
		return;
	}

	QueueClear(cn);
	BufferOpen(&cn->queue_buffer, true);
	for (i = 0; i < n; i++) {
		if (g_sel[i] < 0 || g_sel[i] >= cn->rv.count)
			continue;
		vc = cn->rv.entries[g_sel[i]];
		if (!strcmp(vc->name, ".."))
			continue;
		ptr = BufferAddStr(&cn->queue_buffer, vc->name);
		if (!ptr) {
			gui_status("ERROR: Queue buffer is full cannot continue");
			break;
		}
		if (QueuePush(cn, CMD_REMOTE_DELETE, ptr, 0, 0, false) == false)
			break;
	}
	BufferClose(&cn->queue_buffer);
	gui_queue_changed();
}

void core_queue_run(void)
{
	Connection *cn = &g_Connection;

	if (!cn->ci.b_processing_queue)
		caf_strncpy(g_QueueLocalPath, cn->lv.CurrentPath, sizeof(g_QueueLocalPath));
	cn->ci.b_processing_queue = true;
	cn->ci.b_queue_step = false;
	QueueAdvance(cn);
}

void core_queue_step(void)
{
	Connection *cn = &g_Connection;

	if (!cn->ci.b_processing_queue)
		caf_strncpy(g_QueueLocalPath, cn->lv.CurrentPath, sizeof(g_QueueLocalPath));
	cn->ci.b_processing_queue = true;
	cn->ci.b_queue_step = true;
	cn->ci.queue_to_step++;
	QueueAdvance(cn);
}

void core_queue_clear(void)
{
	QueueClear(&g_Connection);
	gui_queue_changed();
}

/* ---------------------------------------------------------------------------
 * Address book and settings
 * ------------------------------------------------------------------------- */
int core_host_count(void) { return g_AddressBook.used_hosts; }

Host *core_host_get(int index)
{
	if (index < 0 || index >= g_AddressBook.used_hosts)
		return 0;
	return &g_AddressBook.Hosts[index];
}

int core_host_add(const Host *h)
{
	if (g_AddressBook.used_hosts >= MAX_HOSTS)
		return -1;
	g_AddressBook.Hosts[g_AddressBook.used_hosts] = *h;
	return g_AddressBook.used_hosts++;
}

void core_host_set(int index, const Host *h)
{
	if (index >= 0 && index < g_AddressBook.used_hosts)
		g_AddressBook.Hosts[index] = *h;
}

void core_host_delete(int index)
{
	int i;

	if (index < 0 || index >= g_AddressBook.used_hosts)
		return;
	for (i = index; i < g_AddressBook.used_hosts - 1; i++)
		g_AddressBook.Hosts[i] = g_AddressBook.Hosts[i + 1];
	g_AddressBook.used_hosts--;
	caf_memset(&g_AddressBook.Hosts[g_AddressBook.used_hosts], 0, sizeof(Host));
}

int core_get_setting(int which)
{
	return (which >= 0 && which < NUM_OF_SETTINGS) ? g_GlobalSettings.SettingsArray[which] : 0;
}

void core_set_setting(int which, int value)
{
	if (which >= 0 && which < NUM_OF_SETTINGS)
		g_GlobalSettings.SettingsArray[which] = value;
}
