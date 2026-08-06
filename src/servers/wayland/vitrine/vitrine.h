/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Vitrine — nested Wayland compositor. Core types shared between the
 * custom wlroots backend (backend.c/output.c), the shell policy
 * (shell.c) and the wiring in main.c. The BeAPI lives strictly behind
 * the pure-C bewindow.h contract.
 */
#ifndef VITRINE_H
#define VITRINE_H

#include <stdbool.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "bewindow.h"

/* From <wlr/xwayland/xwayland.h> — kept out of this header so that only
 * xwayland.c needs the xcb includes. */
struct wlr_xwayland;
struct wlr_xwayland_surface;

struct vitrine_input {
	struct vitrine_server *server;

	struct wlr_keyboard keyboard;
	/* Our view of held keys (evdev codes, LSB-first per byte). Drives the
	 * autorepeat-duplicate filter, FOCUS_OUT release-all, and the
	 * BE_INPUT_KEY_STATES reconcile — release-only self-healing. */
	uint8_t pressed[256 / 8];
	struct wl_listener keyboard_key;
	struct wl_listener keyboard_modifiers;

	double pointer_x, pointer_y;  /* output-local (rootful) */
	uint32_t button_mask;         /* last seen BeOS button bitmask */
	/* Desktop origin of the surface the pointer entered last (desktop
	 * coords minus surface-local coords) — translates motion during the
	 * implicit grab, when no re-hit-testing may happen. */
	double focus_origin_x, focus_origin_y;

	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;

	int inotify_fd;               /* xkb_layout live-reload watch */
	struct wl_event_source *inotify_source;
};

/* clipboard.c — CLIPBOARD ⇄ BClipboard text bridge (F7). */
struct vitrine_clipboard {
	struct vitrine_server *server;
	struct wl_listener seat_set_selection;

	/* wl → BeOS: pending read from the selection owner's pipe */
	struct wl_event_source *reader;
	int reader_fd;
	char *reader_buf;
	size_t reader_len, reader_cap;

	/* BeOS → wl: our outstanding bridge source (NULL when none) */
	struct vitrine_clipboard_source *bridge_source;
};

struct vitrine_server {
	struct wl_display *display;
	struct wl_event_loop *event_loop;
	BeShim *shim;
	bool rootful;

	struct vitrine_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_scene *scene;
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_seat *seat;
	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;

	struct vitrine_output *rootful_output;  /* rootful mode only */
	struct wl_event_source *input_source;
	struct vitrine_input input;

	/* rootless mode */
	struct wlr_xdg_decoration_manager_v1 *decoration_manager;
	struct wl_listener new_decoration;
	struct wl_listener new_xdg_popup;
	struct wl_list rootless_windows;        /* vitrine_rootless_window.link */
	struct wl_list popups;                  /* vitrine_popup.link, newest first */
	int next_win_id;
	struct vitrine_output *virtual_output;  /* desktop-sized metadata global */

	struct vitrine_clipboard clipboard;

	/* XWayland (rootless only; NULL when unavailable) */
	struct wlr_xwayland *xwayland;
	struct wl_list xwindows;                /* vitrine_xwindow.link, newest first */
	struct wl_listener xw_ready;
	struct wl_listener xw_new_surface;
	struct vitrine_xwindow *x_top;          /* last window raised in X stacking
	                                         * (dedupe for the F7 restack) */
};

/* Rootless X11 window (phase 4): same private-scene + internal-output +
 * BeOS-window pattern as vitrine_rootless_window, driven by the wlr XWM's
 * surface lifecycle (associate/map/unmap/dissociate) instead of xdg-shell. */
struct vitrine_xwindow {
	struct vitrine_server *server;
	struct wlr_xwayland_surface *xsurface;  /* NULL once the X window died */
	struct wl_list link;                    /* server.xwindows */

	int win_id;                             /* -1 until first map */
	BeWindow *window;                       /* NULL while unmapped/torn down */
	struct vitrine_output *output;
	struct wlr_scene *scene;
	struct wlr_scene_tree *surface_tree;

	int x, y;                               /* desktop == X root coordinates */
	int width, height;
	bool or_window;                         /* override-redirect at map time */
	bool teardown_scheduled;
	bool restack_pending;                   /* X raise deferred while a menu
	                                         * (popup/OR window) is mapped */

	struct wl_listener destroy;
	struct wl_listener request_configure;
	struct wl_listener request_activate;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener set_title;
	struct wl_listener set_geometry;
	/* valid only between associate and dissociate */
	struct wl_listener surface_map;
	struct wl_listener surface_unmap;
};

/* Rootless popup (menu/tooltip): override-redirect BeOS window whose view
 * snoops desktop-wide pointer events — while any popup lives, input.c
 * consumes pointer events ONLY from the newest popup's snoop and routes
 * them by desktop-coordinate hit-testing (finding #10). */
struct vitrine_popup {
	struct vitrine_server *server;
	struct wlr_xdg_popup *popup;            /* NULL once client went away */
	struct wl_list link;

	int win_id;
	BeWindow *window;                       /* NULL after teardown starts */
	struct vitrine_output *output;
	struct wlr_scene *scene;
	struct wlr_scene_tree *surface_tree;

	int x, y;                               /* desktop position */
	int width, height;

	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;

	bool teardown_scheduled;
};

struct vitrine_backend {
	struct wlr_backend base;
	struct vitrine_server *server;
	struct wl_list outputs;  /* vitrine_output.link */
	bool started;
};

struct vitrine_output {
	struct wlr_output base;
	struct vitrine_server *server;
	struct wl_list link;

	BeWindow *window;
	struct wlr_scene_output *scene_output;

	/* Frame pacing: a wl_event_loop timer per output, armed on backend
	 * start and re-armed from its own handler. wlr_output_send_frame is
	 * NEVER called from the commit hook — that would re-enter
	 * wlr_scene_output_commit while a commit is on the stack and render
	 * at an uncapped rate under continuous damage. */
	struct wl_event_source *frame_timer;
	int refresh_ms;

	struct wl_listener frame;
};

struct vitrine_toplevel {
	struct vitrine_server *server;
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_scene_tree *scene_tree;      /* rootful only */
	struct wlr_xdg_toplevel_decoration_v1 *decoration; /* may be NULL (CSD) */
	struct vitrine_rootless_window *rootless; /* rootless only, NULL before map */
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener set_parent;
};

/* Rootless: one BeOS window per xdg_toplevel — private scene + internal
 * (non-advertised) wlr_output whose scanout is the window's BBitmap. */
struct vitrine_rootless_window {
	struct vitrine_server *server;
	struct vitrine_toplevel *toplevel;      /* NULL once client went away */
	struct wl_list link;                    /* server.rootless_windows */

	int win_id;                             /* BeInputEvent.screen token */
	BeWindow *window;                       /* NULL after teardown starts */
	struct vitrine_output *output;
	struct wlr_scene *scene;
	struct wlr_scene_tree *surface_tree;

	int x, y;                               /* desktop position bookkeeping:
	                                         * translates desktop-absolute
	                                         * pointer coords; updated from
	                                         * BE_WINDOW_MOVED */
	int width, height;                      /* current content size */

	/* compositor→client resize handshake: serial of the last un-acked
	 * configure; BE_WINDOW_RESIZED during the drag coalesces into
	 * pending_* until the ack lands. */
	uint32_t configure_serial;
	int pending_width, pending_height;

	bool teardown_scheduled;
};

/* backend.c */
struct vitrine_backend *vitrine_backend_create(struct vitrine_server *server);

/* output.c */
struct vitrine_output *vitrine_output_create(struct vitrine_server *server,
	int width, int height, const char *title);
struct vitrine_output *vitrine_output_create_from_window(
	struct vitrine_server *server, BeWindow *window, int width, int height);
struct vitrine_output *vitrine_output_create_virtual(
	struct vitrine_server *server, int width, int height);
void vitrine_output_resize(struct vitrine_output *output, int width,
	int height);

/* shell.c */
void vitrine_shell_init(struct vitrine_server *server);

/* input.c */
void vitrine_input_init(struct vitrine_server *server);
void vitrine_focus_surface(struct vitrine_server *server,
	struct wlr_surface *surface);

/* rootless.c */
void vitrine_rootless_init(struct vitrine_server *server);
/* Center + 8-slot/24 px cascade placement with full-fit clamp (shared with
 * the XWayland policy for windows that expressed no position preference). */
void vitrine_place_window(struct vitrine_server *server, int w, int h,
	int *x, int *y);
void vitrine_rootless_new_toplevel(struct vitrine_toplevel *toplevel);
void vitrine_rootless_toplevel_map(struct vitrine_toplevel *toplevel);
void vitrine_rootless_toplevel_commit(struct vitrine_toplevel *toplevel);
void vitrine_rootless_toplevel_unmap(struct vitrine_toplevel *toplevel);
void vitrine_rootless_apply_decoration(struct vitrine_toplevel *toplevel);
/* xdg_toplevel.set_parent: re-apply the transient-above-parent stacking. */
void vitrine_rootless_toplevel_set_parent(struct vitrine_toplevel *toplevel);

/* clipboard.c */
void vitrine_clipboard_init(struct vitrine_server *server);
/* BE_INPUT_CLIPBOARD arrived: re-read BClipboard and bridge it seat-ward. */
void vitrine_clipboard_handle_be_change(struct vitrine_server *server);

/* popup.c */
void vitrine_popup_init(struct vitrine_server *server);
/* Desktop-coordinate hit-test across popups (newest first) then toplevels;
 * used by input.c while a popup snoop owns the pointer stream. */
struct wlr_surface *vitrine_desktop_surface_at(struct vitrine_server *server,
	double x, double y, double *sx, double *sy);
/* win_id of the newest live popup, or -1 when none (input dedupe rule). */
int vitrine_popup_top_win_id(struct vitrine_server *server);
/* Returns false when the win_id belongs to no (live or torn-down) rootless
 * window — the caller then offers the event to the XWayland layer. */
bool vitrine_rootless_handle_window_event(struct vitrine_server *server,
	const BeInputEvent *ev);
struct vitrine_rootless_window *vitrine_rootless_window_by_id(
	struct vitrine_server *server, int win_id);

/* xwayland.c */
void vitrine_xwayland_init(struct vitrine_server *server);
void vitrine_xwayland_destroy(struct vitrine_server *server);
void vitrine_xwayland_handle_window_event(struct vitrine_server *server,
	const BeInputEvent *ev);
/* win_id of the newest live override-redirect X window, or -1 (extends the
 * popup snoop-consume rule in input.c to X menus). */
int vitrine_xwayland_top_or_win_id(struct vitrine_server *server);
/* Perform an X raise that was deferred while a menu was open (called from
 * both teardown paths that can close the menu: OR X windows and Wayland
 * popups). */
void vitrine_xwayland_flush_pending_restack(struct vitrine_server *server);
/* Hit-test within one X window's scene (win_id routing, input.c). */
struct wlr_surface *vitrine_xwayland_surface_at_win(
	struct vitrine_server *server, int win_id, double x, double y,
	double *sx, double *sy);
/* Desktop-coordinate hit-test across all X windows, newest first. */
struct wlr_surface *vitrine_xwayland_desktop_surface_at(
	struct vitrine_server *server, double x, double y, double *sx,
	double *sy);

#endif /* VITRINE_H */
