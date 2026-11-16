/*
 * SSH.C - minimal SSH-2 client, see ssh.h.
 *
 * Packet layout (RFC 4253 6):
 *   uint32 packet_length, byte padding_length, payload, random padding,
 *   MAC = HMAC(key, uint32 sequence_number || unencrypted packet)
 * With aes-ctr the whole packet is encrypted, the MAC is appended in clear.
 *
 * "Strict KEX" (kex-strict-c-v00@openssh.com, the Terrapin countermeasure)
 * is supported: when both sides announce it the sequence numbers restart
 * from zero after every NEWKEYS.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "ssh.h"
#include "ssh_crypto.h"

/* ---------------------------------------------------------------------------
 * Sizes
 * ------------------------------------------------------------------------- */
#define PKT_MAX			35000		/* largest packet accepted (RFC: >= 35000) */
#define RX_SIZE			16384		/* socket read buffer */
#define CHAN_WINDOW		65536		/* our receive window = channel buffer */
#define CHAN_MAXPKT		32768		/* largest data packet we accept */
#define OUT_MAXDATA		32000		/* largest data packet we send */
#define KEXBUF_SIZE		16384

/* Message numbers */
enum {
	MSG_DISCONNECT = 1, MSG_IGNORE = 2, MSG_UNIMPLEMENTED = 3, MSG_DEBUG = 4,
	MSG_SERVICE_REQUEST = 5, MSG_SERVICE_ACCEPT = 6, MSG_EXT_INFO = 7,
	MSG_KEXINIT = 20, MSG_NEWKEYS = 21, MSG_KEX_INIT = 30, MSG_KEX_REPLY = 31,
	MSG_USERAUTH_REQUEST = 50, MSG_USERAUTH_FAILURE = 51, MSG_USERAUTH_SUCCESS = 52,
	MSG_USERAUTH_BANNER = 53, MSG_USERAUTH_INFO_REQUEST = 60, MSG_USERAUTH_INFO_RESPONSE = 61,
	MSG_GLOBAL_REQUEST = 80, MSG_REQUEST_FAILURE = 82,
	MSG_CHANNEL_OPEN = 90, MSG_CHANNEL_OPEN_CONFIRMATION = 91, MSG_CHANNEL_OPEN_FAILURE = 92,
	MSG_CHANNEL_WINDOW_ADJUST = 93, MSG_CHANNEL_DATA = 94, MSG_CHANNEL_EXTENDED_DATA = 95,
	MSG_CHANNEL_EOF = 96, MSG_CHANNEL_CLOSE = 97, MSG_CHANNEL_REQUEST = 98,
	MSG_CHANNEL_SUCCESS = 99, MSG_CHANNEL_FAILURE = 100
};

/* Algorithms, in order of preference */
enum { KEX_C25519, KEX_C25519_LIBSSH, KEX_DH14_SHA256, KEX_DH14_SHA1 };
static const char *kex_names[] = { "curve25519-sha256", "curve25519-sha256@libssh.org",
								   "diffie-hellman-group14-sha256", "diffie-hellman-group14-sha1", NULL };
enum { HK_ED25519, HK_RSA_SHA256, HK_RSA_SHA512, HK_RSA_SHA1 };
static const char *hostkey_names[] = { "ssh-ed25519", "rsa-sha2-256", "rsa-sha2-512", "ssh-rsa", NULL };
enum { CIPHER_AES128, CIPHER_AES256 };
static const char *cipher_names[] = { "aes128-ctr", "aes256-ctr", NULL };
enum { MAC_SHA256, MAC_SHA1 };
static const char *mac_names[] = { "hmac-sha2-256", "hmac-sha1", NULL };

#define KEX_LIST "curve25519-sha256,curve25519-sha256@libssh.org,diffie-hellman-group14-sha256,diffie-hellman-group14-sha1,kex-strict-c-v00@openssh.com"
#define HOSTKEY_LIST "ssh-ed25519,rsa-sha2-256,rsa-sha2-512,ssh-rsa"
#define CIPHER_LIST "aes128-ctr,aes256-ctr"
#define MAC_LIST "hmac-sha2-256,hmac-sha1"

/* RFC 3526 group 14, 2048 bit MODP */
static const char dh14_p[] =
	"FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD"
	"EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
	"EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
	"83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
	"E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
	"15728E5A8AACAA68FFFFFFFFFFFFFFFF";

/* Keys of one direction */
typedef struct Keys {
	EVP_CIPHER_CTX *cipher;
	const EVP_MD *mac_md;
	unsigned char mac_key[32];
	int mac_len;
} Keys;

struct Ssh {
	SshIo io;
	SshLog log;
	SshHostKeyCheck check;
	void *ctx;
	char err[200];

	unsigned char *rx;					/* raw socket data */
	int rx_len, rx_pos;
	unsigned char *pktbuf;				/* 4 bytes sequence + incoming packet */
	unsigned char *outbuf;				/* 4 bytes sequence + outgoing packet + MAC */
	unsigned char *tmp;					/* payload being built */
	unsigned char *payload;				/* payload of the last packet received */
	int payload_len;
	unsigned long seq_in, seq_out;
	int keyed_in, keyed_out;
	Keys kin, kout, pend_in, pend_out;

	/* key exchange */
	char v_c[64], v_s[256];
	unsigned char *i_c, *i_s;
	int i_c_len, i_s_len;
	unsigned char session_id[32];
	int session_id_len;
	int kex, hostkey, cipher_c2s, cipher_s2c, mac_c2s, mac_s2c, strict, in_kex, kex_count;
	char fingerprint[64];

	/* channel */
	unsigned long remote_chan, remote_window, remote_maxpkt, to_adjust;
	int chan_open, chan_closed, chan_wait;	/* chan_wait: return on data/window */
	unsigned char *cbuf;
	int cbuf_len, cbuf_pos;
};

/* ---------------------------------------------------------------------------
 * Small helpers
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

static int fail(Ssh *s, const char *msg)
{
	if (!s->err[0]) {
		strncpy(s->err, msg, sizeof(s->err) - 1);
		s->err[sizeof(s->err) - 1] = 0;
	}
	return 0;
}

static void say(Ssh *s, const char *msg)
{
	if (s->log)
		s->log(s->ctx, msg);
}

/* Builder of SSH encoded data */
typedef struct W {
	unsigned char *p;
	int len, cap, err;
} W;

static void w_init(W *w, unsigned char *buf, int cap)
{
	w->p = buf;
	w->len = 0;
	w->cap = cap;
	w->err = 0;
}

static void w_raw(W *w, const void *data, int n)
{
	if (w->len + n > w->cap) {
		w->err = 1;
		return;
	}
	memcpy(w->p + w->len, data, n);
	w->len += n;
}

static void w_byte(W *w, int b)
{
	unsigned char c = (unsigned char) b;

	w_raw(w, &c, 1);
}

static void w_u32(W *w, unsigned long v)
{
	unsigned char b[4];

	put_u32(b, v);
	w_raw(w, b, 4);
}

static void w_str(W *w, const void *data, int n)
{
	w_u32(w, (unsigned long) n);
	w_raw(w, data, n);
}

static void w_cstr(W *w, const char *s)
{
	w_str(w, s, strlen(s));
}

/* mpint from an unsigned big endian number */
static void w_mpint(W *w, const unsigned char *num, int n)
{
	while (n > 0 && num[0] == 0) {
		num++;
		n--;
	}
	if (n > 0 && (num[0] & 0x80)) {
		w_u32(w, (unsigned long) n + 1);
		w_byte(w, 0);
		w_raw(w, num, n);
	} else
		w_str(w, num, n);
}

/* Reader */
typedef struct R {
	const unsigned char *p;
	int len, pos, err;
} R;

static void r_init(R *r, const unsigned char *p, int len)
{
	r->p = p;
	r->len = len;
	r->pos = 0;
	r->err = 0;
}

static int r_byte(R *r)
{
	if (r->pos + 1 > r->len) {
		r->err = 1;
		return 0;
	}
	return r->p[r->pos++];
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

/* string as a C string (truncated) */
static void r_cstr(R *r, char *dst, int size)
{
	int n;
	const unsigned char *p = r_str(r, &n);

	if (n > size - 1)
		n = size - 1;
	memcpy(dst, p, n);
	dst[n] = 0;
}

static int str_eq(const unsigned char *p, int n, const char *s)
{
	return n == (int) strlen(s) && !memcmp(p, s, n);
}

/* name in a comma separated name-list */
static int in_list(const char *name, const unsigned char *list, int n)
{
	int len = strlen(name), i = 0, j;

	while (i < n) {
		for (j = i; j < n && list[j] != ','; j++)
			;
		if (j - i == len && !memcmp(list + i, name, len))
			return 1;
		i = j + 1;
	}
	return 0;
}

/* first of our algorithms that the server supports, -1 if none */
static int choose(const char **ours, const unsigned char *theirs, int n)
{
	int i;

	for (i = 0; ours[i]; i++)
		if (in_list(ours[i], theirs, n))
			return i;
	return -1;
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
	if (n - i == 1) {
		v = (unsigned long) src[i] << 16;
		dst[o++] = tab[(v >> 18) & 63];
		dst[o++] = tab[(v >> 12) & 63];
	} else if (n - i == 2) {
		v = ((unsigned long) src[i] << 16) | ((unsigned long) src[i + 1] << 8);
		dst[o++] = tab[(v >> 18) & 63];
		dst[o++] = tab[(v >> 12) & 63];
		dst[o++] = tab[(v >> 6) & 63];
	}
	dst[o] = 0;						/* OpenSSH style: no '=' padding */
}

/* ---------------------------------------------------------------------------
 * Raw I/O
 * ------------------------------------------------------------------------- */
static int raw_get(Ssh *s, unsigned char *dst, int n)
{
	int r;

	while (n > 0) {
		if (s->rx_pos == s->rx_len) {
			r = s->io.recv(s->io.ctx, s->rx, RX_SIZE);
			if (r <= 0)
				return fail(s, r == 0 ? "connection closed by the server" : "receive error or timeout");
			s->rx_len = r;
			s->rx_pos = 0;
		}
		r = s->rx_len - s->rx_pos;
		if (r > n)
			r = n;
		memcpy(dst, s->rx + s->rx_pos, r);
		s->rx_pos += r;
		dst += r;
		n -= r;
	}
	return 1;
}

static int raw_put(Ssh *s, const unsigned char *src, int n)
{
	int r;

	while (n > 0) {
		r = s->io.send(s->io.ctx, src, n);
		if (r <= 0)
			return fail(s, "send error");
		src += r;
		n -= r;
	}
	return 1;
}

/* ---------------------------------------------------------------------------
 * Packets
 * ------------------------------------------------------------------------- */
static int send_packet(Ssh *s, const unsigned char *payload, int len)
{
	unsigned char *seq = s->outbuf, *p = s->outbuf + 4;
	int block = s->keyed_out ? 16 : 8, pad, total, outl;
	unsigned int ml;

	if (len > PKT_MAX - 64)
		return fail(s, "packet too large");
#ifdef SSH_TRACE
	fprintf(stderr, "[ssh] -> msg %d, %d bytes\n", payload[0], len);
#endif
	pad = block - ((len + 5) % block);
	if (pad < 4)
		pad += block;
	total = 5 + len + pad;
	put_u32(p, (unsigned long) (total - 4));
	p[4] = (unsigned char) pad;
	memmove(p + 5, payload, len);
	RAND_bytes(p + 5 + len, pad);
	if (s->keyed_out) {
		put_u32(seq, s->seq_out);
		ml = 0;
		HMAC(s->kout.mac_md, s->kout.mac_key, s->kout.mac_len, seq, total + 4, p + total, &ml);
		outl = 0;
		EVP_EncryptUpdate(s->kout.cipher, p, &outl, p, total);
		total += s->kout.mac_len;
	}
	s->seq_out = (s->seq_out + 1) & 0xffffffffUL;
	return raw_put(s, p, total);
}

static int recv_packet(Ssh *s)
{
	unsigned char *p = s->pktbuf + 4, mac[64], calc[64];
	int block = s->keyed_in ? 16 : 8, outl, pad;
	unsigned long len;
	unsigned int ml;

	if (!raw_get(s, p, block))
		return 0;
	if (s->keyed_in)
		EVP_EncryptUpdate(s->kin.cipher, p, &outl, p, block);
	len = get_u32(p);
	if (len < 5 || len + 4 > PKT_MAX || (len + 4) % block)
		return fail(s, "bad packet length (wrong keys or corrupted data)");
	if (!raw_get(s, p + block, (int) len + 4 - block))
		return 0;
	if (s->keyed_in) {
		EVP_EncryptUpdate(s->kin.cipher, p + block, &outl, p + block, (int) len + 4 - block);
		if (!raw_get(s, mac, s->kin.mac_len))
			return 0;
		put_u32(s->pktbuf, s->seq_in);
		ml = 0;
		HMAC(s->kin.mac_md, s->kin.mac_key, s->kin.mac_len, s->pktbuf, (int) len + 8, calc, &ml);
		if (memcmp(mac, calc, s->kin.mac_len))
			return fail(s, "MAC error: corrupted data");
	}
	s->seq_in = (s->seq_in + 1) & 0xffffffffUL;
	pad = p[4];
	if (pad + 1 > (int) len - 1)
		return fail(s, "bad padding");
	s->payload = p + 5;
	s->payload_len = (int) len - pad - 1;
	return 1;
}

static int do_kex(Ssh *s, const unsigned char *server_kexinit, int server_len);

static int chan_data_in(Ssh *s, const unsigned char *data, int n)
{
	if (s->cbuf_pos > 0 && s->cbuf_pos == s->cbuf_len)
		s->cbuf_pos = s->cbuf_len = 0;
	if (s->cbuf_len + n > CHAN_WINDOW) {
		memmove(s->cbuf, s->cbuf + s->cbuf_pos, s->cbuf_len - s->cbuf_pos);
		s->cbuf_len -= s->cbuf_pos;
		s->cbuf_pos = 0;
	}
	if (s->cbuf_len + n > CHAN_WINDOW)
		return fail(s, "the server ignored the channel window");
	memcpy(s->cbuf + s->cbuf_len, data, n);
	s->cbuf_len += n;
	return 1;
}

static int send_window_adjust(Ssh *s, unsigned long n)
{
	W w;

	w_init(&w, s->tmp, 16);
	w_byte(&w, MSG_CHANNEL_WINDOW_ADJUST);
	w_u32(&w, s->remote_chan);
	w_u32(&w, n);
	return send_packet(s, w.p, w.len);
}

/*
 * Next message for the caller. Transport and channel housekeeping is done
 * here: ignore/debug, global requests, rekeying, channel data and window.
 * Returns the message number, 0 on error.
 */
static int recv_msg(Ssh *s)
{
	R r;
	int t, n, want;
	char text[160];
	const unsigned char *d;
	W w;

	for (;;) {
		if (!recv_packet(s))
			return 0;
		if (s->payload_len < 1)
			continue;
		r_init(&r, s->payload, s->payload_len);
		t = r_byte(&r);
#ifdef SSH_TRACE
		fprintf(stderr, "[ssh] <- msg %d, %d bytes\n", t, s->payload_len);
#endif
		switch (t) {
			case MSG_DISCONNECT:
				r_u32(&r);
				strcpy(text, "disconnected by the server: ");
				r_cstr(&r, text + strlen(text), sizeof(text) - strlen(text));
				return fail(s, text);
			case MSG_IGNORE:
			case MSG_DEBUG:
			case MSG_UNIMPLEMENTED:
			case MSG_EXT_INFO:
				if (s->strict && s->in_kex && !s->kex_count)
					return fail(s, "unexpected message during the strict key exchange");
				continue;
			case MSG_GLOBAL_REQUEST:
				r_str(&r, &n);
				want = r_byte(&r);
				if (want) {
					w_init(&w, s->tmp, 8);
					w_byte(&w, MSG_REQUEST_FAILURE);
					if (!send_packet(s, w.p, w.len))
						return 0;
				}
				continue;
			case MSG_KEXINIT:
				if (s->in_kex)
					return t;
				say(s, "SSH: the server asked for new keys");
				if (!do_kex(s, s->payload, s->payload_len))
					return 0;
				continue;
		}
		if (s->chan_open) {
			switch (t) {
				case MSG_CHANNEL_WINDOW_ADJUST:
					r_u32(&r);
					s->remote_window += r_u32(&r);
					if (s->chan_wait)
						return t;
					continue;
				case MSG_CHANNEL_DATA:
					r_u32(&r);
					d = r_str(&r, &n);
					if (r.err || !chan_data_in(s, d, n))
						return fail(s, "bad channel data");
					if (s->chan_wait)
						return t;
					continue;
				case MSG_CHANNEL_EXTENDED_DATA:			/* stderr: dropped */
					r_u32(&r);
					r_u32(&r);
					r_str(&r, &n);
					s->to_adjust += n;
					continue;
				case MSG_CHANNEL_REQUEST:
					r_u32(&r);
					r_str(&r, &n);
					want = r_byte(&r);
					if (want) {
						w_init(&w, s->tmp, 8);
						w_byte(&w, MSG_CHANNEL_FAILURE);
						w_u32(&w, s->remote_chan);
						if (!send_packet(s, w.p, w.len))
							return 0;
					}
					continue;
				case MSG_CHANNEL_EOF:
				case MSG_CHANNEL_CLOSE:
					s->chan_closed = 1;
					return fail(s, "the server closed the sftp channel");
			}
		}
		return t;
	}
}

/* ---------------------------------------------------------------------------
 * Key exchange
 * ------------------------------------------------------------------------- */
static const EVP_MD *kex_md(Ssh *s)
{
	return s->kex == KEX_DH14_SHA1 ? EVP_sha1() : EVP_sha256();
}

static int send_kexinit(Ssh *s)
{
	W w;
	unsigned char cookie[16];

	w_init(&w, s->tmp, 2048);
	w_byte(&w, MSG_KEXINIT);
	RAND_bytes(cookie, 16);
	w_raw(&w, cookie, 16);
	w_cstr(&w, s->kex_count ? KEX_LIST : KEX_LIST);
	w_cstr(&w, HOSTKEY_LIST);
	w_cstr(&w, CIPHER_LIST);
	w_cstr(&w, CIPHER_LIST);
	w_cstr(&w, MAC_LIST);
	w_cstr(&w, MAC_LIST);
	w_cstr(&w, "none");
	w_cstr(&w, "none");
	w_cstr(&w, "");
	w_cstr(&w, "");
	w_byte(&w, 0);
	w_u32(&w, 0);
	free(s->i_c);
	s->i_c = (unsigned char *) malloc(w.len);
	if (!s->i_c)
		return fail(s, "out of memory");
	memcpy(s->i_c, w.p, w.len);
	s->i_c_len = w.len;
	return send_packet(s, w.p, w.len);
}

static int parse_kexinit(Ssh *s)
{
	R r;
	const unsigned char *l[10];
	int n[10], i;
	char text[200];

	r_init(&r, s->i_s, s->i_s_len);
	r_byte(&r);
	r.pos += 16;
	for (i = 0; i < 10; i++)
		l[i] = r_str(&r, &n[i]);
	if (r.err)
		return fail(s, "bad KEXINIT from the server");
	s->kex = choose(kex_names, l[0], n[0]);
	s->hostkey = choose(hostkey_names, l[1], n[1]);
	s->cipher_c2s = choose(cipher_names, l[2], n[2]);
	s->cipher_s2c = choose(cipher_names, l[3], n[3]);
	s->mac_c2s = choose(mac_names, l[4], n[4]);
	s->mac_s2c = choose(mac_names, l[5], n[5]);
	if (!s->kex_count)
		s->strict = in_list("kex-strict-s-v00@openssh.com", l[0], n[0]);
	if (s->kex < 0)
		return fail(s, "no common key exchange method (need curve25519 or group14)");
	if (s->hostkey < 0)
		return fail(s, "no common host key type (need ed25519 or rsa)");
	if (s->cipher_c2s < 0 || s->cipher_s2c < 0)
		return fail(s, "no common cipher (need aes128-ctr or aes256-ctr)");
	if (s->mac_c2s < 0 || s->mac_s2c < 0)
		return fail(s, "no common MAC (need hmac-sha2-256 or hmac-sha1)");
	if (!in_list("none", l[6], n[6]) || !in_list("none", l[7], n[7]))
		return fail(s, "the server requires compression");
	if (!s->kex_count) {
		sprintf(text, "SSH: %s, %s, %s, %s%s", kex_names[s->kex], hostkey_names[s->hostkey],
				cipher_names[s->cipher_s2c], mac_names[s->mac_s2c], s->strict ? ", strict kex" : "");
		say(s, text);
	}
	return 1;
}

/* DER length of n bytes after a tag */
static int der_len(unsigned char *o, int n)
{
	if (n < 128) {
		o[0] = (unsigned char) n;
		return 1;
	}
	if (n < 256) {
		o[0] = 0x81;
		o[1] = (unsigned char) n;
		return 2;
	}
	o[0] = 0x82;
	o[1] = (unsigned char) (n >> 8);
	o[2] = (unsigned char) n;
	return 3;
}

/* DER INTEGER from big endian unsigned bytes (no leading zeros) */
static int der_int(unsigned char *o, const unsigned char *v, int n)
{
	int k, pad;

	while (n > 1 && v[0] == 0) {
		v++;
		n--;
	}
	pad = n > 0 && (v[0] & 0x80) ? 1 : 0;
	o[0] = 0x02;
	k = 1 + der_len(o + 1, n + pad);
	if (pad)
		o[k++] = 0;
	memcpy(o + k, v, n);
	return k + n;
}

/* SubjectPublicKeyInfo of the RSA key (n, e): out needs n_n + e_n + 40 bytes */
static int rsa_spki(unsigned char *out, const unsigned char *e, int e_n, const unsigned char *n, int n_n)
{
	static const unsigned char alg[] = {	/* rsaEncryption, NULL */
		0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00
	};
	unsigned char ints[1024 + 40];
	int il, k, l;

	il = der_int(ints, n, n_n);
	il += der_int(ints + il, e, e_n);
	/* body: alg + BIT STRING(00 + SEQUENCE(ints)) */
	k = 0;
	out[k++] = 0x30;
	{
		unsigned char t[4];
		int sl = 1 + der_len(t, il) + il;			/* SEQUENCE of the ints */
		int bl = 1 + sl;							/* BIT STRING content */
		int body = sizeof(alg) + 1 + der_len(t, bl) + bl;

		k += der_len(out + k, body);
		memcpy(out + k, alg, sizeof(alg));
		k += sizeof(alg);
		out[k++] = 0x03;
		k += der_len(out + k, bl);
		out[k++] = 0;
		out[k++] = 0x30;
		k += der_len(out + k, il);
	}
	l = k;
	memcpy(out + l, ints, il);
	return l + il;
}

/* Verifies the server signature of the exchange hash with its host key */
static int verify_hostkey(Ssh *s, const unsigned char *ks, int ks_len, const unsigned char *sig, int sig_len,
						  const unsigned char *h, int h_len)
{
	R rk, rs;
	const unsigned char *type, *stype, *sg, *a, *b;
	int type_n, stype_n, sg_n, a_n, b_n, ok = 0, mlen;
	unsigned char fp[32], padded[1024], der[1024 + 64];
	unsigned int dl;
	char text[160];

	/* fingerprint, as shown by OpenSSH */
	dl = 0;
	EVP_Digest(ks, ks_len, fp, &dl, EVP_sha256(), NULL);
	strcpy(s->fingerprint, "SHA256:");
	base64(s->fingerprint + 7, fp, 32);

	r_init(&rk, ks, ks_len);
	type = r_str(&rk, &type_n);
	r_init(&rs, sig, sig_len);
	stype = r_str(&rs, &stype_n);
	sg = r_str(&rs, &sg_n);
	if (rk.err || rs.err)
		return fail(s, "bad host key or signature");

	if (s->hostkey == HK_ED25519) {
		EVP_PKEY *pk;
		EVP_MD_CTX *md;

		a = r_str(&rk, &a_n);
		if (!str_eq(type, type_n, "ssh-ed25519") || !str_eq(stype, stype_n, "ssh-ed25519") || a_n != 32 || sg_n != 64)
			return fail(s, "bad ed25519 host key or signature");
		pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, a, 32);
		md = EVP_MD_CTX_new();
		if (pk && md && EVP_DigestVerifyInit(md, NULL, NULL, NULL, pk) == 1
			&& EVP_DigestVerify(md, sg, 64, h, h_len) == 1)
			ok = 1;
		if (md)
			EVP_MD_CTX_free(md);
		if (pk)
			EVP_PKEY_free(pk);
	} else {
		EVP_PKEY *pk = NULL;
		EVP_MD_CTX *md;
		const unsigned char *p;
		int der_n;

		a = r_str(&rk, &a_n);		/* e */
		b = r_str(&rk, &b_n);		/* n */
		if (!str_eq(type, type_n, "ssh-rsa") || rk.err || !str_eq(stype, stype_n, hostkey_names[s->hostkey]))
			return fail(s, "bad rsa host key or signature");
		while (b_n > 0 && b[0] == 0) {
			b++;
			b_n--;
		}
		mlen = b_n;
		if (mlen > (int) sizeof(padded) || sg_n > mlen || a_n > 16)
			return fail(s, "rsa key too large");
		memset(padded, 0, mlen - sg_n);			/* signature as long as the modulus */
		memcpy(padded + mlen - sg_n, sg, sg_n);
		/* the public key goes to OpenSSL as DER SubjectPublicKeyInfo: the
		   RSA_* functions are deprecated in OpenSSL 3 and missing from some
		   AmiSSL SDKs */
		der_n = rsa_spki(der, a, a_n, b, b_n);
		p = der;
		pk = d2i_PUBKEY(NULL, &p, der_n);
		md = EVP_MD_CTX_new();
		if (pk && md && EVP_DigestVerifyInit(md, NULL, s->hostkey == HK_RSA_SHA256 ? EVP_sha256()
											 : s->hostkey == HK_RSA_SHA512 ? EVP_sha512() : EVP_sha1(), NULL, pk) == 1
			&& EVP_DigestVerify(md, padded, mlen, h, h_len) == 1)
			ok = 1;
		if (md)
			EVP_MD_CTX_free(md);
		if (pk)
			EVP_PKEY_free(pk);
	}
	if (!ok)
		return fail(s, "the server host key signature is not valid");

	if (s->kex_count) {
		/* rekeying: the key must be the one accepted at login */
		return 1;
	}
	sprintf(text, "SSH: host key %.*s %s", type_n > 20 ? 20 : type_n, (const char *) type, s->fingerprint);
	say(s, text);
	if (s->check) {
		char tname[32];
		int n2 = type_n > 31 ? 31 : type_n;

		memcpy(tname, type, n2);
		tname[n2] = 0;
		if (!s->check(s->ctx, tname, s->fingerprint))
			return fail(s, "host key refused");
	}
	return 1;
}

/* key = HASH(K || H || letter || session_id) [|| HASH(K || H || key...)] */
static void derive(Ssh *s, const unsigned char *k, int k_len, const unsigned char *h, int h_len,
				   char letter, unsigned char *out, int need)
{
	unsigned char buf[600], hash[64];
	unsigned int dl;
	int have = 0, n;
	W w;

	w_init(&w, buf, sizeof(buf));
	w_raw(&w, k, k_len);
	w_raw(&w, h, h_len);
	w_byte(&w, letter);
	w_raw(&w, s->session_id, s->session_id_len);
	dl = 0;
	EVP_Digest(buf, w.len, hash, &dl, kex_md(s), NULL);
	n = (int) dl;
	while (have < need) {
		int c = need - have < n ? need - have : n;

		memcpy(out + have, hash, c);
		have += c;
		if (have >= need)
			break;
		w_init(&w, buf, sizeof(buf));
		w_raw(&w, k, k_len);
		w_raw(&w, h, h_len);
		w_raw(&w, out, have);
		dl = 0;
		EVP_Digest(buf, w.len, hash, &dl, kex_md(s), NULL);
	}
}

static void free_keys(Keys *k)
{
	if (k->cipher)
		EVP_CIPHER_CTX_free(k->cipher);
	memset(k, 0, sizeof(Keys));
}

static int make_keys(Ssh *s, Keys *k, int cipher, int mac, const unsigned char *kk, int kk_len,
					 const unsigned char *h, int h_len, char iv_l, char key_l, char mac_l)
{
	unsigned char iv[16], key[32];
	int klen = cipher == CIPHER_AES256 ? 32 : 16;

	free_keys(k);
	derive(s, kk, kk_len, h, h_len, iv_l, iv, 16);
	derive(s, kk, kk_len, h, h_len, key_l, key, klen);
	k->mac_len = mac == MAC_SHA256 ? 32 : 20;
	k->mac_md = mac == MAC_SHA256 ? EVP_sha256() : EVP_sha1();
	derive(s, kk, kk_len, h, h_len, mac_l, k->mac_key, k->mac_len);
	k->cipher = EVP_CIPHER_CTX_new();
	if (!k->cipher || EVP_EncryptInit_ex(k->cipher, cipher == CIPHER_AES256 ? EVP_aes_256_ctr() : EVP_aes_128_ctr(),
										 NULL, key, iv) != 1)
		return fail(s, "cannot set up the cipher");
	memset(key, 0, sizeof(key));
	return 1;
}

/* curve25519: our ephemeral key; returns the shared secret K (raw, 32 bytes) */
static int kex_curve25519(Ssh *s, W *hb, unsigned char *secret, int *secret_len,
						  const unsigned char **ks, int *ks_n, const unsigned char **sig, int *sig_n)
{
	EVP_PKEY_CTX *pc = NULL, *dc = NULL;
	EVP_PKEY *mine = NULL, *peer = NULL;
	unsigned char qc[32];
	const unsigned char *qs;
	size_t len = 32;
	int qs_n, t, ok = 0;
	W w;
	R r;

	pc = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
	if (!pc || EVP_PKEY_keygen_init(pc) != 1 || EVP_PKEY_keygen(pc, &mine) != 1
		|| EVP_PKEY_get_raw_public_key(mine, qc, &len) != 1 || len != 32) {
		fail(s, "curve25519 key generation failed");
		goto out;
	}
	w_init(&w, s->tmp, 64);
	w_byte(&w, MSG_KEX_INIT);
	w_str(&w, qc, 32);
	if (!send_packet(s, w.p, w.len))
		goto out;
	t = recv_msg(s);
	if (t != MSG_KEX_REPLY) {
		fail(s, "unexpected answer to the key exchange");
		goto out;
	}
	r_init(&r, s->payload + 1, s->payload_len - 1);
	*ks = r_str(&r, ks_n);
	qs = r_str(&r, &qs_n);
	*sig = r_str(&r, sig_n);
	if (r.err || qs_n != 32) {
		fail(s, "bad curve25519 reply");
		goto out;
	}
	peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, qs, 32);
	dc = EVP_PKEY_CTX_new(mine, NULL);
	len = 32;
	if (!peer || !dc || EVP_PKEY_derive_init(dc) != 1 || EVP_PKEY_derive_set_peer(dc, peer) != 1
		|| EVP_PKEY_derive(dc, secret, &len) != 1 || len != 32) {
		fail(s, "curve25519 key agreement failed");
		goto out;
	}
	*secret_len = 32;
	w_str(hb, *ks, *ks_n);
	w_str(hb, qc, 32);
	w_str(hb, qs, 32);
	ok = 1;
out:
	if (dc)
		EVP_PKEY_CTX_free(dc);
	if (pc)
		EVP_PKEY_CTX_free(pc);
	if (peer)
		EVP_PKEY_free(peer);
	if (mine)
		EVP_PKEY_free(mine);
	return ok;
}

/* diffie-hellman group 14 */
static int kex_dh14(Ssh *s, W *hb, unsigned char *secret, int *secret_len,
					const unsigned char **ks, int *ks_n, const unsigned char **sig, int *sig_n)
{
	BIGNUM *p = NULL, *g = NULL, *x = NULL, *e = NULL, *f = NULL, *k = NULL, *pm1 = NULL;
	BN_CTX *bc = NULL;
	unsigned char xb[32], eb[260];
	const unsigned char *fb;
	int fb_n, en, t, ok = 0;
	W w;
	R r;

	bc = BN_CTX_new();
	p = BN_new();
	g = BN_new();
	e = BN_new();
	k = BN_new();
	pm1 = BN_new();
	RAND_bytes(xb, 32);
	xb[0] |= 0x80;
	x = BN_bin2bn(xb, 32, NULL);
	if (!bc || !p || !g || !e || !k || !pm1 || !x || !BN_hex2bn(&p, dh14_p) || !BN_set_word(g, 2)
		|| !BN_mod_exp(e, g, x, p, bc) || !BN_sub(pm1, p, BN_value_one())) {
		fail(s, "diffie-hellman setup failed");
		goto out;
	}
	en = BN_bn2bin(e, eb);
	w_init(&w, s->tmp, 300);
	w_byte(&w, MSG_KEX_INIT);
	w_mpint(&w, eb, en);
	if (!send_packet(s, w.p, w.len))
		goto out;
	t = recv_msg(s);
	if (t != MSG_KEX_REPLY) {
		fail(s, "unexpected answer to the key exchange");
		goto out;
	}
	r_init(&r, s->payload + 1, s->payload_len - 1);
	*ks = r_str(&r, ks_n);
	fb = r_str(&r, &fb_n);
	*sig = r_str(&r, sig_n);
	f = BN_bin2bn(fb, fb_n, NULL);
	if (r.err || !f || BN_is_zero(f) || BN_is_one(f) || BN_cmp(f, pm1) >= 0) {
		fail(s, "bad diffie-hellman reply");
		goto out;
	}
	if (!BN_mod_exp(k, f, x, p, bc)) {
		fail(s, "diffie-hellman failed");
		goto out;
	}
	*secret_len = BN_bn2bin(k, secret);
	w_str(hb, *ks, *ks_n);
	w_mpint(hb, eb, en);
	w_str(hb, fb, fb_n);			/* f as received: already an mpint */
	ok = 1;
out:
	memset(xb, 0, sizeof(xb));
	if (bc) BN_CTX_free(bc);
	if (p) BN_free(p);
	if (g) BN_free(g);
	if (x) BN_free(x);
	if (e) BN_free(e);
	if (f) BN_free(f);
	if (k) BN_free(k);
	if (pm1) BN_free(pm1);
	return ok;
}

static int do_kex(Ssh *s, const unsigned char *server_kexinit, int server_len)
{
	unsigned char *hbuf, secret[260], h[64], kenc[270];
	const unsigned char *ks, *sig;
	int ks_n, sig_n, secret_len = 0, t, ok = 0, h_len;
	unsigned int dl;
	W hb, kw;

	s->in_kex = 1;
	if (!send_kexinit(s))
		return 0;
	if (!server_kexinit) {
		t = recv_msg(s);
		if (t != MSG_KEXINIT)
			return fail(s, "the server did not send KEXINIT");
		server_kexinit = s->payload;
		server_len = s->payload_len;
	}
	free(s->i_s);
	s->i_s = (unsigned char *) malloc(server_len);
	if (!s->i_s)
		return fail(s, "out of memory");
	memcpy(s->i_s, server_kexinit, server_len);
	s->i_s_len = server_len;
	if (!parse_kexinit(s))
		return 0;

	hbuf = (unsigned char *) malloc(KEXBUF_SIZE);
	if (!hbuf)
		return fail(s, "out of memory");
	w_init(&hb, hbuf, KEXBUF_SIZE);
	w_cstr(&hb, s->v_c);
	w_cstr(&hb, s->v_s);
	w_str(&hb, s->i_c, s->i_c_len);
	w_str(&hb, s->i_s, s->i_s_len);
	if (s->kex == KEX_C25519 || s->kex == KEX_C25519_LIBSSH)
		t = kex_curve25519(s, &hb, secret, &secret_len, &ks, &ks_n, &sig, &sig_n);
	else
		t = kex_dh14(s, &hb, secret, &secret_len, &ks, &ks_n, &sig, &sig_n);
	if (!t)
		goto out;
	w_mpint(&hb, secret, secret_len);
	if (hb.err) {
		fail(s, "key exchange data too large");
		goto out;
	}
	dl = 0;
	EVP_Digest(hbuf, hb.len, h, &dl, kex_md(s), NULL);
	h_len = (int) dl;
	if (!verify_hostkey(s, ks, ks_n, sig, sig_n, h, h_len))
		goto out;
	if (!s->session_id_len) {
		memcpy(s->session_id, h, h_len);
		s->session_id_len = h_len;
	}

	/* K as an mpint, for the key derivation */
	w_init(&kw, kenc, sizeof(kenc));
	w_mpint(&kw, secret, secret_len);
	if (!make_keys(s, &s->pend_out, s->cipher_c2s, s->mac_c2s, kenc, kw.len, h, h_len, 'A', 'C', 'E')
		|| !make_keys(s, &s->pend_in, s->cipher_s2c, s->mac_s2c, kenc, kw.len, h, h_len, 'B', 'D', 'F'))
		goto out;

	/* NEWKEYS: our keys change after sending it, the server ones after receiving it */
	{
		unsigned char nk = MSG_NEWKEYS;

		if (!send_packet(s, &nk, 1))
			goto out;
	}
	free_keys(&s->kout);
	s->kout = s->pend_out;
	memset(&s->pend_out, 0, sizeof(Keys));
	s->keyed_out = 1;
	if (s->strict)
		s->seq_out = 0;
	t = recv_msg(s);
	if (t != MSG_NEWKEYS) {
		fail(s, "the server did not send NEWKEYS");
		goto out;
	}
	free_keys(&s->kin);
	s->kin = s->pend_in;
	memset(&s->pend_in, 0, sizeof(Keys));
	s->keyed_in = 1;
	if (s->strict)
		s->seq_in = 0;
	s->kex_count++;
	ok = 1;
out:
	memset(secret, 0, sizeof(secret));
	memset(kenc, 0, sizeof(kenc));
	free(hbuf);
	s->in_kex = 0;
	return ok;
}

/* ---------------------------------------------------------------------------
 * Connection
 * ------------------------------------------------------------------------- */
Ssh *ssh_new(SshIo *io, SshLog log, SshHostKeyCheck check, void *ctx)
{
	Ssh *s = (Ssh *) malloc(sizeof(Ssh));

	if (!s)
		return NULL;
	memset(s, 0, sizeof(Ssh));
	s->io = *io;
	s->log = log;
	s->check = check;
	s->ctx = ctx;
	s->rx = (unsigned char *) malloc(RX_SIZE);
	s->pktbuf = (unsigned char *) malloc(PKT_MAX + 8);
	s->outbuf = (unsigned char *) malloc(PKT_MAX + 4 + 64);
	s->tmp = (unsigned char *) malloc(PKT_MAX);
	s->cbuf = (unsigned char *) malloc(CHAN_WINDOW);
	if (!s->rx || !s->pktbuf || !s->outbuf || !s->tmp || !s->cbuf) {
		ssh_free(s);
		return NULL;
	}
	return s;
}

void ssh_free(Ssh *s)
{
	if (!s)
		return;
	free_keys(&s->kin);
	free_keys(&s->kout);
	free_keys(&s->pend_in);
	free_keys(&s->pend_out);
	free(s->rx);
	free(s->pktbuf);
	free(s->outbuf);
	free(s->tmp);
	free(s->cbuf);
	free(s->i_c);
	free(s->i_s);
	memset(s, 0, sizeof(Ssh));
	free(s);
}

const char *ssh_error(Ssh *s)
{
	return s->err[0] ? s->err : "unknown error";
}

static int version_exchange(Ssh *s)
{
	char line[256];
	int n, lines = 0;
	unsigned char c;

	strcpy(s->v_c, "SSH-2.0-MarranoFTP_1.0");
	sprintf(line, "%s\r\n", s->v_c);
	if (!raw_put(s, (unsigned char *) line, strlen(line)))
		return 0;
	/* the server may send other lines before its version */
	for (;;) {
		n = 0;
		for (;;) {
			if (!raw_get(s, &c, 1))
				return 0;
			if (c == '\n')
				break;
			if (c != '\r' && n < (int) sizeof(line) - 1)
				line[n++] = c;
		}
		line[n] = 0;
		if (!strncmp(line, "SSH-", 4))
			break;
		if (++lines > 50)
			return fail(s, "not an SSH server");
	}
	if (strncmp(line, "SSH-2.0-", 8) && strncmp(line, "SSH-1.99-", 9))
		return fail(s, "the server does not speak SSH-2");
	strcpy(s->v_s, line);
	sprintf(line, "SSH: server %.100s", s->v_s);
	say(s, line);
	return 1;
}

static int userauth(Ssh *s, const char *user, const char *password)
{
	W w;
	R r;
	int t, n, tried_kbd = 0, i, prompts;
	const unsigned char *methods;
	char text[200];

	w_init(&w, s->tmp, 64);
	w_byte(&w, MSG_SERVICE_REQUEST);
	w_cstr(&w, "ssh-userauth");
	if (!send_packet(s, w.p, w.len))
		return 0;
	t = recv_msg(s);
	if (t != MSG_SERVICE_ACCEPT)
		return t ? fail(s, "user authentication refused") : 0;

	/* password first */
	w_init(&w, s->tmp, 1024);
	w_byte(&w, MSG_USERAUTH_REQUEST);
	w_cstr(&w, user);
	w_cstr(&w, "ssh-connection");
	w_cstr(&w, "password");
	w_byte(&w, 0);
	w_cstr(&w, password);
	if (w.err || !send_packet(s, w.p, w.len))
		return fail(s, "user name or password too long");

	for (;;) {
		t = recv_msg(s);
		r_init(&r, s->payload + 1, s->payload_len - 1);
		switch (t) {
			case 0:
				return 0;
			case MSG_USERAUTH_SUCCESS:
				say(s, "SSH: authenticated");
				return 1;
			case MSG_USERAUTH_BANNER:
				strcpy(text, "SSH banner: ");
				r_cstr(&r, text + strlen(text), sizeof(text) - strlen(text));
				for (i = 0; text[i]; i++)
					if (text[i] == '\r' || text[i] == '\n')
						text[i] = ' ';
				say(s, text);
				break;
			case MSG_USERAUTH_FAILURE:
				methods = r_str(&r, &n);
				if (!tried_kbd && in_list("keyboard-interactive", methods, n)) {
					tried_kbd = 1;
					w_init(&w, s->tmp, 1024);
					w_byte(&w, MSG_USERAUTH_REQUEST);
					w_cstr(&w, user);
					w_cstr(&w, "ssh-connection");
					w_cstr(&w, "keyboard-interactive");
					w_cstr(&w, "");
					w_cstr(&w, "");
					if (!send_packet(s, w.p, w.len))
						return 0;
					break;
				}
				if (!in_list("password", methods, n) && !in_list("keyboard-interactive", methods, n)) {
					sprintf(text, "the server does not accept passwords (it wants: %.*s)", n > 100 ? 100 : n,
							(const char *) methods);
					return fail(s, text);
				}
				return fail(s, "authentication failed: wrong user name or password");
			case MSG_USERAUTH_INFO_REQUEST:
				if (!tried_kbd)
					return fail(s, "the server asks to change the password");
				r_str(&r, &n);			/* name */
				r_str(&r, &n);			/* instruction */
				r_str(&r, &n);			/* language */
				prompts = (int) r_u32(&r);
				if (r.err || prompts < 0 || prompts > 10)
					return fail(s, "bad keyboard-interactive request");
				/* the password answers every prompt */
				w_init(&w, s->tmp, 4096);
				w_byte(&w, MSG_USERAUTH_INFO_RESPONSE);
				w_u32(&w, (unsigned long) prompts);
				for (i = 0; i < prompts; i++)
					w_cstr(&w, password);
				if (!send_packet(s, w.p, w.len))
					return 0;
				break;
			default:
				return fail(s, "unexpected message during the authentication");
		}
	}
}

static int open_sftp_channel(Ssh *s)
{
	W w;
	R r;
	int t;

	w_init(&w, s->tmp, 64);
	w_byte(&w, MSG_CHANNEL_OPEN);
	w_cstr(&w, "session");
	w_u32(&w, 0);					/* our channel number */
	w_u32(&w, CHAN_WINDOW);
	w_u32(&w, CHAN_MAXPKT);
	if (!send_packet(s, w.p, w.len))
		return 0;
	t = recv_msg(s);
	if (t != MSG_CHANNEL_OPEN_CONFIRMATION)
		return t ? fail(s, "the server refused the session channel") : 0;
	r_init(&r, s->payload + 1, s->payload_len - 1);
	r_u32(&r);
	s->remote_chan = r_u32(&r);
	s->remote_window = r_u32(&r);
	s->remote_maxpkt = r_u32(&r);
	if (s->remote_maxpkt > OUT_MAXDATA)
		s->remote_maxpkt = OUT_MAXDATA;
	if (s->remote_maxpkt < 512)
		s->remote_maxpkt = 512;
	s->chan_open = 1;

	w_init(&w, s->tmp, 64);
	w_byte(&w, MSG_CHANNEL_REQUEST);
	w_u32(&w, s->remote_chan);
	w_cstr(&w, "subsystem");
	w_byte(&w, 1);
	w_cstr(&w, "sftp");
	if (!send_packet(s, w.p, w.len))
		return 0;
	t = recv_msg(s);
	if (t != MSG_CHANNEL_SUCCESS)
		return t ? fail(s, "the server has no sftp subsystem") : 0;
	return 1;
}

int ssh_connect(Ssh *s, const char *user, const char *password)
{
	return version_exchange(s) && do_kex(s, NULL, 0) && userauth(s, user, password)
		   && open_sftp_channel(s);
}

/* ---------------------------------------------------------------------------
 * Channel I/O
 * ------------------------------------------------------------------------- */
int ssh_chan_write(Ssh *s, const unsigned char *data, int len)
{
	int chunk, t;
	W w;

	while (len > 0) {
		while (s->remote_window == 0) {
			s->chan_wait = 1;
			t = s->chan_closed ? 0 : recv_msg(s);
			s->chan_wait = 0;
			if (!t)
				return 0;
		}
		chunk = len;
		if ((unsigned long) chunk > s->remote_window)
			chunk = (int) s->remote_window;
		if ((unsigned long) chunk > s->remote_maxpkt)
			chunk = (int) s->remote_maxpkt;
		w_init(&w, s->tmp, PKT_MAX - 64);
		w_byte(&w, MSG_CHANNEL_DATA);
		w_u32(&w, s->remote_chan);
		w_str(&w, data, chunk);
		if (!send_packet(s, w.p, w.len))
			return 0;
		s->remote_window -= chunk;
		data += chunk;
		len -= chunk;
	}
	return 1;
}

int ssh_chan_pending(Ssh *s)
{
	return s->cbuf_len - s->cbuf_pos;
}

int ssh_chan_read(Ssh *s, unsigned char *buf, int len)
{
	int n;

	while (s->cbuf_pos == s->cbuf_len) {
		if (s->chan_closed)
			return fail(s, "the sftp channel is closed");
		s->chan_wait = 1;
		n = recv_msg(s);
		s->chan_wait = 0;
		if (!n)
			return 0;
	}
	n = s->cbuf_len - s->cbuf_pos;
	if (n > len)
		n = len;
	memcpy(buf, s->cbuf + s->cbuf_pos, n);
	s->cbuf_pos += n;
	s->to_adjust += n;
	if (s->to_adjust >= CHAN_WINDOW / 2) {
		if (!send_window_adjust(s, s->to_adjust))
			return 0;
		s->to_adjust = 0;
	}
	return n;
}

void ssh_disconnect(Ssh *s)
{
	W w;

	if (!s->keyed_out)
		return;
	if (s->chan_open && !s->chan_closed) {
		w_init(&w, s->tmp, 16);
		w_byte(&w, MSG_CHANNEL_CLOSE);
		w_u32(&w, s->remote_chan);
		send_packet(s, w.p, w.len);
	}
	w_init(&w, s->tmp, 64);
	w_byte(&w, MSG_DISCONNECT);
	w_u32(&w, 11);					/* SSH_DISCONNECT_BY_APPLICATION */
	w_cstr(&w, "bye");
	w_cstr(&w, "");
	send_packet(s, w.p, w.len);
}
