/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * bewindow_win.h — window-side API of the Vitrine BeOS shim (C++ only).
 * The seam between the compositor-side shim (bewindow.cpp: BApplication,
 * clipboard, lifecycle) and the window objects: a BeEventSink is all a
 * window needs to report input back, so the same window code can run
 * inside the compositor today and inside a per-window helper process later
 * (per-window-helper plan, phase H1+).
 */
#ifndef VITRINE_BEWINDOW_WIN_H
#define VITRINE_BEWINDOW_WIN_H

#include "bewindow.h"

#include <OS.h>

/* Everything a window may touch of its owner: the self-pipe write end plus
 * the drop counter (window threads only) and the shutdown flag windows
 * consult in QuitRequested(). Embedded in BeShim; a helper process owns a
 * standalone one wired to its socket. */
typedef struct BeEventSink {
	int     pipe_w;          /* O_NONBLOCK write end; -1 = disabled */
	int64   droppedEvents;   /* window-thread only counter          */
	int32   shuttingDown;    /* accessed via atomic_*()             */
} BeEventSink;

/* One BeInputEvent record down the sink, non-blocking (drops + counts on a
 * full pipe — see bewindow.cpp SELF-PIPE OVERFLOW). */
void bewin_push_event(BeEventSink* sink, const BeInputEvent& ev);

/* Window factories + operations; BeWindow stays opaque (bewindow.h). The
 * bodies moved verbatim from the beshim_* implementations — bewindow.cpp
 * keeps beshim_* as thin wrappers over these. */
BeWindow* bewin_create_screen(BeEventSink* sink, int w, int h,
	const char* title);
BeWindow* bewin_create_x(BeEventSink* sink, const BeWindowSpec* spec);
void  bewin_set_title(BeWindow* win, const char* title);
void  bewin_move_window(BeWindow* win, int x, int y);
void* bewin_window_bits(BeWindow* win, int* bytes_per_row);
void  bewin_blit(BeWindow* win, int x, int y, int w, int h);
void  bewin_resize_window(BeWindow* win, int w, int h);
void  bewin_destroy_window(BeWindow* win);
void  bewin_set_size_limits(BeWindow* win, int min_w, int min_h,
	int max_w, int max_h);
void  bewin_activate(BeWindow* win);
void  bewin_send_behind(BeWindow* win, BeWindow* behind_of);

#endif	/* VITRINE_BEWINDOW_WIN_H */
