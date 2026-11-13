/*
 * GUI_LINES.H - text lines shared by the OS 1.3 user interfaces
 * (gui_gt.c for MarranoFTP13GD, gui_int.c for MarranoFTPI).
 *
 * A LineList is an array of fixed width text lines in one allocation, also
 * linked as an exec List (ln_Name = the line) for the GadTools listviews.
 */
#ifndef MFTP_GUI_LINES_H
#define MFTP_GUI_LINES_H

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include "core.h"

typedef struct LineList {
	struct List list;
	struct Node *nodes;
	char *text;
	ULONG nodes_size, text_size;
	int count, width;
} LineList;

void ll_init(LineList *ll);
void ll_free(LineList *ll);
bool ll_alloc(LineList *ll, int count, int width);	/* count blank lines */
char *ll_line(LineList *ll, int i);

/* Copies src into dst padded with blanks to exactly len characters */
void gl_put_field(char *dst, const char *src, int len, bool right);

/* " name ... size" lines of cols characters, column 0 is kept for the mark */
bool gl_format_files(LineList *ll, ViewColumn **entries, int count, int cols);

/* "GET    name" lines for the pending items of the transfer queue */
bool gl_format_queue(LineList *ll, int width);

/* TRUE if line i is the ".." entry */
bool gl_is_parent(LineList *ll, int i);

#endif
