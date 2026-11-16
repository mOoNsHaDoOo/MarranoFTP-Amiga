/*
 * TLS.C - TLS for FTPS, see tls.h.
 *
 * Certificates: the chain is checked against the AmiSSL CA certificates
 * (AmiSSL:Certs) and the name against the host. If the check fails (self
 * signed certificate, IP address, unknown CA) the certificate is handled
 * like an SSH host key: its SHA256 fingerprint is saved the first time in
 * marranoftp.hosts and a different one later is refused.
 */
#include <string.h>
#include <stdio.h>

#if defined(__SASC) || defined(AMIGA)
#define PROTO_TIMER_H 1				/* see ftp_core.c */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <proto/exec.h>
#include <proto/socket.h>
#include <caf/caf_types.h>
#include "core.h"
#include "gui.h"
#include "core_sftp.h"				/* core_amissl_open(), core_known_host() */
#else
/* Linux test build (tests/test_ftps_linux.c provides these) */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>
#include <unistd.h>
void gui_status(const char *text);
int core_known_host(const char *host, int port, const char *type, const char *fingerprint);
#define core_amissl_open() 1
#endif

#include "ssh_crypto.h"
#include "tls.h"

#define MAX_TLS		3					/* control + data (+1 spare) */
#define IO_CHUNK	4096

typedef struct TlsConn {
	long sock;
	SSL *ssl;
	BIO *rb, *wb;						/* network -> SSL, SSL -> network */
	int used, ready;
} TlsConn;

static TlsConn g_tls[MAX_TLS];
static SSL_CTX *g_ctx;

/* ---------------------------------------------------------------------------
 * Platform
 * ------------------------------------------------------------------------- */
static int timeout_secs(void)
{
#if defined(__SASC) || defined(AMIGA)
	int t = core_get_setting(SETTING_CONNECT_TIMEOUT);

	return t < 10 ? 10 : t;
#else
	return 15;
#endif
}

/* 1 when the socket is readable/writable, 0 on timeout or error */
static int wait_io(long sock, int for_write, int secs)
{
	fd_set fds;
	struct timeval tv;
	int r;

	FD_ZERO(&fds);
	FD_SET(sock, &fds);
#if defined(__SASC) || defined(AMIGA)
	{
		ULONG sigs = SIGBREAKF_CTRL_C;

		tv.tv_secs = secs;
		tv.tv_micro = 0;
		r = WaitSelect(sock + 1, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL, &tv, &sigs);
	}
#else
	tv.tv_sec = secs;
	tv.tv_usec = 0;
	r = select((int) sock + 1, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL, &tv);
#endif
	return r > 0 && FD_ISSET(sock, &fds);
}

/* AmiSSL reads AmiSSL:Certs and its configuration from AmiSSL:, which
   may be missing: no "insert volume" requesters, it simply goes on */
#if defined(__SASC) || defined(AMIGA)
static APTR no_requesters(void)
{
	struct Process *me = (struct Process *) FindTask(NULL);
	APTR old = me->pr_WindowPtr;

	me->pr_WindowPtr = (APTR) -1;
	return old;
}

static void restore_requesters(APTR old)
{
	((struct Process *) FindTask(NULL))->pr_WindowPtr = old;
}
#else
typedef void *APTR;
#define no_requesters() NULL
#define restore_requesters(old) (void) (old)
#endif

static int would_block(void)
{
	return errno == EWOULDBLOCK || errno == EAGAIN;
}

static void set_would_block(void)
{
	errno = EWOULDBLOCK;
}

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */
static TlsConn *find(long sock)
{
	int i;

	if (sock < 0)
		return NULL;
	for (i = 0; i < MAX_TLS; i++)
		if (g_tls[i].used && g_tls[i].sock == sock)
			return &g_tls[i];
	return NULL;
}

static void ssl_error(const char *what)
{
	char text[200], err[120];
	unsigned long e = ERR_get_error();

	err[0] = 0;
	if (e)
		ERR_error_string_n(e, err, sizeof(err));
	sprintf(text, "ERROR: TLS %s%s%.120s", what, err[0] ? ": " : "", err);
	gui_status(text);
}

/* everything TLS wants to send goes to the socket */
static int flush_out(TlsConn *t)
{
	unsigned char buf[IO_CHUNK];
	int n, s, off;

	while ((n = BIO_read(t->wb, buf, sizeof(buf))) > 0) {
		for (off = 0; off < n; ) {
			s = (int) send(t->sock, (void *) (buf + off), n - off, 0);
			if (s > 0)
				off += s;
			else if (s < 0 && would_block()) {
				if (!wait_io(t->sock, 1, timeout_secs()))
					return 0;
			} else
				return 0;
		}
	}
	return 1;
}

/* socket -> SSL: >0 bytes, 0 end of connection, -1 nothing now, -2 error */
static int fill_in(TlsConn *t)
{
	unsigned char buf[IO_CHUNK];
	int r = (int) recv(t->sock, (void *) buf, sizeof(buf), 0);

	if (r > 0) {
		BIO_write(t->rb, buf, r);
		return r;
	}
	if (r == 0)
		return 0;
	return would_block() ? -1 : -2;
}

static int is_ip(const char *host)
{
	for (; *host; host++)
		if (!((*host >= '0' && *host <= '9') || *host == '.'))
			return 0;
	return 1;
}

static void base64(char *dst, const unsigned char *src, int n)
{
	static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int i, o = 0;
	unsigned long v;

	for (i = 0; i + 2 < n; i += 3) {
		v = ((unsigned long) src[i] << 16) | ((unsigned long) src[i + 1] << 8) | src[i + 2];
		dst[o++] = tab[(v >> 18) & 63];
		dst[o++] = tab[(v >> 12) & 63];
		dst[o++] = tab[(v >> 6) & 63];
		dst[o++] = tab[v & 63];
	}
	if (n - i == 2) {
		v = ((unsigned long) src[i] << 16) | ((unsigned long) src[i + 1] << 8);
		dst[o++] = tab[(v >> 18) & 63];
		dst[o++] = tab[(v >> 12) & 63];
		dst[o++] = tab[(v >> 6) & 63];
	} else if (n - i == 1) {
		v = (unsigned long) src[i] << 16;
		dst[o++] = tab[(v >> 18) & 63];
		dst[o++] = tab[(v >> 12) & 63];
	}
	dst[o] = 0;
}

/* ---------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */
static int init_ctx(void);

int tls_init(void)
{
	APTR old;
	int ok;

	if (g_ctx)
		return 1;
	old = no_requesters();
	ok = init_ctx();
	restore_requesters(old);
	return ok;
}

static int init_ctx(void)
{
	if (!core_amissl_open())
		return 0;
	g_ctx = SSL_CTX_new(TLS_client_method());
	if (!g_ctx) {
		ssl_error("context");
		return 0;
	}
	SSL_CTX_ctrl(g_ctx, SSL_CTRL_SET_MIN_PROTO_VERSION, TLS1_2_VERSION, NULL);
	SSL_CTX_ctrl(g_ctx, SSL_CTRL_MODE, (long) (SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER), NULL);
	/* the result of the check is read after the handshake (see above) */
	SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);
	SSL_CTX_set_default_verify_paths(g_ctx);
	return 1;
}

void tls_cleanup(void)
{
	int i;

	for (i = 0; i < MAX_TLS; i++)
		if (g_tls[i].used)
			tls_close(g_tls[i].sock);
	if (g_ctx)
		SSL_CTX_free(g_ctx);
	g_ctx = NULL;
}

static int check_certificate(TlsConn *t, const char *host, int port)
{
	X509 *cert = SSL_get1_peer_certificate(t->ssl);
	unsigned char md[32];
	unsigned int len = 0;
	char fp[64], text[260];
	long vr;

	if (!cert) {
		gui_status("ERROR: TLS: the server sent no certificate");
		return 0;
	}
	X509_digest(cert, EVP_sha256(), md, &len);
	X509_free(cert);
	strcpy(fp, "SHA256:");
	base64(fp + 7, md, 32);
	vr = SSL_get_verify_result(t->ssl);
	if (vr == X509_V_OK) {
		sprintf(text, "STATUS: TLS certificate verified, %s", fp);
		gui_status(text);
		return 1;
	}
	sprintf(text, "WARNING: TLS certificate not verified (%.100s), %s", X509_verify_cert_error_string(vr), fp);
	gui_status(text);
	return core_known_host(host, port, "x509", fp);
}

static int start(long sock, const char *host, int port, long reuse_from);

int tls_start(long sock, const char *host, int port, long reuse_from)
{
	APTR old;
	int ok;

	if (!tls_init())
		return 0;
	old = no_requesters();
	ok = start(sock, host, port, reuse_from);
	restore_requesters(old);
	return ok;
}

static int start(long sock, const char *host, int port, long reuse_from)
{
	TlsConn *t = NULL, *src;
	SSL_SESSION *sess;
	int i, n, e, r, secs = timeout_secs();

	tls_close(sock);
	for (i = 0; i < MAX_TLS && !t; i++)
		if (!g_tls[i].used)
			t = &g_tls[i];
	if (!t) {
		gui_status("ERROR: TLS: too many connections");
		return 0;
	}
	memset(t, 0, sizeof(TlsConn));
	t->sock = sock;
	t->ssl = SSL_new(g_ctx);
	t->rb = BIO_new(BIO_s_mem());
	t->wb = BIO_new(BIO_s_mem());
	if (!t->ssl || !t->rb || !t->wb) {
		ssl_error("setup");
		if (t->ssl)
			SSL_free(t->ssl);
		return 0;
	}
	SSL_set_bio(t->ssl, t->rb, t->wb);		/* the SSL owns the BIOs now */
	SSL_set_connect_state(t->ssl);
	t->used = 1;
	if (host && !is_ip(host)) {
		SSL_ctrl(t->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, (void *) host);
		X509_VERIFY_PARAM_set1_host(SSL_get0_param(t->ssl), host, 0);
	}
	src = find(reuse_from);
	if (src && src->ready && (sess = SSL_get1_session(src->ssl)) != NULL) {
		SSL_set_session(t->ssl, sess);
		SSL_SESSION_free(sess);
	}

	/* a data connection may still be connecting */
	if (!wait_io(sock, 1, secs)) {
		gui_status("ERROR: TLS: the connection did not open");
		tls_close(sock);
		return 0;
	}
	for (;;) {
		n = SSL_connect(t->ssl);
		if (!flush_out(t)) {
			gui_status("ERROR: TLS: send failed during the handshake");
			tls_close(sock);
			return 0;
		}
		if (n == 1)
			break;
		e = SSL_get_error(t->ssl, n);
		if (e == SSL_ERROR_WANT_READ) {
			if (!wait_io(sock, 0, secs)) {
				gui_status("ERROR: TLS: handshake timed out");
				tls_close(sock);
				return 0;
			}
			r = fill_in(t);
			if (r == 0 || r == -2) {
				gui_status("ERROR: TLS: connection closed during the handshake");
				tls_close(sock);
				return 0;
			}
		} else if (e != SSL_ERROR_WANT_WRITE) {
			ssl_error("handshake failed");
			tls_close(sock);
			return 0;
		}
	}
	t->ready = 1;
	if (host && !check_certificate(t, host, port)) {
		tls_close(sock);
		return 0;
	}
	return 1;
}

void tls_close(long sock)
{
	TlsConn *t = find(sock);
	int r, drained = 0, rounds = 0;

	if (!t)
		return;
	if (t->ready) {
		SSL_shutdown(t->ssl);			/* close_notify: the end of an upload */
		flush_out(t);
		/* wait for the server close_notify, reading what is left (TLS 1.3
		   session tickets...): closing a socket with unread data sends a
		   RST and the server may lose the end of an upload */
		while (drained < 65536 && rounds++ < 40 && wait_io(sock, 0, 3)) {
			r = fill_in(t);
			if (r == 0 || r == -2)
				break;
			if (r > 0) {
				char sink[IO_CHUNK];

				drained += r;
				while (SSL_read(t->ssl, sink, sizeof(sink)) > 0)
					;
				if (SSL_shutdown(t->ssl) == 1)
					break;
			}
		}
	}
	SSL_free(t->ssl);
	memset(t, 0, sizeof(TlsConn));
}

int tls_active(long sock)
{
	return find(sock) != NULL;
}

int tls_pending(long sock)
{
	TlsConn *t = find(sock);

	return t && t->ready && SSL_has_pending(t->ssl);
}

int tls_recv(long sock, void *buf, int len)
{
	TlsConn *t = find(sock);
	int total = 0, n, e, r, eof = 0;

	if (!t)
		return (int) recv(sock, buf, len, 0);
	if (!t->ready) {
		set_would_block();
		return TLS_NO_DATA;
	}
	/* everything available: what stays inside the SSL is not seen by select */
	while (total < len) {
		n = SSL_read(t->ssl, (char *) buf + total, len - total);
		if (n > 0) {
			total += n;
			continue;
		}
		e = SSL_get_error(t->ssl, n);
		if (e == SSL_ERROR_WANT_READ) {
			r = fill_in(t);
			if (r > 0)
				continue;
			if (r == 0 || r == -2)
				eof = 1;
			break;
		}
		if (e == SSL_ERROR_WANT_WRITE) {
			if (!flush_out(t))
				break;
			continue;
		}
		if (e == SSL_ERROR_ZERO_RETURN)		/* close_notify */
			eof = 1;
		else if (!total) {
			ssl_error("receive");
			return -1;
		}
		break;
	}
	flush_out(t);
	if (total)
		return total;
	if (eof)
		return 0;
	set_would_block();
	return TLS_NO_DATA;
}

int tls_send(long sock, const void *buf, int len)
{
	TlsConn *t = find(sock);
	int n;

	if (!t)
		return (int) send(sock, (void *) buf, len, 0);
	if (!t->ready) {
		set_would_block();
		return TLS_NO_DATA;
	}
	n = SSL_write(t->ssl, buf, len);
	if (!flush_out(t) || n <= 0)
		return -1;
	return n;
}
