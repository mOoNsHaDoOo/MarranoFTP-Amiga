/*
 * SSH.H - minimal SSH-2 client: transport, user authentication and one
 * "session" channel running the "sftp" subsystem (RFC 4251-4254).
 *
 *   key exchange : curve25519-sha256, diffie-hellman-group14-sha256/sha1
 *   host keys    : ssh-ed25519, rsa-sha2-256, rsa-sha2-512, ssh-rsa
 *   ciphers      : aes128-ctr, aes256-ctr
 *   MACs         : hmac-sha2-256, hmac-sha1
 *   user auth    : password, keyboard-interactive
 *
 * Plain C89 without 64 bit integers (SAS/C 6). The crypto comes from
 * OpenSSL (AmiSSL on the Amiga, see ssh_crypto.h). The socket is used
 * through the SshIo callbacks, so this file has no network code.
 */
#ifndef MFTP_SSH_H
#define MFTP_SSH_H

typedef struct SshIo {
	void *ctx;
	/* both return the number of bytes moved, <= 0 on error/timeout/EOF;
	   recv returns as soon as some data is available */
	int (*send)(void *ctx, const unsigned char *buf, int len);
	int (*recv)(void *ctx, unsigned char *buf, int len);
} SshIo;

/* Host key check: type ("ssh-ed25519", "ssh-rsa") and fingerprint
   ("SHA256:base64"); return 1 to accept the key, 0 to refuse it */
typedef int (*SshHostKeyCheck)(void *ctx, const char *type, const char *fingerprint);
/* Progress and server messages (banner, algorithms...) */
typedef void (*SshLog)(void *ctx, const char *text);

typedef struct Ssh Ssh;

Ssh *ssh_new(SshIo *io, SshLog log, SshHostKeyCheck check, void *ctx);
void ssh_free(Ssh *s);
const char *ssh_error(Ssh *s);

/* Version exchange, key exchange, authentication, channel + sftp subsystem.
   Returns 1 when the "sftp" channel is ready. */
int ssh_connect(Ssh *s, const char *user, const char *password);

/* Channel I/O: write all the bytes, read at least one byte (blocking) */
int ssh_chan_write(Ssh *s, const unsigned char *data, int len);
int ssh_chan_read(Ssh *s, unsigned char *buf, int len);
/* Bytes already received and buffered on the channel */
int ssh_chan_pending(Ssh *s);

/* Polite close: channel close + disconnect message */
void ssh_disconnect(Ssh *s);

#endif
