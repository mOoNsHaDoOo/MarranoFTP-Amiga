/*
 * GUI.H - what the MarranoFTP core needs from a user interface.
 *
 * Every GUI (gui_mui.c, gui_gt.c) implements all of these. The core never
 * touches a gadget or an object: it only calls these functions, and the GUI
 * calls the core_* functions of core.h when the user does something.
 *
 * Strings and ViewColumn pointers passed to the GUI stay valid until the next
 * call that replaces the same list.
 */

#ifndef MFTP_GUI_H
#define MFTP_GUI_H

#include "core.h"

enum { GUI_LOCAL = 0, GUI_REMOTE = 1 };

/* Life cycle: gui_open() after core_init(), gui_close() before core_cleanup().
   gui_open() returns FALSE if the interface cannot be created. */
bool gui_open(void);
void gui_close(void);

/* Event loop support.
   gui_signals(): the signal mask the core must wait on (besides sockets).
   gui_handle(sigs): process the pending GUI input (the received signals are
   passed in), calling the core_* actions. Returns FALSE when the GUI wants to
   quit immediately (e.g. close gadget). */
ULONG gui_signals(void);
bool gui_handle(ULONG sigs);

/* Status window: one line of text (FTP commands, answers, messages) */
void gui_status(const char *text);

/* Transfer progress line */
void gui_transfer_info(const char *text);

/* Connected / disconnected (Connect/Disconnect button, ...) */
void gui_set_connected(bool connected);

/* Path fields */
void gui_set_path(int which, const char *path);

/* File lists: entries[0..count-1], to be shown in this order */
void gui_set_list(int which, ViewColumn **entries, int count);

/* Selected entries of a file list (indices into the last gui_set_list() array).
   Returns how many indices were stored in idx[] (at most max). */
int gui_get_selection(int which, int *idx, int max);

/* Transfer queue changed: show core_queue_item(core_queue_first() ..
   core_queue_used()-1) */
void gui_queue_changed(void);

/* A host list (address book) changed outside the address book window
   (e.g. configuration loaded) */
void gui_hosts_changed(void);

#endif
