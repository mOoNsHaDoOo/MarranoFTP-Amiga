/*
 * FTP_LIST.H - parser of the lines sent by FTP servers in answer to LIST.
 *
 * Plain C with no Amiga or CafLib dependency, so it can be tested on any
 * machine. The approach is the one of FileZilla's directory listing parser:
 * for Unix lines the date is searched, the size is the number before it and
 * the name is what follows it, so the number of owner/group fields and the
 * presence of the link count do not matter.
 *
 * Formats:
 *   Unix   drwxr-xr-x 2 user group 4096 Nov 13 10:00 name
 *          -rw-r--r-- 1 user 1234 Nov 13  2025 name      (no group)
 *          -rw-r--r-- 1 user group 1234 2026-11-13 10:00 name   (ISO date)
 *          -rw-r--r-- 1 user group 1234 ott 13 10:00 name       (it/de/fr/es)
 *          -rw-r--r-- 1 user group 1234 13 nov 10:00 name       (day first)
 *          lrwxrwxrwx 1 root root 12 Nov 13 10:00 www -> /web/htdocs
 *          d [RWCEAFMS] owner 512 Nov 13 10:00 name             (Netware)
 *   DOS    11-13-26  03:00PM       <DIR>          name           (IIS)
 *          11-13-2026  15:00              1234 name
 *          2026-11-13  15:00              1234 name
 *   EPLF   +i8388621.48594,m825718503,r,s280,\tname
 * "total N" lines, "." and ".." are skipped. A trailing '/', '*' or '|'
 * (ls -F) is removed from the names; symbolic links keep only the name and
 * are reported as directories (on web hostings they usually are).
 */
#ifndef MFTP_FTP_LIST_H
#define MFTP_FTP_LIST_H

typedef struct ListEntry {
	const char *name;		/* points into the line ... */
	int name_len;			/* ... not terminated */
	char size[24];
	char flags[16];
	char date[40];
	int is_dir;
} ListEntry;

enum { LIST_UNKNOWN = -1, LIST_SKIP = 0, LIST_ENTRY = 1 };

/* LIST_ENTRY: e is filled, LIST_SKIP: "total", ".", "..", empty line,
   LIST_UNKNOWN: format not recognized */
int ftp_parse_list_line(const char *line, ListEntry *e);

#endif
