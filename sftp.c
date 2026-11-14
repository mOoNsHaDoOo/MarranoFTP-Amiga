/*
 * SFTP.C - SFTP version 3 client, see sftp.h.
 *
 * Packet: uint32 length, byte type, uint32 request id, data.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "sftp.h"

enum {
	FXP_INIT = 1, FXP_VERSION = 2, FXP_OPEN = 3, FXP_CLOSE = 4, FXP_READ = 5, FXP_WRITE = 6,
	FXP_OPENDIR = 11, FXP_READDIR = 12, FXP_REMOVE = 13, FXP_MKDIR = 14, FXP_RMDIR = 15,
	FXP_REALPATH = 16, FXP_STAT = 17, FXP_RENAME = 18,
	FXP_STATUS = 101, FXP_HANDLE = 102, FXP_DATA = 103, FXP_NAME = 104, FXP_ATTRS = 105
};

enum { FX_OK = 0, FX_EOF = 1, FX_NO_SUCH_FILE = 2, FX_PERMISSION_DENIED = 3 };

#define FXF_READ	0x01
#define FXF_WRITE	0x02
#define FXF_CREAT	0x08
#define FXF_TRUNC	0x10

#define PIPELINE	4					/* requests in flight */
#define PKT_SIZE	(SFTP_BLOCK + 1024)	/* largest answer we accept */

typedef struct Req {
	unsigned long id, off_lo, off_hi;
	int len;
} Req;

struct Sftp {
	Ssh *ssh;
	char err[200];
	unsigned long next_id;
	unsigned char *in;					/* last answer: type, id, data */
	int in_len;
	unsigned char *out;					/* request being built */
	int out_len, out_cap;

	/* transfer */
	unsigned char handle[256];
	int handle_len, xfer_open, xfer_eof, xfer_error;
	unsigned long off_lo, off_hi;		/* next offset to request/write */
	Req req[PIPELINE];
	int nreq, drop;						/* drop: answers to throw away */
	int broken;							/* the SSH connection failed */
};

/* ---------------------------------------------------------------------------
 * Encoding
 * ------------------------------------------------------------------------- */
static void put_u32(unsigned char *p, unsigned long v)
{
	p[0] = (unsigned char) (v >> 24);
	p[1] = (unsigned char) (v >> 16);
	p[2] = (unsigned char) (v >> 8);
	p[3] = (unsigned char) v;
}

static unsigned long get_u32(const unsigned char *p)
{
	return ((unsigned long) p[0] << 24) | ((unsigned long) p[1] << 16) | ((unsigned long) p[2] << 8) | p[3];
}

static int fail(Sftp *f, const char *msg)
{
	strncpy(f->err, msg, sizeof(f->err) - 1);
	f->err[sizeof(f->err) - 1] = 0;
	return 0;
}

static int ssh_fail(Sftp *f)
{
	f->broken = 1;
	return fail(f, ssh_error(f->ssh));
}

/* request: length placeholder, type, id */
static unsigned long begin(Sftp *f, int type)
{
	f->out_len = 4;
	f->out[f->out_len++] = (unsigned char) type;
	put_u32(f->out + f->out_len, f->next_id);
	f->out_len += 4;
	return f->next_id++;
}

static void add_u32(Sftp *f, unsigned long v)
{
	if (f->out_len + 4 <= f->out_cap) {
		put_u32(f->out + f->out_len, v);
		f->out_len += 4;
	}
}

static void add_str(Sftp *f, const void *data, int n)
{
	if (f->out_len + 4 + n <= f->out_cap) {
		put_u32(f->out + f->out_len, (unsigned long) n);
		memcpy(f->out + f->out_len + 4, data, n);
		f->out_len += 4 + n;
	} else
		f->out_len = f->out_cap + 1;		/* too long: send() fails */
}

static void add_cstr(Sftp *f, const char *s)
{
	add_str(f, s, strlen(s));
}

static int send_req(Sftp *f)
{
	if (f->out_len > f->out_cap)
		return fail(f, "request too large");
	put_u32(f->out, (unsigned long) f->out_len - 4);
	return ssh_chan_write(f->ssh, f->out, f->out_len) ? 1 : ssh_fail(f);
}

static int read_full(Sftp *f, unsigned char *buf, int n)
{
	int r;

	while (n > 0) {
		r = ssh_chan_read(f->ssh, buf, n);
		if (r <= 0)
			return ssh_fail(f);
		buf += r;
		n -= r;
	}
	return 1;
}

/* next answer in f->in; returns its type, 0 on error */
static int recv_answer(Sftp *f)
{
	unsigned char len[4];
	unsigned long n;

	if (!read_full(f, len, 4))
		return 0;
	n = get_u32(len);
	if (n < 5 || n > PKT_SIZE) {
		f->broken = 1;
		return fail(f, "bad sftp packet");
	}
	if (!read_full(f, f->in, (int) n))
		return 0;
	f->in_len = (int) n;
	return f->in[0];
}

/* Reader of the answer data (after type and id) */
typedef struct R {
	const unsigned char *p;
	int len, pos, err;
} R;

static void r_init(Sftp *f, R *r, int skip)
{
	r->p = f->in;
	r->len = f->in_len;
	r->pos = skip;
	r->err = 0;
}

static unsigned long r_u32(R *r)
{
	unsigned long v;

	if (r->pos + 4 > r->len) {
		r->err = 1;
		return 0;
	}
	v = get_u32(r->p + r->pos);
	r->pos += 4;
	return v;
}

static const unsigned char *r_str(R *r, int *n)
{
	unsigned long l = r_u32(r);
	const unsigned char *p;

	if (r->err || l > (unsigned long) (r->len - r->pos)) {
		r->err = 1;
		*n = 0;
		return (const unsigned char *) "";
	}
	p = r->p + r->pos;
	r->pos += (int) l;
	*n = (int) l;
	return p;
}

static void r_cstr(R *r, char *dst, int size)
{
	int n;
	const unsigned char *p = r_str(r, &n);

	if (n > size - 1)
		n = size - 1;
	memcpy(dst, p, n);
	dst[n] = 0;
}

static void r_attrs(R *r, SftpAttr *a)
{
	unsigned long n, i;
	int len;

	memset(a, 0, sizeof(SftpAttr));
	a->flags = r_u32(r);
	if (a->flags & 0x01) {			/* size, uint64 */
		a->size_hi = r_u32(r);
		a->size_lo = r_u32(r);
	}
	if (a->flags & 0x02) {			/* uid, gid */
		r_u32(r);
		r_u32(r);
	}
	if (a->flags & 0x04)
		a->perm = r_u32(r);
	if (a->flags & 0x08) {			/* atime, mtime */
		r_u32(r);
		a->mtime = r_u32(r);
	}
	if (a->flags & 0x80000000UL) {	/* extended */
		n = r_u32(r);
		for (i = 0; i < n && !r->err; i++) {
			r_str(r, &len);
			r_str(r, &len);
		}
	}
}

/* STATUS answer: 1 if FX_OK, else the error message is kept */
static int status_ok(Sftp *f, const char *what)
{
	R r;
	unsigned long code;
	char msg[120], text[200];

	if (f->in[0] != FXP_STATUS)
		return fail(f, "unexpected sftp answer");
	r_init(f, &r, 5);
	code = r_u32(&r);
	if (code == FX_OK)
		return 1;
	r_cstr(&r, msg, sizeof(msg));
	sprintf(text, "%s: %s", what, msg[0] ? msg : code == FX_NO_SUCH_FILE ? "no such file"
			: code == FX_PERMISSION_DENIED ? "permission denied" : "failure");
	return fail(f, text);
}

/* one request with one answer, the answer id is checked */
static int transact(Sftp *f, unsigned long id)
{
	int t;

	if (!send_req(f))
		return 0;
	t = recv_answer(f);
	if (!t)
		return 0;
	if (f->in_len < 5 || get_u32(f->in + 1) != id)
		return fail(f, "sftp answer out of order");
	return t;
}

/* ---------------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------------- */
Sftp *sftp_new(Ssh *ssh)
{
	Sftp *f = (Sftp *) malloc(sizeof(Sftp));
	unsigned char init[9];
	unsigned char len[4];
	unsigned long n;

	if (!f)
		return NULL;
	memset(f, 0, sizeof(Sftp));
	f->ssh = ssh;
	f->out_cap = SFTP_BLOCK + 1024;
	f->in = (unsigned char *) malloc(PKT_SIZE);
	f->out = (unsigned char *) malloc(f->out_cap);
	f->next_id = 1;
	if (!f->in || !f->out) {
		sftp_free(f);
		return NULL;
	}
	/* INIT has no request id: uint32 length, byte 1, uint32 version 3 */
	put_u32(init, 5);
	init[4] = FXP_INIT;
	put_u32(init + 5, 3);
	if (!ssh_chan_write(ssh, init, 9) || !read_full(f, len, 4)) {
		sftp_free(f);
		return NULL;
	}
	n = get_u32(len);
#ifdef SSH_TRACE
	fprintf(stderr, "[sftp] VERSION length %lu\n", n);
#endif
	if (n < 5 || n > PKT_SIZE || !read_full(f, f->in, (int) n) || f->in[0] != FXP_VERSION) {
		sftp_free(f);
		return NULL;
	}
	return f;
}

void sftp_free(Sftp *f)
{
	if (!f)
		return;
	free(f->in);
	free(f->out);
	free(f);
}

const char *sftp_error(Sftp *f)
{
	return f->err[0] ? f->err : "unknown error";
}

int sftp_broken(Sftp *f)
{
	return f->broken;
}

int sftp_realpath(Sftp *f, const char *path, char *out, int outlen)
{
	unsigned long id = begin(f, FXP_REALPATH);
	R r;
	int t;

	add_cstr(f, path);
	t = transact(f, id);
	if (t == FXP_STATUS)
		return status_ok(f, path) && fail(f, "no path in the answer");
	if (t != FXP_NAME)
		return t ? fail(f, "unexpected answer to REALPATH") : 0;
	r_init(f, &r, 5);
	if (r_u32(&r) < 1)
		return fail(f, "empty REALPATH answer");
	r_cstr(&r, out, outlen);
	return !r.err;
}

int sftp_stat(Sftp *f, const char *path, SftpAttr *a)
{
	unsigned long id = begin(f, FXP_STAT);
	R r;
	int t;

	add_cstr(f, path);
	t = transact(f, id);
	if (t == FXP_STATUS)
		return status_ok(f, path) && fail(f, "no attributes");
	if (t != FXP_ATTRS)
		return t ? fail(f, "unexpected answer to STAT") : 0;
	r_init(f, &r, 5);
	r_attrs(&r, a);
	return !r.err;
}

static int close_handle(Sftp *f, const unsigned char *h, int hlen)
{
	unsigned long id = begin(f, FXP_CLOSE);
	int t;

	add_str(f, h, hlen);
	t = transact(f, id);
	return t == FXP_STATUS ? status_ok(f, "close") : (t ? fail(f, "unexpected answer to CLOSE") : 0);
}

/* OPEN / OPENDIR answer: the handle is copied in out (256 bytes) */
static int get_handle(Sftp *f, const char *what, unsigned char *out, int *out_len)
{
	R r;
	const unsigned char *h;

	if (f->in[0] == FXP_STATUS)
		return status_ok(f, what) && 0;
	if (f->in[0] != FXP_HANDLE)
		return fail(f, "unexpected answer to OPEN");
	r_init(f, &r, 5);
	h = r_str(&r, out_len);
	if (r.err || *out_len > 256)
		return fail(f, "bad handle");
	memcpy(out, h, *out_len);
	return 1;
}

int sftp_list(Sftp *f, const char *path, SftpDirCb cb, void *ctx)
{
	unsigned long id, count, i;
	unsigned char dh[256];
	int dh_len, t, ok = 1, go = 1;
	char name[512], longname[512];
	SftpAttr a;
	R r;

	/* own handle: a transfer may be open (f->handle) */
	id = begin(f, FXP_OPENDIR);
	add_cstr(f, path);
	if (!transact(f, id) || !get_handle(f, path, dh, &dh_len))
		return 0;
	while (go) {
		id = begin(f, FXP_READDIR);
		add_str(f, dh, dh_len);
		t = transact(f, id);
		if (t == FXP_STATUS) {
			r_init(f, &r, 5);
			if (r_u32(&r) != FX_EOF)
				ok = status_ok(f, path);
			break;
		}
		if (t != FXP_NAME) {
			ok = t ? fail(f, "unexpected answer to READDIR") : 0;
			break;
		}
		r_init(f, &r, 5);
		count = r_u32(&r);
		for (i = 0; i < count && !r.err && go; i++) {
			r_cstr(&r, name, sizeof(name));
			r_cstr(&r, longname, sizeof(longname));
			r_attrs(&r, &a);
			if (!r.err && cb && !cb(ctx, name, longname, &a))
				go = 0;
		}
		if (r.err) {
			ok = fail(f, "bad READDIR answer");
			break;
		}
	}
	if (!close_handle(f, dh, dh_len) && ok)
		ok = 0;
	return ok;
}

static int simple(Sftp *f, int type, const char *path, const char *path2, const char *what)
{
	unsigned long id = begin(f, type);
	int t;

	add_cstr(f, path);
	if (path2)
		add_cstr(f, path2);
	if (type == FXP_MKDIR)
		add_u32(f, 0);					/* no attributes */
	t = transact(f, id);
	return t ? status_ok(f, what) : 0;
}

int sftp_remove(Sftp *f, const char *path) { return simple(f, FXP_REMOVE, path, NULL, path); }
int sftp_rmdir(Sftp *f, const char *path) { return simple(f, FXP_RMDIR, path, NULL, path); }
int sftp_mkdir(Sftp *f, const char *path) { return simple(f, FXP_MKDIR, path, NULL, path); }
int sftp_rename(Sftp *f, const char *from, const char *to) { return simple(f, FXP_RENAME, from, to, from); }

/* ---------------------------------------------------------------------------
 * Transfers
 * ------------------------------------------------------------------------- */
static void add_offset(unsigned long *hi, unsigned long *lo, unsigned long n)
{
	unsigned long old = *lo;

	*lo += n;
	if (*lo < old)
		(*hi)++;
}

static int open_file(Sftp *f, const char *path, unsigned long pflags)
{
	unsigned long id = begin(f, FXP_OPEN);

	add_cstr(f, path);
	add_u32(f, pflags);
	add_u32(f, 0);						/* no attributes */
	f->xfer_open = f->xfer_eof = f->xfer_error = 0;
	f->off_lo = f->off_hi = 0;
	f->nreq = f->drop = 0;
	if (!transact(f, id) || !get_handle(f, path, f->handle, &f->handle_len))
		return 0;
	f->xfer_open = 1;
	return 1;
}

static int send_read(Sftp *f)
{
	Req *q = &f->req[f->nreq];

	q->id = begin(f, FXP_READ);
	q->off_hi = f->off_hi;
	q->off_lo = f->off_lo;
	q->len = SFTP_BLOCK;
	add_str(f, f->handle, f->handle_len);
	add_u32(f, q->off_hi);
	add_u32(f, q->off_lo);
	add_u32(f, (unsigned long) q->len);
	if (!send_req(f))
		return 0;
	add_offset(&f->off_hi, &f->off_lo, (unsigned long) q->len);
	f->nreq++;
	return 1;
}

/* answer of the oldest request in flight */
static int next_answer(Sftp *f, Req *q)
{
	int t = recv_answer(f);

	if (!t)
		return 0;
	if (f->in_len < 5 || get_u32(f->in + 1) != f->req[0].id)
		return fail(f, "sftp answer out of order");
	*q = f->req[0];
	f->nreq--;
	memmove(&f->req[0], &f->req[1], f->nreq * sizeof(Req));
	return t;
}

int sftp_get_begin(Sftp *f, const char *path)
{
	return open_file(f, path, FXF_READ);
}

int sftp_get_step(Sftp *f, const unsigned char **data, int *len)
{
	Req q;
	R r;
	int t, n;
	const unsigned char *d;

	for (;;) {
		while (!f->xfer_eof && f->nreq < PIPELINE)
			if (!send_read(f))
				return -1;
		if (f->nreq == 0)
			break;						/* end of file and nothing in flight */
		t = next_answer(f, &q);
		if (!t)
			return -1;
		if (f->drop > 0) {				/* after a short read */
			f->drop--;
			continue;
		}
		if (t == FXP_STATUS) {
			r_init(f, &r, 5);
			if (r_u32(&r) == FX_EOF) {
				f->xfer_eof = 1;
				f->drop = f->nreq;		/* the later ones are past the end */
				continue;
			}
			status_ok(f, "read");
			return -1;
		}
		if (t != FXP_DATA) {
			fail(f, "unexpected answer to READ");
			return -1;
		}
		r_init(f, &r, 5);
		d = r_str(&r, &n);
		if (r.err || n > q.len) {
			fail(f, "bad READ answer");
			return -1;
		}
		if (n < q.len) {
			/* short read: ask again from the end of this block */
			f->off_hi = q.off_hi;
			f->off_lo = q.off_lo;
			add_offset(&f->off_hi, &f->off_lo, (unsigned long) n);
			f->drop = f->nreq;
		}
		*data = d;
		*len = n;
		if (n > 0)
			return 1;
	}
	close_handle(f, f->handle, f->handle_len);
	f->xfer_open = 0;
	return 0;
}

int sftp_put_begin(Sftp *f, const char *path)
{
	return open_file(f, path, FXF_WRITE | FXF_CREAT | FXF_TRUNC);
}

static int wait_write(Sftp *f)
{
	Req q;
	int t = next_answer(f, &q);

	return t ? status_ok(f, "write") : 0;
}

int sftp_put_step(Sftp *f, const unsigned char *data, int len)
{
	Req *q;

	while (len > 0) {
		int n = len > SFTP_BLOCK ? SFTP_BLOCK : len;

		if (f->nreq == PIPELINE && !wait_write(f))
			return 0;
		q = &f->req[f->nreq];
		q->id = begin(f, FXP_WRITE);
		q->off_hi = f->off_hi;
		q->off_lo = f->off_lo;
		q->len = n;
		add_str(f, f->handle, f->handle_len);
		add_u32(f, q->off_hi);
		add_u32(f, q->off_lo);
		add_str(f, data, n);
		if (!send_req(f))
			return 0;
		f->nreq++;
		add_offset(&f->off_hi, &f->off_lo, (unsigned long) n);
		data += n;
		len -= n;
	}
	return 1;
}

int sftp_put_end(Sftp *f)
{
	int ok = 1;

	while (f->nreq > 0 && ok)
		ok = wait_write(f);
	if (!close_handle(f, f->handle, f->handle_len))
		ok = 0;
	f->xfer_open = 0;
	return ok;
}

void sftp_xfer_abort(Sftp *f)
{
	Req q;

	if (!f->xfer_open)
		return;
	while (f->nreq > 0)
		if (!next_answer(f, &q))
			return;
	close_handle(f, f->handle, f->handle_len);
	f->xfer_open = 0;
}
