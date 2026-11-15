/*
 * Linux test of tls.c against a local FTPS server (vsftpd with
 * require_ssl_reuse=YES), explicit (AUTH TLS) or implicit:
 *
 *   gcc -std=gnu89 -Wall -I.. -o test_ftps test_ftps_linux.c ../tls.c -lssl -lcrypto
 *   ./test_ftps 2121          explicit
 *   ./test_ftps 9990 i        implicit
 *
 * Downloads immagine.iff (dl.iff), uploads it back as up.iff, lists "/".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "tls.h"

void gui_status(const char *text) { printf("  [%s]\n", text); }
int core_known_host(const char *host, int port, const char *type, const char *fp)
{ printf("  [known host %s:%d %s %s -> accettato]\n", host, port, type, fp); return 1; }

static int ctrl;
static char line[1024];

static int tcp(int port)
{
	struct sockaddr_in a;
	int s = socket(AF_INET, SOCK_STREAM, 0);

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = inet_addr("127.0.0.1");
	if (connect(s, (struct sockaddr *) &a, sizeof(a)) < 0) { perror("connect"); exit(1); }
	fcntl(s, F_SETFL, O_NONBLOCK);		/* like the FTP core */
	return s;
}

static int rd(int s, void *buf, int len)
{
	fd_set f;
	int r;

	for (;;) {
		r = tls_recv(s, buf, len);
		if (r != TLS_NO_DATA && !(r < 0 && errno == EAGAIN))
			return r;
		FD_ZERO(&f); FD_SET(s, &f);
		select(s + 1, &f, NULL, NULL, NULL);
	}
}

/* reply: returns the code, multi line replies included */
static int reply(void)
{
	int n = 0, code = 0;
	char c;

	for (;;) {
		n = 0;
		while (rd(ctrl, &c, 1) == 1 && c != '\n')
			if (c != '\r' && n < (int) sizeof(line) - 1)
				line[n++] = c;
		line[n] = 0;
		printf("<- %s\n", line);
		if (n >= 4 && line[3] == ' ') {
			code = atoi(line);
			return code;
		}
		if (n == 0)
			return -1;
	}
}

static int cmd(const char *c)
{
	char b[300];
	int n = sprintf(b, "%s\r\n", c);

	printf("-> %s\n", strncmp(c, "PASS", 4) ? c : "PASS ****");
	tls_send(ctrl, b, n);
	return reply();
}

static int pasv(void)
{
	int h1, h2, h3, h4, p1, p2;
	char *p = strchr(line, '(');

	if (!p || sscanf(p, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6) return -1;
	return tcp(p1 * 256 + p2);
}

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 2121, implicit = argc > 2, d, n, total;
	char buf[8192];
	FILE *fp;

	ctrl = tcp(port);
	if (implicit && !tls_start(ctrl, "127.0.0.1", port, -1)) return 1;
	if (reply() != 220) return 1;
	if (!implicit) {
		if (cmd("AUTH TLS") != 234) return 1;
		if (!tls_start(ctrl, "127.0.0.1", port, -1)) return 1;
	}
	cmd("USER amiga");
	if (cmd("PASS marrano") != 230) return 1;
	cmd("PBSZ 0");
	cmd("PROT P");
	cmd("TYPE I");

	/* download */
	if (cmd("PASV") != 227) return 1;
	d = pasv();
	n = cmd("RETR immagine.iff");
	if (n != 150 && n != 125) return 1;
	if (!tls_start(d, NULL, 0, ctrl)) return 1;
	fp = fopen("dl.iff", "wb");
	total = 0;
	while ((n = rd(d, buf, sizeof(buf))) > 0) { fwrite(buf, 1, n, fp); total += n; }
	fclose(fp);
	tls_close(d); close(d);
	printf("   download: %d byte\n", total);
	reply();

	/* upload */
	if (cmd("PASV") != 227) return 1;
	d = pasv();
	n = cmd("STOR up.iff");
	if (n != 150 && n != 125) return 1;
	if (!tls_start(d, NULL, 0, ctrl)) return 1;
	fp = fopen("dl.iff", "rb");
	total = 0;
	while ((n = (int) fread(buf, 1, sizeof(buf), fp)) > 0) { tls_send(d, buf, n); total += n; }
	fclose(fp);
	tls_close(d); close(d);
	printf("   upload: %d byte\n", total);
	reply();

	/* list */
	if (cmd("PASV") != 227) return 1;
	d = pasv();
	n = cmd("LIST");
	if (n != 150 && n != 125) return 1;
	if (!tls_start(d, NULL, 0, ctrl)) return 1;
	while ((n = rd(d, buf, sizeof(buf) - 1)) > 0) { buf[n] = 0; printf("%s", buf); }
	tls_close(d); close(d);
	reply();
	cmd("QUIT");
	tls_close(ctrl);
	close(ctrl);
	tls_cleanup();
	return 0;
}
