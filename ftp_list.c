/*
 * FTP_LIST.C - parser of the LIST answers, see ftp_list.h.
 */
#include <string.h>
#include "ftp_list.h"

#define MAX_TOKENS 16

typedef struct Tokens {
	const char *line;
	int pos[MAX_TOKENS], len[MAX_TOKENS];
	int n;
} Tokens;

static int is_blank(char c)
{
	return c == ' ' || c == '\t';
}

static void tokenize(const char *line, Tokens *t)
{
	int i = 0;

	t->line = line;
	t->n = 0;
	while (t->n < MAX_TOKENS) {
		while (is_blank(line[i]))
			i++;
		if (!line[i])
			break;
		t->pos[t->n] = i;
		while (line[i] && !is_blank(line[i]))
			i++;
		t->len[t->n] = i - t->pos[t->n];
		t->n++;
	}
}

static const char *tok(Tokens *t, int k)
{
	return t->line + t->pos[k];
}

static void copy_tok(char *dst, int size, Tokens *t, int k)
{
	int n = t->len[k];

	if (n > size - 1)
		n = size - 1;
	memcpy(dst, tok(t, k), n);
	dst[n] = 0;
}

/* dst = tokens first..last separated by one blank */
static void copy_toks(char *dst, int size, Tokens *t, int first, int last)
{
	int k, used = 0, n;

	dst[0] = 0;
	for (k = first; k <= last; k++) {
		n = t->len[k];
		if (used + n + 2 > size)
			break;
		if (used)
			dst[used++] = ' ';
		memcpy(dst + used, tok(t, k), n);
		used += n;
		dst[used] = 0;
	}
}

static int digits(const char *s, int n)
{
	int i;

	if (n <= 0)
		return 0;
	for (i = 0; i < n; i++)
		if (s[i] < '0' || s[i] > '9')
			return 0;
	return 1;
}

static int number(const char *s, int n)
{
	int v = 0, i;

	for (i = 0; i < n; i++)
		v = v * 10 + s[i] - '0';
	return v;
}

static char lower(char c)
{
	if (c >= 'A' && c <= 'Z')
		c = (char) (c - 'A' + 'a');
	return c;
}

/* Month names and abbreviations: English, Italian, German, French, Spanish.
   The accented ones are listed both in ISO-8859-1 and in UTF-8. */
static const char *month_names[] = {
	"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec",
	"gen", "mag", "giu", "lug", "ago", "set", "ott", "dic",
	"mrz", "m\xe4r", "m\xc3\xa4r", "mai", "okt", "dez", "j\xe4n", "j\xc3\xa4n",
	"jui", "f\xe9v", "f\xc3\xa9v", "fev", "avr", "ao\xfb", "ao\xc3\xbb", "aou", "d\xe9" "c", "d\xc3\xa9" "c",
	"ene", "abr",
	NULL
};

static int is_month(const char *s, int n)
{
	char low[8];
	int i, k, m;

	if (n < 3 || n > 7)
		return 0;
	for (i = 0; i < n; i++)
		low[i] = lower(s[i]);
	low[n] = 0;
	if (low[n - 1] == '.')				/* "ott." */
		low[--n] = 0;
	for (k = 0; month_names[k]; k++) {
		m = strlen(month_names[k]);
		if (n >= m && !memcmp(low, month_names[k], m))
			return 1;
	}
	return 0;
}

static int is_day(const char *s, int n)
{
	int d;

	if (n > 0 && s[n - 1] == '.')		/* "13." */
		n--;
	if (n < 1 || n > 2 || !digits(s, n))
		return 0;
	d = number(s, n);
	return d >= 1 && d <= 31;
}

/* hh:mm or hh:mm:ss */
static int is_time(const char *s, int n)
{
	return (n == 4 || n == 5 || n == 7 || n == 8) && s[n == 4 || n == 7 ? 1 : 2] == ':'
		   && digits(s, n == 4 || n == 7 ? 1 : 2);
}

static int is_year(const char *s, int n)
{
	return n == 4 && digits(s, 4) && number(s, 4) >= 1900;
}

/* yyyy-mm-dd (also with '/' or '.') */
static int is_iso_date(const char *s, int n)
{
	return n == 10 && digits(s, 4) && (s[4] == '-' || s[4] == '/' || s[4] == '.')
		   && digits(s + 5, 2) && s[7] == s[4] && digits(s + 8, 2);
}

/* mm-dd-yy, mm-dd-yyyy, dd.mm.yy ... (DOS listings) */
static int is_dos_date(const char *s, int n)
{
	return (n == 8 || n == 10) && digits(s, 2) && (s[2] == '-' || s[2] == '/' || s[2] == '.')
		   && digits(s + 3, 2) && s[5] == s[2] && digits(s + 6, n - 6);
}

/* Unix permission string: "drwxr-xr-x", "-rw-r--r--+", "lrwxrwxrwx", ... */
static int is_unix_flags(const char *s, int n)
{
	return n >= 10 && strchr("-dlbcpsD", s[0]) != NULL && strchr("r-", s[1]) != NULL
		   && strchr("w-", s[2]) != NULL;
}

/* Name = rest of the line from token k: ls -F marks and link targets removed */
static void set_name(ListEntry *e, Tokens *t, int k, int is_link)
{
	const char *name = tok(t, k), *arrow;
	int n = strlen(name);

	while (n > 0 && is_blank(name[n - 1]))
		n--;
	if (is_link) {
		for (arrow = name; arrow + 4 <= name + n; arrow++)
			if (!memcmp(arrow, " -> ", 4)) {
				n = arrow - name;
				break;
			}
	}
	if (n > 1 && (name[n - 1] == '/' || name[n - 1] == '*' || name[n - 1] == '|'))
		n--;
	e->name = name;
	e->name_len = n;
}

static int parse_unix(Tokens *t, ListEntry *e)
{
	int k, first = 1, name_tok = -1, date_first = 0, date_last = 0, is_link;

	if (t->n < 4)
		return LIST_UNKNOWN;
	if (t->len[0] == 1 && strchr("d-", tok(t, 0)[0]) && tok(t, 1)[0] == '[') {
		first = 2;							/* Netware: "d [RWCEAFMS] owner ..." */
	} else if (!is_unix_flags(tok(t, 0), t->len[0]))
		return LIST_UNKNOWN;

	/* the date: the size is the token before it, the name follows it */
	for (k = first + 1; k + 1 < t->n && name_tok < 0; k++) {
		if (!digits(tok(t, k - 1), t->len[k - 1]))
			continue;
		if (k + 2 < t->n && is_month(tok(t, k), t->len[k]) && is_day(tok(t, k + 1), t->len[k + 1])
			&& (is_time(tok(t, k + 2), t->len[k + 2]) || is_year(tok(t, k + 2), t->len[k + 2]))) {
			name_tok = k + 3;				/* Nov 13 10:00 / Nov 13 2025 */
			date_last = k + 2;
		} else if (k + 2 < t->n && is_day(tok(t, k), t->len[k]) && is_month(tok(t, k + 1), t->len[k + 1])
				   && (is_time(tok(t, k + 2), t->len[k + 2]) || is_year(tok(t, k + 2), t->len[k + 2]))) {
			name_tok = k + 3;				/* 13 nov 10:00 */
			date_last = k + 2;
		} else if (is_iso_date(tok(t, k), t->len[k]) && is_time(tok(t, k + 1), t->len[k + 1])) {
			name_tok = k + 2;				/* 2026-11-13 10:00 */
			date_last = k + 1;
		}
		if (name_tok >= 0)
			date_first = k;
	}
	if (name_tok < 0 || name_tok >= t->n)
		return LIST_UNKNOWN;

	is_link = tok(t, 0)[0] == 'l';
	copy_tok(e->flags, sizeof(e->flags), t, 0);
	copy_tok(e->size, sizeof(e->size), t, date_first - 1);
	copy_toks(e->date, sizeof(e->date), t, date_first, date_last);
	e->is_dir = tok(t, 0)[0] == 'd' || tok(t, 0)[0] == 'D' || is_link;
	set_name(e, t, name_tok, is_link);
	return LIST_ENTRY;
}

static int parse_dos(Tokens *t, ListEntry *e)
{
	int k = 1, n;

	if (t->n < 4 || !(is_dos_date(tok(t, 0), t->len[0]) || is_iso_date(tok(t, 0), t->len[0])))
		return LIST_UNKNOWN;
	n = t->len[1];
	if (n > 2 && (lower(tok(t, 1)[n - 1]) == 'm'))			/* "03:00PM" */
		n -= 2;
	if (!is_time(tok(t, 1), n))
		return LIST_UNKNOWN;
	if (t->len[2] == 2 && (lower(tok(t, 2)[1]) == 'm'))		/* "03:00 PM" */
		k = 2;
	if (k + 2 >= t->n)
		return LIST_UNKNOWN;
	copy_toks(e->date, sizeof(e->date), t, 0, k);
	e->flags[0] = 0;
	if (t->len[k + 1] == 5 && !memcmp(tok(t, k + 1), "<DIR>", 5)) {
		e->is_dir = 1;
		strcpy(e->size, "<DIR>");
	} else if (digits(tok(t, k + 1), t->len[k + 1])) {
		e->is_dir = 0;
		copy_tok(e->size, sizeof(e->size), t, k + 1);
	} else
		return LIST_UNKNOWN;
	set_name(e, t, k + 2, 0);
	return LIST_ENTRY;
}

/* +i8388621.48594,m825718503,r,s280,<TAB>name */
static int parse_eplf(const char *line, ListEntry *e)
{
	const char *p = line + 1, *tab = strchr(line, '\t');
	int n;

	if (line[0] != '+' || !tab)
		return LIST_UNKNOWN;
	e->is_dir = 0;
	strcpy(e->size, "0");
	e->flags[0] = e->date[0] = 0;
	while (p < tab) {
		const char *end = p;
		while (end < tab && *end != ',')
			end++;
		if (*p == '/')
			e->is_dir = 1;
		else if (*p == 's' && end - p > 1 && end - p < (int) sizeof(e->size)) {
			memcpy(e->size, p + 1, end - p - 1);
			e->size[end - p - 1] = 0;
		}
		p = end + 1;
	}
	e->name = tab + 1;
	n = strlen(e->name);
	while (n > 0 && is_blank(e->name[n - 1]))
		n--;
	e->name_len = n;
	return LIST_ENTRY;
}

int ftp_parse_list_line(const char *line, ListEntry *e)
{
	Tokens t;
	int r;

	tokenize(line, &t);
	if (t.n == 0 || (t.n <= 2 && t.len[0] == 5 && !memcmp(tok(&t, 0), "total", 5)))
		return LIST_SKIP;
	if (line[0] == '+')
		r = parse_eplf(line, e);
	else if ((r = parse_unix(&t, e)) == LIST_UNKNOWN)
		r = parse_dos(&t, e);
	if (r != LIST_ENTRY)
		return r;
	if (e->name_len <= 0 || (e->name_len == 1 && e->name[0] == '.')
		|| (e->name_len == 2 && e->name[0] == '.' && e->name[1] == '.'))
		return LIST_SKIP;
	return LIST_ENTRY;
}
