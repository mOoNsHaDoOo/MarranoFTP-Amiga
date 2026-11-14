/*
 * CORE_SFTP.C - SFTP sessions on the Amiga, see core_sftp.h.
 *
 * Host keys: "trust on first use". The first time a server is seen its key
 * fingerprint is saved in PROGDIR:marranoftp.hosts (S:marranoftp.hosts if
 * PROGDIR: cannot be written); later connections are refused if the key
 * changed. Lines: "host:port type SHA256:fingerprint".
 */
#ifdef __SASC
#define PROTO_TIMER_H 1				/* see ftp_core.c */
#endif
#include <exec/types.h>
#include <exec/exec.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>

/* the SDK header repeats an enum value (Warning 79) */
#pragma msg 79 ignore push
#include <libraries/amisslmaster.h>
#pragma msg 79 pop

#include <caf/caf_types.h>
#include <caf/thread_helpers.h>

#include "core.h"
#include "gui.h"
#include "ssh.h"
#include "sftp.h"
#include "core_sftp.h"

/* amisslmaster.library: declared here, the SDK clib header needs <utility/tagitem.h>
   types that SAS/C pragmas handle fine */
LONG OpenAmiSSLTags(LONG APIVersion, Tag tag, ...);
void CloseAmiSSL(void);
#include <pragmas/amisslmaster_pragmas.h>

#define AmiSSL_SocketBase			(TAG_USER + 0x01)
#define AmiSSL_ErrNoPtr				(TAG_USER + 0x0b)
#define AmiSSL_UsesOpenSSLStructs	(TAG_USER + 0x0c)
#define AmiSSL_GetAmiSSLBase		(TAG_USER + 0x0d)
#define AmiSSL_GetAmiSSLExtBase		(TAG_USER + 0x0e)
#define AmiSSL_InitAmiSSL			(TAG_USER + 0x10)

struct Library *AmiSSLMasterBase, *AmiSSLBase, *AmiSSLExtBase;
extern struct Library *SocketBase;

static bool g_amissl_open;

typedef struct Session {
	LONG sock;
	Ssh *ssh;
	char host[140];
	int port;
} Session;

/* one SFTP session at a time */
static Session g_sess = { -1, NULL };

/* ---------------------------------------------------------------------------
 * AmiSSL
 * ------------------------------------------------------------------------- */
static bool amissl_open(void)
{
	LONG err;

	if (g_amissl_open)
		return true;
	if (SysBase->LibNode.lib_Version < 39) {
		gui_status("ERROR: SFTP needs AmigaOS 3.0+, a 68020+ and AmiSSL 5.25+");
		return false;
	}
	AmiSSLMasterBase = OpenLibrary((STRPTR) "amisslmaster.library", AMISSLMASTER_MIN_VERSION);
	if (!AmiSSLMasterBase) {
		gui_status("ERROR: SFTP needs AmiSSL 5.25+ (amisslmaster.library not found)");
		return false;
	}
	/* no AmiSSL_SocketBase: ssh.c does its own network I/O, AmiSSL is used
	   only for the cryptography (and some stacks do not like being queried) */
	err = OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
						 AmiSSL_UsesOpenSSLStructs, FALSE,
						 AmiSSL_InitAmiSSL, TRUE,
						 AmiSSL_GetAmiSSLBase, (ULONG) &AmiSSLBase,
						 AmiSSL_GetAmiSSLExtBase, (ULONG) &AmiSSLExtBase,
						 TAG_DONE);
	if (err != 0) {
		char temp[120];

		/* 1: amisslmaster too old, 2: LIBS:AmiSSL/amissl_v3xx.library missing
		   or not enough memory (it takes about 3.5 MB), 3: initialisation */
		sprintf(temp, err == 2 ? "ERROR: cannot open AmiSSL: LIBS:AmiSSL/amissl_v3xx.library missing or not enough free memory (3.5 MB)"
				: "ERROR: cannot open AmiSSL 5.25+ (error %ld)", err);
		gui_status(temp);
		CloseLibrary(AmiSSLMasterBase);
		AmiSSLMasterBase = NULL;
		return false;
	}
	g_amissl_open = true;
	return true;
}

void csftp_cleanup(void)
{
	if (g_sess.ssh)
		csftp_close(NULL);
	if (g_amissl_open)
		CloseAmiSSL();
	if (AmiSSLMasterBase)
		CloseLibrary(AmiSSLMasterBase);
	AmiSSLMasterBase = AmiSSLBase = AmiSSLExtBase = NULL;
	g_amissl_open = false;
}

/* ---------------------------------------------------------------------------
 * Socket
 * ------------------------------------------------------------------------- */
static int wait_socket(LONG sock, bool for_write, int seconds)
{
	fd_set fds;
	struct timeval tv;
	ULONG sigs = SIGBREAKF_CTRL_C;
	int r;

	FD_ZERO(&fds);
	FD_SET(sock, &fds);
	tv.tv_secs = seconds;
	tv.tv_micro = 0;
	r = WaitSelect(sock + 1, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL, &tv, &sigs);
	return r > 0 && FD_ISSET(sock, &fds);
}

static int io_send(void *ctx, const unsigned char *buf, int len)
{
	Session *s = (Session *) ctx;

	return (int) send(s->sock, (APTR) buf, len, 0);
}

static int io_recv(void *ctx, unsigned char *buf, int len)
{
	Session *s = (Session *) ctx;
	int t = core_get_setting(SETTING_TRANSFER_TIMEOUT);

	if (t < 5)
		t = 5;
	if (!wait_socket(s->sock, false, t))
		return -1;
	return (int) recv(s->sock, (APTR) buf, len, 0);
}

static LONG tcp_connect(const char *host, int port)
{
	struct sockaddr_in sa;
	struct hostent *he;
	LONG sock, one = 1, zero = 0, err = 0, len = sizeof(err);
	int t = core_get_setting(SETTING_CONNECT_TIMEOUT);
	char temp[200];

	caf_memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short) port);
	he = gethostbyname((STRPTR) host);
	if (he)
		CopyMem(he->h_addr, &sa.sin_addr, 4);
	else
		sa.sin_addr.s_addr = inet_addr((STRPTR) host);
	if (sa.sin_addr.s_addr == 0 || sa.sin_addr.s_addr == 0xffffffffUL) {
		gui_status("ERROR: cannot resolve the host name");
		return -1;
	}
	sprintf(temp, "STATUS: Connecting to %s port %d (SFTP)", host, port);
	gui_status(temp);
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		gui_status("ERROR: cannot create a socket");
		return -1;
	}
	/* non blocking connect, with a timeout */
	IoctlSocket(sock, FIONBIO, (char *) &one);
	if (connect(sock, (struct sockaddr *) &sa, sizeof(sa)) < 0 && errno != EINPROGRESS) {
		gui_status("ERROR: connection refused");
		CloseSocket(sock);
		return -1;
	}
	if (t < 5)
		t = 5;
	if (!wait_socket(sock, true, t)) {
		gui_status("ERROR: connection timed out");
		CloseSocket(sock);
		return -1;
	}
	getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *) &err, (void *) &len);
	if (err) {
		gui_status("ERROR: connection refused");
		CloseSocket(sock);
		return -1;
	}
	IoctlSocket(sock, FIONBIO, (char *) &zero);
	return sock;
}

/* ---------------------------------------------------------------------------
 * Known host keys
 * ------------------------------------------------------------------------- */
#define HOSTS_MAX 8192

static const char *hosts_file(int i)
{
	return i == 0 ? "PROGDIR:marranoftp.hosts" : "S:marranoftp.hosts";
}

/* the saved "type fingerprint" of host:port, "" if none */
static void hosts_find(const char *key, char *found, int size)
{
	static char buf[HOSTS_MAX];
	BPTR fh;
	LONG n;
	int i, k, klen = strlen(key);
	struct Process *me = (struct Process *) FindTask(NULL);
	APTR oldwin = me->pr_WindowPtr;

	found[0] = 0;
	me->pr_WindowPtr = (APTR) -1;				/* no "insert volume" requesters */
	for (i = 0; i < 2 && !found[0]; i++) {
		fh = Open((STRPTR) hosts_file(i), MODE_OLDFILE);
		if (!fh)
			continue;
		n = Read(fh, buf, HOSTS_MAX - 1);
		Close(fh);
		if (n <= 0)
			continue;
		buf[n] = 0;
		for (k = 0; k < n; ) {
			int e = k;

			while (e < n && buf[e] != '\n')
				e++;
			if (e - k > klen && !strncmp(buf + k, key, klen) && buf[k + klen] == ' ') {
				int l = e - k - klen - 1;

				if (l > size - 1)
					l = size - 1;
				memcpy(found, buf + k + klen + 1, l);
				found[l] = 0;
				break;
			}
			k = e + 1;
		}
	}
	me->pr_WindowPtr = oldwin;
}

static bool hosts_add(const char *line)
{
	BPTR fh;
	int i;
	bool ok = false;
	struct Process *me = (struct Process *) FindTask(NULL);
	APTR oldwin = me->pr_WindowPtr;

	me->pr_WindowPtr = (APTR) -1;
	for (i = 0; i < 2 && !ok; i++) {
		fh = Open((STRPTR) hosts_file(i), MODE_READWRITE);
		if (!fh)
			continue;
		Seek(fh, 0, OFFSET_END);
		ok = Write(fh, (APTR) line, strlen(line)) == (LONG) strlen(line);
		Close(fh);
	}
	me->pr_WindowPtr = oldwin;
	return ok;
}

static int check_hostkey(void *ctx, const char *type, const char *fingerprint)
{
	Session *s = (Session *) ctx;
	char key[160], want[120], have[120], line[300];

	sprintf(key, "%s:%d", s->host, s->port);
	sprintf(have, "%s %s", type, fingerprint);
	hosts_find(key, want, sizeof(want));
	if (!want[0]) {
		sprintf(line, "%s %s\n", key, have);
		gui_status(hosts_add(line) ? "STATUS: New SFTP server, its host key was saved in marranoftp.hosts"
				   : "WARNING: cannot save the host key in marranoftp.hosts");
		return 1;
	}
	if (!strcmp(want, have))
		return 1;
	gui_status("ERROR: THE HOST KEY OF THIS SERVER HAS CHANGED! Connection refused.");
	sprintf(line, "ERROR: saved %.100s", want);
	gui_status(line);
	gui_status("ERROR: if the change is expected, remove its line from marranoftp.hosts");
	return 0;
}

static void log_cb(void *ctx, const char *text)
{
	char temp[260];

	sprintf(temp, "STATUS: %.240s", text);
	gui_status(temp);
}

/* ---------------------------------------------------------------------------
 * Sessions
 * ------------------------------------------------------------------------- */
static Sftp *open_session(const char *host, int port, const char *user, const char *password);

Sftp *csftp_open(const char *host, int port, const char *user, const char *password)
{
	struct Process *me;
	APTR oldwin;
	Sftp *f;

	if (g_sess.ssh)
		csftp_close(NULL);
	/* OpenSSL reads its files from AmiSSL: (assigned by the AmiSSL
	   installer): without it no "insert volume" requester, it just goes on */
	me = (struct Process *) FindTask(NULL);
	oldwin = me->pr_WindowPtr;
	me->pr_WindowPtr = (APTR) -1;
	f = amissl_open() ? open_session(host, port, user, password) : NULL;
	me->pr_WindowPtr = oldwin;
	return f;
}

static Sftp *open_session(const char *host, int port, const char *user, const char *password)
{
	SshIo io;
	Sftp *f;
	char temp[300];

	caf_strncpy(g_sess.host, (char *) host, sizeof(g_sess.host));
	g_sess.port = port;
	g_sess.sock = tcp_connect(host, port);
	if (g_sess.sock < 0)
		return NULL;
	io.ctx = &g_sess;
	io.send = io_send;
	io.recv = io_recv;
	g_sess.ssh = ssh_new(&io, log_cb, check_hostkey, &g_sess);
	if (!g_sess.ssh) {
		gui_status("ERROR: not enough memory for SFTP");
		csftp_close(NULL);
		return NULL;
	}
	if (!ssh_connect(g_sess.ssh, user, password)) {
		sprintf(temp, "ERROR: SSH: %.250s", ssh_error(g_sess.ssh));
		gui_status(temp);
		csftp_close(NULL);
		return NULL;
	}
	f = sftp_new(g_sess.ssh);
	if (!f) {
		sprintf(temp, "ERROR: SFTP: %.250s", ssh_error(g_sess.ssh));
		gui_status(temp);
		csftp_close(NULL);
		return NULL;
	}
	gui_status("STATUS: SFTP session ready");
	return f;
}

void csftp_close(Sftp *f)
{
	if (f)
		sftp_free(f);
	if (g_sess.ssh) {
		ssh_disconnect(g_sess.ssh);
		ssh_free(g_sess.ssh);
		g_sess.ssh = NULL;
	}
	if (g_sess.sock >= 0) {
		CloseSocket(g_sess.sock);
		g_sess.sock = -1;
	}
}
