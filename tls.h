/*
 * TLS.H - TLS for FTPS (RFC 4217) on top of the sockets of the FTP core.
 *
 * AmiSSL does not touch the sockets: each TLS connection uses two memory
 * BIOs and the bytes are moved with the same send()/recv() as plain FTP,
 * so it works with any TCP/IP stack and with the non blocking sockets of
 * the core. tls_send()/tls_recv() behave like send()/recv() on sockets
 * without TLS.
 */
#ifndef MFTP_TLS_H
#define MFTP_TLS_H

/* tls_recv()/tls_send(): the socket was readable but no whole TLS record is
   there yet, or the data connection handshake is not done: try again later */
#define TLS_NO_DATA (-2)

/* Opens AmiSSL (first call) and the TLS context; FALSE if not available */
int tls_init(void);
void tls_cleanup(void);

/*
 * Handshake on a connected (or connecting) socket. host: server name for
 * SNI and certificate checks, NULL for a data connection; reuse_from: socket
 * of the control connection whose TLS session is reused (most servers
 * require it on the data connection), -1 for none. Messages go to the GUI.
 */
int tls_start(long sock, const char *host, int port, long reuse_from);
void tls_close(long sock);				/* close_notify, frees; no-op without TLS */

int tls_send(long sock, const void *buf, int len);
int tls_recv(long sock, void *buf, int len);
int tls_active(long sock);
int tls_pending(long sock);				/* decrypted or buffered data waiting */

#endif
