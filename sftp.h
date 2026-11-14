/*
 * SFTP.H - SFTP version 3 client (draft-ietf-secsh-filexfer-02) on top of
 * the ssh.c channel. Plain C89, no 64 bit integers: file sizes and offsets
 * are kept as two 32 bit halves, transfers are limited to 4 GB.
 *
 * Transfers are split in steps so the caller can keep its user interface
 * alive: several READ/WRITE requests are kept in flight (pipelining).
 */
#ifndef MFTP_SFTP_H
#define MFTP_SFTP_H

#include "ssh.h"

typedef struct SftpAttr {
	unsigned long flags;
	unsigned long size_hi, size_lo;
	unsigned long perm;			/* POSIX mode bits */
	unsigned long mtime;		/* seconds since 1970 */
} SftpAttr;

#define SFTP_ATTR_SIZE			0x01
#define SFTP_ATTR_PERMISSIONS	0x04
#define SFTP_ATTR_ACMODTIME		0x08
#define SFTP_IS_DIR(a)	(((a)->flags & SFTP_ATTR_PERMISSIONS) && ((a)->perm & 0170000UL) == 0040000UL)
#define SFTP_IS_LINK(a)	(((a)->flags & SFTP_ATTR_PERMISSIONS) && ((a)->perm & 0170000UL) == 0120000UL)

typedef struct Sftp Sftp;

/* Directory entries; return 0 to stop */
typedef int (*SftpDirCb)(void *ctx, const char *name, const char *longname, const SftpAttr *a);

Sftp *sftp_new(Ssh *ssh);				/* SSH_FXP_INIT, NULL on error (see ssh_error) */
void sftp_free(Sftp *f);
const char *sftp_error(Sftp *f);
int sftp_broken(Sftp *f);				/* 1: the connection is gone */

int sftp_realpath(Sftp *f, const char *path, char *out, int outlen);
int sftp_stat(Sftp *f, const char *path, SftpAttr *a);
int sftp_list(Sftp *f, const char *path, SftpDirCb cb, void *ctx);
int sftp_remove(Sftp *f, const char *path);
int sftp_rmdir(Sftp *f, const char *path);
int sftp_mkdir(Sftp *f, const char *path);
int sftp_rename(Sftp *f, const char *from, const char *to);

/* Download: get_begin, then get_step until it returns 0 (end of file) or
   -1 (error); 1 means that data and len hold the next bytes of the file */
int sftp_get_begin(Sftp *f, const char *path);
int sftp_get_step(Sftp *f, const unsigned char **data, int *len);
/* Upload: put_begin, put_step for each block, put_end */
int sftp_put_begin(Sftp *f, const char *path);
int sftp_put_step(Sftp *f, const unsigned char *data, int len);
int sftp_put_end(Sftp *f);
/* Closes the file of a transfer in progress (stop, error) */
void sftp_xfer_abort(Sftp *f);

#define SFTP_BLOCK	16384		/* bytes per READ/WRITE request */

#endif
