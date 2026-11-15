/*
 * CORE_SFTP.H - SFTP sessions on the Amiga: AmiSSL, socket and known host
 * keys around ssh.c/sftp.c. Used by ftp_core.c when the address book entry
 * is of type SFTP.
 *
 * AmiSSL (OpenSSL 3) needs AmigaOS 3.0+ and a 68020+; it is opened at the
 * first SFTP connection, so the programs still run without it.
 */
#ifndef MFTP_CORE_SFTP_H
#define MFTP_CORE_SFTP_H

#include "sftp.h"

/* Connects, checks the host key, logs in and starts the sftp subsystem.
   Progress and errors go to gui_status(). NULL on error. */
Sftp *csftp_open(const char *host, int port, const char *user, const char *password);
void csftp_close(Sftp *f);
/* Closes AmiSSL, at the end of the program */
void csftp_cleanup(void);

/* Shared with tls.c (FTPS) */
bool core_amissl_open(void);
/* Trust on first use: 1 if the key/certificate fingerprint of host:port is
   the saved one or new (then it is saved), 0 if it changed */
int core_known_host(const char *host, int port, const char *type, const char *fingerprint);

#endif
