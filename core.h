/*
 * CORE.H - MarranoFTP core: FTP protocol, sockets, transfer queue, config.
 *
 * Nothing in here knows about the user interface: the core talks to the GUI
 * only through the functions declared in gui.h, and the GUIs (gui_mui.c for
 * MUI, gui_gt.c for GadTools / OS 1.3) drive the core only through the
 * core_* functions declared at the end of this file.
 *
 * The core uses OS 1.3 compatible calls only (no dos/exec V36+ functions, no
 * iffparse.library, no floating point), so the same file builds MarranoFTP
 * (MUI) and MarranoFTP13 (GadTools emulation, Kickstart 1.3+).
 */

#ifndef MFTP_CORE_H
#define MFTP_CORE_H

#include <exec/types.h>
#include <dos/dos.h>
#include <caf/caf_types.h>
#include <caf/networking_helpers.h>

/* ---------------------------------------------------------------------------
 * Sizes. The MUI build keeps the historical (large) buffers, the OS 1.3
 * build (MFTP_SMALL) fits a 1 MB Amiga.
 * ------------------------------------------------------------------------- */
#ifdef MFTP_SMALL
#define COMMAND_BUFFER_SIZE		4096
#define TRANSFER_BUFFER_SIZE	(96*1024)		/* also holds a whole LIST */
#define LIST_BUFFER_SIZE		(48*1024)
#define QUEUE_BUFFER_SIZE		(32*1024)
#define TEMP_BUFFER_SIZE		(16*1024)
#define QUEUE_SIZE				1024
#define STACK_SIZE				512
#define MAX_LIST_LINES			2048
#define MAX_VIEW_ENTRIES		1024
#else
#define COMMAND_BUFFER_SIZE		4096
#define TRANSFER_BUFFER_SIZE	1048572
#define LIST_BUFFER_SIZE		1048572
#define QUEUE_BUFFER_SIZE		(512*1024)
#define TEMP_BUFFER_SIZE		(256*1024)
#define QUEUE_SIZE				8192
#define STACK_SIZE				8192
#define MAX_LIST_LINES			16384
#define MAX_VIEW_ENTRIES		8192
#endif

#define RECV_CHUNK				8192		/* bytes per recv()/Read() call */

/* ---------------------------------------------------------------------------
 * SockConnect() API
 * ------------------------------------------------------------------------- */
enum {
	CSOCKERR_NO_ERROR = 0,
	CSOCKERR_NATIVE,
	CSOCKERR_BINDFAILED,
	CSOCKERR_SOCKCREATFAILED,
	CSOCKERR_SETNONBLOCKFAILED,
	CSOCKERR_WRONGPARAMS
};

enum {
	CSOCKSTATUS_EARLYCONNECT = 0,
	CSOCKSTATUS_PENDING,
	CSOCKSTATUS_ERROR
};
#define CSOCKSTATUS_OK CSOCKSTATUS_EARLYCONNECT

typedef struct CSOCK {
	int ip_i;		/* [IN ] ip to connect to (network order) */
	int port;		/* [IN ] port */
	int socket;		/* [OUT] created socket */
	int csockerr;	/* [OUT] custom error */
	int sockerr;	/* [OUT] native socket error */
} CSOCK;

/* ---------------------------------------------------------------------------
 * Enumerations
 * ------------------------------------------------------------------------- */
enum {
	FTP_CMD_CONNECT, FTP_CMD_DISCONNECT, FTP_CMD_QUIT,
	CMD_DOWNLOAD, CMD_UPLOAD, CMD_REMOTE_DELETE, CMD_RENAME,
	CMD_LOCAL_DIR_SCAN, CMD_REMOTE_DIR_SCAN,
	CMD_LOCAL_MKDIR, CMD_REMOTE_MKDIR,
	CMD_LOCAL_BASEPATH, CMD_REMOTE_CHDIR,
	STATUS_QUEUED, STATUS_COMPLETED, STATUS_ERROR
};

enum {
	SELECT_ERROR = -2,
	ANSWER_PENDING,
	NO_ERRORS,
	COMMAND_PENDING,
	NON_BLOCKING_COMMAND,
	NOT_CONNECTED,
	FLOW_CONTROL_ERROR
};

enum { CTYPE_ACTV, CTYPE_PASV, CTYPE_SFTP };
enum { MAX_HOSTS = 128 };
enum { OPT_DOWNLOAD_DELETE_PARTIAL, OPT_DOWNLOAD_KEEP_PARTIAL };
enum { NUM_OF_SETTINGS = 6 };
enum { OPT_QUEUE_STOP_ON_ERROR, OPT_QUEUE_PROCESS_ALL };
enum { SETTING_SOCKET_TIMEOUT, SETTING_TRANSFER_TIMEOUT, SETTING_CONNECT_TIMEOUT,
	   SETTING_QUEUE_TYPE, SETTING_KEEP_PARTIAL, SETTING_LOCAL_REFRESH_AFTER_RETR };
enum { OPT_NO_REFRESH_AFTER_RETR, OPT_REFRESH_AFTER_RETR };
enum { CONFIG_VERSION1 = 0x100 };

/* ---------------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------------- */

/* Time stamps in seconds + microseconds (no floating point) */
typedef struct TimerVal {
	ULONG start_s, start_us;
	ULONG cur_s, cur_us;
} TimerVal;

/* One line of the local or remote file list */
typedef struct ViewColumn {
	char *name;
	char *size;
	char *flags;
	char *date;
	bool is_dir;
} ViewColumn;

/* One line of the transfer queue */
typedef struct QueueColumn {
	int command;
	char *name;
	int status;
} QueueColumn;

typedef struct ClientInfo {
	char hostname[512];
	char filename[512];
	char ip_s[32];
	int ip_i;
	int port;
	SOCKET cmd_socket;			/* FTP commands */
	SOCKET listen_socket;		/* active mode: data connections are accepted here */
	SOCKET transfer_socket;		/* data connection */
	int bytes_transfered;
	int last_transfer_in_bytes;
	BPTR filehandle;
	bool b_eof;
	int file_size, file_readed, file_sent;
	int queue_step, queue_to_step;

	bool	b_file_transfer,		/* a file (not a LIST) is being transferred */
			b_file_download,		/* download, otherwise upload */
			b_connected,
			b_connecting,
			b_waitingfordataport,
			b_transfer_error,
			b_passive,
			b_transfering,
			b_transfered,
			b_can_send_command,
			b_directory_processed,
			b_user_accepted,
			b_pass_accepted,
			b_request_list,
			b_request_download,
			b_request_upload,
			b_request_conn,
			b_request_disconn,
			b_request_quit,
			b_last_data,
			b_processing_queue,
			b_queue_step;
} ClientInfo;

#define ENTRYNAME_CONFIG_LEN 128
#define HOSTNAME_CONFIG_LEN 128
#define PORT_COFING_LEN 8
#define USERNAME_CONFIG_LEN 32
#define PASSWORD_CONFIG_LEN 32

typedef struct Host {
	char entryname[ENTRYNAME_CONFIG_LEN];
	char hostname[HOSTNAME_CONFIG_LEN];
	char port[PORT_COFING_LEN];
	char username[USERNAME_CONFIG_LEN];
	char password[PASSWORD_CONFIG_LEN];
	int conn_type;
} Host;

typedef struct HostInfo {
	char hostname[HOSTNAME_CONFIG_LEN];
	char ip_s[HOSTNAME_CONFIG_LEN];
	int ip_i;
	int port;
	int conn_type;
	CSOCK pasvsettings;
	char username[USERNAME_CONFIG_LEN];
	char password[PASSWORD_CONFIG_LEN];
} HostInfo;

typedef struct buffer {
	char *ptr;
	int curpos;
	int size;
	bool b_opened;
} buffer;

/* A file list as shown by the GUI: entries[0..count-1], ".." first, then the
   directories, then the files */
typedef struct FileView {
	buffer ListBuffer;					/* strings and ViewColumn structures */
	ViewColumn *entries[MAX_VIEW_ENTRIES];
	int count;
	char CurrentPath[512];
} FileView;

typedef struct QueueItem {
	int command;
	char *data;
	char *c_arg1;
	int i_arg1;
	bool b_local_cmd;		/* local commands run without waiting for the server */
} QueueItem;

typedef struct StackItem {
	char *data;
} StackItem;

typedef struct Connection {
	ClientInfo ci;
	HostInfo hi;
	FileView lv;							/* local */
	FileView rv;							/* remote */
	int num_lines;							/* remote LIST parsing */
	int lines_start[MAX_LIST_LINES];
	char QueueBasePath[512];				/* remote path when queuing started */

	buffer transfer_buffer, cmd_buffer, queue_buffer, temp_buffer;

	QueueItem QueuedItems[QUEUE_SIZE+1];
	int queue_cur, queue_used, last_queue_update;

	StackItem StackStack[STACK_SIZE+1];
	int stack_cur;
} Connection;

/* Address book and settings: data only, the windows belong to the GUIs */
typedef struct AddressBook {
	Host Hosts[MAX_HOSTS];
	int used_hosts;
} AddressBook;

typedef struct GlobalSettings {
	int SettingsArray[NUM_OF_SETTINGS];
} GlobalSettings;

extern Connection g_Connection;
extern AddressBook g_AddressBook;
extern GlobalSettings g_GlobalSettings;

/* ---------------------------------------------------------------------------
 * Core API, called by the GUIs
 * ------------------------------------------------------------------------- */

/* Life cycle: core_init() opens the network/timer resources (no GUI calls),
   then the GUI opens, then core_start() loads the configuration and fills the
   local list (it talks to the GUI). core_run() is the main loop: it
   waits on the sockets and on the GUI signals, it returns when the user quit
   (the configuration is saved). core_cleanup() frees everything. */
bool core_init(void);
void core_start(void);					/* after gui_open(): config + local list */
void core_run(void);
void core_cleanup(void);

/* Toolbar / menu actions */
void core_connect_toggle(void);			/* Connect / Disconnect button */
void core_connect_host(int index);		/* connect to an address book entry */
void core_stop(void);
void core_request_quit(void);			/* polite quit: QUIT if connected */
void core_local_refresh(void);
void core_remote_refresh(void);
void core_set_local_path(const char *path);	/* user typed a local path */
void core_set_remote_path(const char *path);/* user typed a remote path: CWD */
void core_local_open(int index);		/* double click: enter dir or upload */
void core_remote_open(int index);		/* double click: CWD or download */
void core_local_parent(void);
void core_remote_parent(void);
void core_download_selected(void);
void core_upload_selected(void);
void core_delete_selected(void);
void core_queue_run(void);
void core_queue_step(void);
void core_queue_clear(void);

/* Address book / settings */
int  core_host_count(void);
Host *core_host_get(int index);
int  core_host_add(const Host *h);		/* returns the new index or -1 */
void core_host_set(int index, const Host *h);
void core_host_delete(int index);
int  core_get_setting(int which);
void core_set_setting(int which, int value);
bool core_save_config(void);

/* State, for the GUIs */
bool core_is_connected(void);
const char *core_local_path(void);
const char *core_remote_path(void);
int  core_queue_first(void);			/* QueuedItems[first..used-1] are pending */
int  core_queue_used(void);
QueueItem *core_queue_item(int index);

#endif
