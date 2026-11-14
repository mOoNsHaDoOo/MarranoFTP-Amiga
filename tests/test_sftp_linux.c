/*
 * Linux test of ssh.c + sftp.c against a local OpenSSH server
 * (127.0.0.1, port 2222 or $PORT, user "amiga", password = argv[1] or "marrano").
 * The same sources build on the Amiga with AmiSSL, here with OpenSSL:
 *
 *   gcc -std=gnu89 -Wall -I.. -o test_sftp test_sftp_linux.c ../ssh.c ../sftp.c -lcrypto
 *
 * It lists the home directory, downloads pub/archivio.lha to dl.bin, uploads
 * it back as up.bin, then tests mkdir, rename, remove and rmdir.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ssh.h"
#include "sftp.h"

static int io_send(void *c, const unsigned char *b, int n) { return (int) send(*(int *) c, b, n, 0); }
static int io_recv(void *c, unsigned char *b, int n) { return (int) recv(*(int *) c, b, n, 0); }
static void lg(void *c, const char *t) { (void) c; printf("  log: %s\n", t); }
static int check(void *c, const char *type, const char *fp) { (void) c; printf("  hostkey %s %s -> accettata\n", type, fp); return 1; }
static int dircb(void *c, const char *name, const char *ln, const SftpAttr *a)
{ (void) c; (void) ln; printf("  %-6s %8lu %s\n", SFTP_IS_DIR(a) ? "<DIR>" : "", a->size_lo, name); return 1; }

int main(int argc, char **argv)
{
	int sock = socket(AF_INET, SOCK_STREAM, 0), n, r, total = 0;
	struct sockaddr_in sa;
	SshIo io;
	Ssh *s;
	Sftp *f;
	char path[512];
	const unsigned char *d;
	FILE *fp;
	unsigned char buf[50000];

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(getenv("PORT") ? atoi(getenv("PORT")) : 2222);
	sa.sin_addr.s_addr = inet_addr("127.0.0.1");
	if (connect(sock, (struct sockaddr *) &sa, sizeof(sa))) { perror("connect"); return 1; }
	io.ctx = &sock; io.send = io_send; io.recv = io_recv;
	s = ssh_new(&io, lg, check, NULL);
	if (!ssh_connect(s, "amiga", argc > 1 ? argv[1] : "marrano")) { printf("ERRORE ssh: %s\n", ssh_error(s)); return 1; }
	f = sftp_new(s);
	if (!f) { printf("ERRORE sftp init: %s\n", ssh_error(s)); return 1; }
	if (!sftp_realpath(f, ".", path, sizeof(path))) { printf("ERRORE realpath: %s\n", sftp_error(f)); return 1; }
	printf("cwd = %s\n", path);
	if (!sftp_list(f, ".", dircb, NULL)) printf("ERRORE list: %s\n", sftp_error(f));
	/* download */
	if (!sftp_get_begin(f, "pub/archivio.lha")) { printf("ERRORE get: %s\n", sftp_error(f)); return 1; }
	fp = fopen("dl.bin", "wb");
	while ((r = sftp_get_step(f, &d, &n)) == 1) { fwrite(d, 1, n, fp); total += n; }
	fclose(fp);
	printf("download: r=%d %d byte %s\n", r, total, r < 0 ? sftp_error(f) : "");
	/* upload */
	fp = fopen("dl.bin", "rb");
	if (!sftp_put_begin(f, "up.bin")) { printf("ERRORE put: %s\n", sftp_error(f)); return 1; }
	total = 0;
	while ((n = (int) fread(buf, 1, 20000, fp)) > 0) { if (!sftp_put_step(f, buf, n)) { printf("ERRORE put step: %s\n", sftp_error(f)); return 1; } total += n; }
	fclose(fp);
	printf("upload: %s %d byte\n", sftp_put_end(f) ? "ok" : sftp_error(f), total);
	printf("mkdir: %s\n", sftp_mkdir(f, "nuova") ? "ok" : sftp_error(f));
	printf("rename: %s\n", sftp_rename(f, "up.bin", "nuova/up2.bin") ? "ok" : sftp_error(f));
	printf("remove inesistente: %s\n", sftp_remove(f, "non_esiste") ? "ok?!" : sftp_error(f));
	printf("remove: %s\n", sftp_remove(f, "nuova/up2.bin") ? "ok" : sftp_error(f));
	printf("rmdir: %s\n", sftp_rmdir(f, "nuova") ? "ok" : sftp_error(f));
	sftp_free(f);
	ssh_disconnect(s);
	ssh_free(s);
	close(sock);
	return 0;
}
