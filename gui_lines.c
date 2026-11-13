/*
 * GUI_LINES.C - text lines shared by the OS 1.3 user interfaces.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <string.h>
#include <proto/exec.h>

#include <caf/caf_types.h>
#include <caf/thread_helpers.h>

#include "core.h"
#include "gui_lines.h"

void ll_init(LineList *ll)
{
	ll->list.lh_Head = (struct Node *) &ll->list.lh_Tail;
	ll->list.lh_Tail = NULL;
	ll->list.lh_TailPred = (struct Node *) &ll->list.lh_Head;
	ll->list.lh_Type = 0;
}

void ll_free(LineList *ll)
{
	if (ll->nodes)
		FreeMem(ll->nodes, ll->nodes_size);
	if (ll->text)
		FreeMem(ll->text, ll->text_size);
	ll->nodes = NULL;
	ll->text = NULL;
	ll->count = 0;
	ll_init(ll);
}

bool ll_alloc(LineList *ll, int count, int width)
{
	int i;

	ll_free(ll);
	if (count <= 0)
		return true;
	ll->nodes_size = count * sizeof(struct Node);
	ll->text_size = count * (width + 1);
	ll->nodes = (struct Node *) AllocMem(ll->nodes_size, MEMF_CLEAR);
	ll->text = (char *) AllocMem(ll->text_size, MEMF_CLEAR);
	if (!ll->nodes || !ll->text) {
		ll_free(ll);
		return false;
	}
	ll->count = count;
	ll->width = width;
	for (i = 0; i < count; i++) {
		ll->nodes[i].ln_Name = ll->text + i * (width + 1);
		AddTail(&ll->list, &ll->nodes[i]);
	}
	return true;
}

char *ll_line(LineList *ll, int i)
{
	return ll->text + i * (ll->width + 1);
}

void gl_put_field(char *dst, const char *src, int len, bool right)
{
	int n = src ? strlen(src) : 0, i;

	if (len <= 0)
		return;
	if (n > len)
		n = len;
	if (right) {
		for (i = 0; i < len - n; i++)
			*dst++ = ' ';
		memcpy(dst, src, n);
	} else {
		memcpy(dst, src, n);
		for (i = n; i < len; i++)
			dst[i] = ' ';
	}
}

bool gl_format_files(LineList *ll, ViewColumn **entries, int count, int cols)
{
	int i, name_w = cols - 1 - 1 - 9;

	if (count > MAX_VIEW_ENTRIES)
		count = MAX_VIEW_ENTRIES;
	if (name_w < 4)
		name_w = 4;
	if (!ll_alloc(ll, count, cols))
		return false;
	for (i = 0; i < count; i++) {
		char *line = ll_line(ll, i);
		ViewColumn *vc = entries[i];
		int e;

		line[0] = ' ';
		gl_put_field(line + 1, vc->name, name_w, false);
		line[1 + name_w] = ' ';
		gl_put_field(line + 2 + name_w, vc->is_dir ? "<DIR>" : vc->size, cols - 2 - name_w, true);
		line[cols] = 0;
		for (e = cols - 1; e > 0 && line[e] == ' '; e--)	/* no trailing blanks */
			line[e] = 0;
	}
	return true;
}

bool gl_format_queue(LineList *ll, int width)
{
	int first = core_queue_first(), n = core_queue_used() - first, i;

	if (n < 0)
		n = 0;
	if (!ll_alloc(ll, n, width))
		return false;
	for (i = 0; i < n; i++) {
		QueueItem *q = core_queue_item(first + i);
		char *line = ll_line(ll, i);
		char *cmd;

		switch (q->command) {
			case CMD_DOWNLOAD:			cmd = "GET"; break;
			case CMD_UPLOAD:			cmd = "PUT"; break;
			case CMD_REMOTE_DELETE:		cmd = "DEL"; break;
			case CMD_RENAME:			cmd = "REN"; break;
			case CMD_LOCAL_DIR_SCAN:	cmd = "LSCAN"; break;
			case CMD_REMOTE_DIR_SCAN:	cmd = "RSCAN"; break;
			case CMD_LOCAL_MKDIR:		cmd = "LMKDIR"; break;
			case CMD_REMOTE_MKDIR:		cmd = "RMKDIR"; break;
			case CMD_LOCAL_BASEPATH:	cmd = "LPATH"; break;
			case CMD_REMOTE_CHDIR:		cmd = "CWD"; break;
			default:					cmd = "?"; break;
		}
		gl_put_field(line, cmd, 7, false);
		caf_strncpy(line + 7, q->data ? q->data : "", width - 7);
	}
	return true;
}

bool gl_is_parent(LineList *ll, int i)
{
	char *l;

	if (i < 0 || i >= ll->count)
		return false;
	l = ll_line(ll, i);
	return (bool) (l[1] == '.' && l[2] == '.' && (l[3] == ' ' || l[3] == 0));
}
