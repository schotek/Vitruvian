/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Rootless XWayland (phase 4): legacy X11 applications as native BeOS
 * windows. wlr_xwayland owns the Xwayland process (lazy — spawned on the
 * first client) and the whole X window manager; our policy layer maps each
 * mapped wlr_xwayland_surface onto the same private-scene + internal-output
 * + BeOS-window pattern as rootless.c, with the placement rules carried
 * over from the X PoC (reference/vitruvian_rootless.c).
 *
 * Confirmed from wlroots 0.18.2 sources (plan question 5.3): the wlr XWM is
 * non-reparenting and never draws frame windows — the BeOS tab that
 * beshim_create_xwindow provides is the only decoration, no conflict.
 *
 * X coordinates == desktop coordinates 1:1 (beshim_screen_size is the X
 * screen size), so wlr_xwayland_surface_configure speaks desktop pixels.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <xcb/xproto.h>

#include <wlr/xwayland/xwayland.h>
#include <wlr/util/log.h>

#include "vitrine.h"
#include "winhost.h"

/* Placement clamps — keep in sync with rootless.c (same policy). */
#define X_CLAMP_MIN_X	4
#define X_CLAMP_MIN_Y	28	/* BeOS tab height */
#define X_CLAMP_CORNER	64

/* Live in either hosting mode (helper team since H4, or in-process)?
 * Same rule as vitrine_rootless_window_alive: guards must never test
 * `->window` alone — that drops helper-hosted windows. */
static bool
xwindow_alive(const struct vitrine_xwindow *xw)
{
	return xw->window != NULL || xw->hosted != NULL;
}

static struct vitrine_xwindow *
xwindow_by_id(struct vitrine_server *server, int win_id)
{
	struct vitrine_xwindow *xw;
	wl_list_for_each(xw, &server->xwindows, link) {
		if (xw->win_id == win_id)
			return xw;
	}
	return NULL;
}

static struct wlr_surface *
xwindow_scene_surface_at(struct vitrine_xwindow *xw, double lx, double ly,
	double *sx, double *sy)
{
	if (xw->scene == NULL)
		return NULL;
	struct wlr_scene_node *node = wlr_scene_node_at(&xw->scene->tree.node,
		lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(
		wlr_scene_buffer_from_node(node));
	return ss != NULL ? ss->surface : NULL;
}

int
vitrine_xwayland_top_or_win_id(struct vitrine_server *server)
{
	if (server->xwayland == NULL)
		return -1;
	struct vitrine_xwindow *xw;
	wl_list_for_each(xw, &server->xwindows, link) {
		if (xw->window != NULL && xw->or_window)
			return xw->win_id;
	}
	return -1;
}

/* Mirror "who is on top" from BeOS into X stacking (F7). X core input picks
 * the event window by walking the X stacking order, so a stack that never
 * follows what the user sees routes clicks into whatever window happens to
 * be above in X — reproduced with Steam's settings modal, which rendered
 * above the main window yet never saw a click (even via XTEST). Only the
 * top window is mirrored: it is the only one the pointer interacts with.
 *
 * Two silencers keep the original F7 regression (a ConfigureNotify landing
 * mid-grab pops down a just-opened spring-loaded Xt menu) impossible:
 * - dedupe: a FOCUS_IN for the window that is already X-top sends nothing;
 * - menu gate: while any menu is mapped (Wayland popup or override-redirect
 *   X window), the raise is deferred — STACK_MODE_ABOVE would lift the
 *   window above the open menu and steal its pointer picking. */
static void
xwindow_restack_top(struct vitrine_xwindow *xw)
{
	struct vitrine_server *server = xw->server;

	if (xw->or_window || xw->xsurface == NULL || !xwindow_alive(xw))
		return;
	if (server->x_top == xw && !xw->restack_pending)
		return;
	if (vitrine_popup_top_win_id(server) >= 0
			|| vitrine_xwayland_top_or_win_id(server) >= 0) {
		xw->restack_pending = true;
		return;
	}
	xw->restack_pending = false;
	wlr_xwayland_surface_restack(xw->xsurface, NULL, XCB_STACK_MODE_ABOVE);
	server->x_top = xw;
}

void
vitrine_xwayland_flush_pending_restack(struct vitrine_server *server)
{
	/* popup.c calls this unconditionally; xwindows is only a live list
	 * once xwayland_init ran (rootless with XWayland available). */
	if (server->xwayland == NULL)
		return;
	/* Newest-first: the first pending window is the top candidate; anyone
	 * older pending lost the race and no longer belongs on top. */
	struct vitrine_xwindow *xw;
	bool raised = false;
	wl_list_for_each(xw, &server->xwindows, link) {
		if (!xw->restack_pending)
			continue;
		if (!raised) {
			xwindow_restack_top(xw);
			/* still pending if another menu is up — keep it */
			raised = !xw->restack_pending;
		} else {
			xw->restack_pending = false;
		}
	}
}

struct wlr_surface *
vitrine_xwayland_surface_at_win(struct vitrine_server *server, int win_id,
	double x, double y, double *sx, double *sy)
{
	struct vitrine_xwindow *xw = xwindow_by_id(server, win_id);
	if (xw == NULL || !xwindow_alive(xw))
		return NULL;
	return xwindow_scene_surface_at(xw, x - xw->x, y - xw->y, sx, sy);
}

/* Desktop-coordinate hit-test across X windows, newest first (called from
 * vitrine_desktop_surface_at between popups and Wayland toplevels). */
struct wlr_surface *
vitrine_xwayland_desktop_surface_at(struct vitrine_server *server,
	double x, double y, double *sx, double *sy)
{
	if (server->xwayland == NULL)
		return NULL;
	struct vitrine_xwindow *xw;
	wl_list_for_each(xw, &server->xwindows, link) {
		if (!xwindow_alive(xw))
			continue;
		double lx = x - xw->x;
		double ly = y - xw->y;
		if (lx < 0 || ly < 0 || lx >= xw->width || ly >= xw->height)
			continue;
		struct wlr_surface *surface = xwindow_scene_surface_at(xw,
			lx, ly, sx, sy);
		if (surface != NULL)
			return surface;
	}
	return NULL;
}

/* ---- teardown (same deferred-idle discipline as rootless.c/popup.c) ---- */

static void
xwindow_teardown_idle(void *data)
{
	struct vitrine_xwindow *xw = data;

	if (xw->output != NULL) {
		wlr_output_destroy(&xw->output->base);
		xw->output = NULL;
	}
	if (xw->scene != NULL) {
		wlr_scene_node_destroy(&xw->scene->tree.node);
		xw->scene = NULL;
		xw->surface_tree = NULL;
	}

	if (xw->xsurface == NULL) {
		/* X window destroyed — the bookkeeping goes too. */
		wl_list_remove(&xw->link);
		free(xw);
	} else {
		/* Just unmapped (withdrawn); a later re-map recreates the
		 * window through xwindow_map. */
		xw->teardown_scheduled = false;
	}
}

static void
xwindow_schedule_teardown(struct vitrine_xwindow *xw)
{
	if (xw->teardown_scheduled)
		return;
	wlr_log(WLR_INFO, "x window %d torn down", xw->win_id);
	xw->teardown_scheduled = true;
	xw->window = NULL; /* stale win_id events now drop by contract */
	xw->hosted = NULL; /* owned by the output, destroyed with it */
	xw->restack_pending = false;
	if (xw->server->x_top == xw)
		xw->server->x_top = NULL;
	wl_event_loop_add_idle(xw->server->event_loop, xwindow_teardown_idle,
		xw);
	/* window == NULL above already hid this window from the menu gate, so
	 * a raise deferred behind a dying X menu can run right now. */
	if (xw->or_window)
		vitrine_xwayland_flush_pending_restack(xw->server);
}

/* ---- placement (PoC rules: ICCCM position honoring + center/cascade) ---- */

static bool
xwindow_wants_position(struct wlr_xwayland_surface *xs)
{
	xcb_size_hints_t *hints = xs->size_hints;
	if (hints == NULL)
		return false;
	if (hints->flags & XCB_ICCCM_SIZE_HINT_US_POSITION)
		return true;
	/* PPosition, but not the meaningless Xt (0,0) default. */
	if ((hints->flags & XCB_ICCCM_SIZE_HINT_P_POSITION)
			&& (xs->x != 0 || xs->y != 0))
		return true;
	return false;
}

static void
xwindow_place(struct vitrine_xwindow *xw, int *x, int *y)
{
	struct wlr_xwayland_surface *xs = xw->xsurface;

	if (xs->override_redirect) {
		/* Menus/tooltips position themselves — verbatim. */
		*x = xs->x;
		*y = xs->y;
		return;
	}

	if (xwindow_wants_position(xs)) {
		*x = xs->x;
		*y = xs->y;
	} else {
		vitrine_place_window(xw->server, xw->width, xw->height, x, y);
		return; /* already clamped */
	}

	/* Honored positions still keep the tab reachable and a grabbable
	 * corner on screen. */
	int screen_w = 0, screen_h = 0;
	if (beshim_screen_size(xw->server->shim, &screen_w, &screen_h) != 0
			|| screen_w <= 0 || screen_h <= 0) {
		screen_w = 1024;
		screen_h = 768;
	}
	if (*x > screen_w - X_CLAMP_CORNER)
		*x = screen_w - X_CLAMP_CORNER;
	if (*y > screen_h - X_CLAMP_CORNER)
		*y = screen_h - X_CLAMP_CORNER;
	if (*x < X_CLAMP_MIN_X)
		*x = X_CLAMP_MIN_X;
	if (*y < X_CLAMP_MIN_Y)
		*y = X_CLAMP_MIN_Y;
}

static void
xwindow_apply_size_limits(struct vitrine_xwindow *xw)
{
	xcb_size_hints_t *hints = xw->xsurface->size_hints;
	if (!xwindow_alive(xw) || hints == NULL)
		return;
	int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
	if (hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
		min_w = hints->min_width;
		min_h = hints->min_height;
	}
	if (hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
		max_w = hints->max_width;
		max_h = hints->max_height;
	}
	if (xw->hosted != NULL)
		winhost_set_limits(xw->hosted, min_w, min_h, max_w, max_h);
	else
		beshim_set_size_limits(xw->window, min_w, min_h, max_w, max_h);
}

/* Hosting-mode split for a compositor-driven move/resize (XWM configures,
 * client requests). The hosted resize is the H2 area-swap handshake: only
 * a successfully sent swap may resize the wlr output — otherwise the
 * shared framebuffer keeps yesterday's size. */
static void
xwindow_apply_geometry(struct vitrine_xwindow *xw, int x, int y, int w, int h)
{
	if (x != xw->x || y != xw->y) {
		xw->x = x;
		xw->y = y;
		if (xw->hosted != NULL)
			winhost_move_window(xw->hosted, x, y);
		else if (xw->window != NULL)
			beshim_move_window(xw->window, x, y);
	}
	if (w != xw->width || h != xw->height) {
		if (xw->hosted != NULL) {
			if (winhost_resize_window(xw->hosted, w, h) != 0)
				return;
		} else if (xw->window != NULL) {
			beshim_resize_window(xw->window, w, h);
		} else {
			return;
		}
		xw->width = w;
		xw->height = h;
		vitrine_output_resize(xw->output, w, h);
	}
}

/* ---- surface lifecycle ---- */

static void
xwindow_map(struct vitrine_xwindow *xw)
{
	struct vitrine_server *server = xw->server;
	struct wlr_xwayland_surface *xs = xw->xsurface;

	if (xwindow_alive(xw) || xw->teardown_scheduled) {
		wlr_log(WLR_ERROR, "xwindow map in unexpected state (window %p"
			" hosted %p teardown %d)", (void *)xw->window,
			(void *)xw->hosted, xw->teardown_scheduled);
		return;
	}

	int w = xs->width;
	int h = xs->height;
	if (w <= 0 || h <= 0) {
		w = xs->surface->current.width;
		h = xs->surface->current.height;
	}
	if (w <= 0 || h <= 0)
		return;

	xw->width = w;
	xw->height = h;
	xw->or_window = xs->override_redirect;
	xwindow_place(xw, &xw->x, &xw->y);

	xw->win_id = ++server->next_win_id;
	BeWindowSpec spec = {
		.x = xw->x,
		.y = xw->y,
		.w = w,
		.h = h,
		.title = xs->title,
		.override_redirect = xs->override_redirect ? 1 : 0,
		.resizable = 1,
		.win_id = xw->win_id,
		.borderless = 0,	/* policy: every X toplevel gets the tab */
	};
	/* Winhost gate (H4): regular X toplevels get a helper team keyed by
	 * WM_CLASS — the X analogue of the Wayland app_id, and what the
	 * .desktop StartupWMClass= key matches. Override-redirect windows
	 * (menus, tooltips) always stay in-process: they have no Deskbar
	 * presence and their grab semantics stop at the process boundary.
	 * Helper failure degrades to the in-process path (crash-storm
	 * fallback included), never to a lost window. */
	if (!xs->override_redirect && winhost_enabled(server)) {
		xw->hosted = winhost_create_window(server, &spec, xs->class);
		if (xw->hosted != NULL) {
			xw->output = vitrine_output_create_from_hosted(server,
				xw->hosted, w, h);
			if (xw->output == NULL) {
				winhost_destroy_window(xw->hosted);
				xw->hosted = NULL;
				return;
			}
		}
	}
	if (xw->hosted == NULL) {
		xw->window = beshim_create_xwindow(server->shim, &spec);
		if (xw->window == NULL)
			return;

		xw->output = vitrine_output_create_from_window(server,
			xw->window, w, h);
		if (xw->output == NULL) {
			beshim_destroy_window(xw->window);
			xw->window = NULL;
			return;
		}
	}

	xw->scene = wlr_scene_create();
	/* Matte under ARGB content (shaped xeyes renders as a rectangle over
	 * panel grey — shaping is out of scope, finding #8/#9). */
	wlr_scene_rect_create(&xw->scene->tree, w, h,
		(float[4]){ 0.85f, 0.85f, 0.85f, 1.0f });
	/* X windows have no xdg geometry: the buffer IS the window. */
	xw->surface_tree = wlr_scene_subsurface_tree_create(&xw->scene->tree,
		xs->surface);
	xw->output->scene_output = wlr_scene_output_create(xw->scene,
		&xw->output->base);

	xwindow_apply_size_limits(xw);

	/* Tell the X side where the window ended up — initial menus and
	 * dialogs position against these root coordinates. NEVER for
	 * override-redirect windows: they placed themselves (we honored it
	 * verbatim), and an unexpected ConfigureNotify on a freshly mapped
	 * spring-loaded Xt menu pops it down mid-grab. */
	if (!xs->override_redirect)
		wlr_xwayland_surface_configure(xs, xw->x, xw->y, w, h);

	if (!xs->override_redirect) {
		wlr_xwayland_surface_activate(xs, true);
		vitrine_focus_surface(server, xs->surface);
		/* A freshly shown BWindow is frontmost on the BeOS side;
		 * mirror that into X right away so the first hover/click
		 * already picks this window (F7). */
		xwindow_restack_top(xw);
	}
	/* Override-redirect windows: hands OFF both X input focus AND wl
	 * keyboard focus. Either shift reaches the grabbing client as an X
	 * FocusOut and spring-loaded Xt menus pop down on it instantly
	 * (observed live twice: activate → died in 1 s; keyboard-focus-only
	 * → died in 5 ms, before any pointer event). Keys still reach an
	 * open menu through its own X keyboard grab. Trade-off: rofi-style
	 * OR keyboard consumers (wants_focus heuristic) are NOT supported in
	 * v1 — none are in the target app matrix. */

	wlr_log(WLR_INFO, "x window %d: %dx%d at %d,%d (%s%s)", xw->win_id,
		w, h, xw->x, xw->y,
		xs->override_redirect ? "override-redirect" : "toplevel",
		xs->title != NULL ? "" : ", untitled");
}

static void
xwindow_handle_surface_map(struct wl_listener *listener, void *data)
{
	struct vitrine_xwindow *xw =
		wl_container_of(listener, xw, surface_map);
	xwindow_map(xw);
}

static void
xwindow_handle_surface_unmap(struct wl_listener *listener, void *data)
{
	struct vitrine_xwindow *xw =
		wl_container_of(listener, xw, surface_unmap);
	xwindow_schedule_teardown(xw);
}

static void
xwindow_handle_associate(struct wl_listener *listener, void *data)
{
	struct vitrine_xwindow *xw = wl_container_of(listener, xw, associate);
	struct wlr_xwayland_surface *xs = xw->xsurface;

	xw->surface_map.notify = xwindow_handle_surface_map;
	wl_signal_add(&xs->surface->events.map, &xw->surface_map);
	xw->surface_unmap.notify = xwindow_handle_surface_unmap;
	wl_signal_add(&xs->surface->events.unmap, &xw->surface_unmap);
}

static void
xwindow_handle_dissociate(struct wl_listener *listener, void *data)
{
	struct vitrine_xwindow *xw = wl_container_of(listener, xw, dissociate);

	wl_list_remove(&xw->surface_map.link);
	wl_list_remove(&xw->surface_unmap.link);
}

/* X clients ask for size/position themselves (initial sizing, xterm's
 * font-menu resize) — apply and answer, or the client hangs waiting for
 * the ConfigureNotify (finding #7). */
static void
xwindow_handle_request_configure(struct wl_listener *listener, void *data)
{
	struct vitrine_xwindow *xw =
		wl_container_of(listener, xw, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;

	int x = ev->x;
	int y = ev->y;
	int w = ev->width;
	int h = ev->height;

	if (xwindow_alive(xw)) {
		if (!xw->or_window && y < X_CLAMP_MIN_Y)
			y = X_CLAMP_MIN_Y;
		xwindow_apply_geometry(xw, x, y, w, h);
	}

	wlr_xwayland_surface_configure(xw->xsurface, x, y, w, h);
}

static void
xwindow_handle_request_activate(struct wl_listener *listener, void *data)
{
	struct vitrine_xwindow *xw =
		wl_container_of(listener, xw, request_activate);

	if (!xwindow_alive(xw) || xw->xsurface->surface == NULL)
		return;
	if (xw->hosted != NULL)
		winhost_activate(xw->hosted);
	else
		beshim_activate(xw->window);
	wlr_xwayland_surface_activate(xw->xsurface, true);
	vitrine_focus_surface(xw->server, xw->xsurface->surface);
}

/* The XWM applied a configure (ours or the client's): follow on the BeOS
 * side. beshim move/resize suppress the echo events by contract. */
static void
xwindow_handle_set_geometry(struct wl_listener *listener, void *data)
{
	struct vitrine_xwindow *xw =
		wl_container_of(listener, xw, set_geometry);
	struct wlr_xwayland_surface *xs = xw->xsurface;

	if (!xwindow_alive(xw))
		return;

	xwindow_apply_geometry(xw, xs->x, xs->y, xs->width, xs->height);
}

static void
xwindow_handle_set_title(struct wl_listener *listener, void *data)
{
	struct vitrine_xwindow *xw = wl_container_of(listener, xw, set_title);

	if (xw->hosted != NULL)
		winhost_set_title(xw->hosted, xw->xsurface->title);
	else if (xw->window != NULL)
		beshim_set_title(xw->window, xw->xsurface->title);
}

static void
xwindow_handle_destroy(struct wl_listener *listener, void *data)
{
	struct vitrine_xwindow *xw = wl_container_of(listener, xw, destroy);

	xwindow_schedule_teardown(xw);
	xw->xsurface = NULL;
	wl_list_remove(&xw->destroy.link);
	wl_list_remove(&xw->request_configure.link);
	wl_list_remove(&xw->request_activate.link);
	wl_list_remove(&xw->associate.link);
	wl_list_remove(&xw->dissociate.link);
	wl_list_remove(&xw->set_title.link);
	wl_list_remove(&xw->set_geometry.link);
}

static void
handle_new_surface(struct wl_listener *listener, void *data)
{
	struct vitrine_server *server =
		wl_container_of(listener, server, xw_new_surface);
	struct wlr_xwayland_surface *xs = data;

	struct vitrine_xwindow *xw = calloc(1, sizeof(*xw));
	if (xw == NULL)
		return;
	xw->server = server;
	xw->xsurface = xs;
	xw->win_id = -1;

	/* Newest first == topmost by construction (BeOS raises new windows). */
	wl_list_insert(&server->xwindows, &xw->link);

	xw->destroy.notify = xwindow_handle_destroy;
	wl_signal_add(&xs->events.destroy, &xw->destroy);
	xw->request_configure.notify = xwindow_handle_request_configure;
	wl_signal_add(&xs->events.request_configure, &xw->request_configure);
	xw->request_activate.notify = xwindow_handle_request_activate;
	wl_signal_add(&xs->events.request_activate, &xw->request_activate);
	xw->associate.notify = xwindow_handle_associate;
	wl_signal_add(&xs->events.associate, &xw->associate);
	xw->dissociate.notify = xwindow_handle_dissociate;
	wl_signal_add(&xs->events.dissociate, &xw->dissociate);
	xw->set_title.notify = xwindow_handle_set_title;
	wl_signal_add(&xs->events.set_title, &xw->set_title);
	xw->set_geometry.notify = xwindow_handle_set_geometry;
	wl_signal_add(&xs->events.set_geometry, &xw->set_geometry);
}

/* ---- BeOS window events (routed here when the win_id is not a rootless
 * Wayland window's — input.c) ---- */

void
vitrine_xwayland_handle_window_event(struct vitrine_server *server,
	const BeInputEvent *ev)
{
	struct vitrine_xwindow *xw = xwindow_by_id(server, ev->screen);
	if (xw == NULL || xw->xsurface == NULL || !xwindow_alive(xw))
		return; /* stale win_id after teardown — drop by contract */

	struct wlr_xwayland_surface *xs = xw->xsurface;

	switch (ev->code) {
	case BE_WINDOW_CLOSE:
		/* WM_DELETE_WINDOW (or a kill for clients without it) — the
		 * wlr XWM picks the right one. */
		wlr_xwayland_surface_close(xs);
		break;

	case BE_WINDOW_MOVED:
		/* X root coordinates are desktop coordinates 1:1; unlike
		 * Wayland clients, X clients DO know their position, so a
		 * user drag must be forwarded (xterm places menus by it). */
		xw->x = ev->x;
		xw->y = ev->y;
		winhost_note_move(xw->hosted, ev->x, ev->y);
		wlr_xwayland_surface_configure(xs, ev->x, ev->y,
			xw->width, xw->height);
		break;

	case BE_WINDOW_RESIZED:
		/* No configure/ack handshake in X: request the size, the XWM
		 * applies it and set_geometry follows up on the BeOS side. */
		wlr_xwayland_surface_configure(xs, xw->x, xw->y, ev->x, ev->y);
		break;

	case BE_WINDOW_FOCUS_IN:
		wlr_xwayland_surface_activate(xs, true);
		if (xs->surface != NULL)
			vitrine_focus_surface(server, xs->surface);
		/* The quieter F7 mechanism the earlier comment asked for:
		 * xwindow_restack_top() dedupes (an already-top window sends
		 * no ConfigureNotify at all — the reproduced Xt-menu killer
		 * was exactly a redundant FOCUS_IN restack) and defers while
		 * any menu is mapped. */
		xwindow_restack_top(xw);
		break;

	case BE_WINDOW_FOCUS_OUT:
		wlr_xwayland_surface_activate(xs, false);
		break;

	default:
		break;
	}
}

/* ---- server lifecycle ---- */

/* Diagnostic tap (VITRINE_DEBUG): log every X event the XWM receives.
 * Returning 0 keeps the default wlr handler in charge. */
static int
xwm_event_tap(struct wlr_xwm *xwm, xcb_generic_event_t *event)
{
	wlr_log(WLR_DEBUG, "xwm event: type=%u", event->response_type & 0x7f);
	return 0;
}

/* Xwayland runs with no -auth file: its fresh access list trusts only the
 * session uid, and the lazy server (-terminate 10) resets that list on
 * every restart — a one-shot xhost grant from the session scripts
 * evaporates with the first terminate. Re-grant root on every ready so
 * sudo-run X clients (sudo xterm, ...) can connect; DISPLAY reaches them
 * via sudoers env_keep (sudoers.d/vos-gui-env). Equivalent to
 * `xhost +si:localuser:root`. Not a privilege grant: uid 0 already owns
 * the machine. */
static void
grant_root_x_access(struct vitrine_server *server)
{
	xcb_connection_t *c = wlr_xwayland_get_xwm_connection(server->xwayland);
	if (c == NULL)
		return;
	static const char kRoot[] = "localuser\0root"; /* SI addr: type '\0' value */
	xcb_change_hosts(c, XCB_HOST_MODE_INSERT, XCB_FAMILY_SERVER_INTERPRETED,
		sizeof(kRoot) - 1, (const uint8_t *)kRoot);
	xcb_flush(c);
}

static void
handle_ready(struct wl_listener *listener, void *data)
{
	struct vitrine_server *server =
		wl_container_of(listener, server, xw_ready);

	grant_root_x_access(server);
	wlr_log(WLR_INFO, "Xwayland ready on DISPLAY=%s",
		server->xwayland->display_name);
}

/* Deterministic DISPLAY=:0 across crash-restarts (finding #17a): when a
 * previous instance died hard, /tmp/.X0-lock survives (the socket itself
 * is abstract on Linux and dies with its owner) and Xwayland would
 * silently come up as :1 — breaking the static session exports. The lock
 * holds the owner's PID; remove the leftovers only when that process is
 * gone, the same liveness rule the X server itself applies. (wayland-0
 * needs no sweep: libwayland arbitrates it via flock on wayland-0.lock,
 * which the kernel drops on crash.) */
static void
sweep_stale_x0(void)
{
	const char *lock_path = "/tmp/.X0-lock";
	const char *sock_path = "/tmp/.X11-unix/X0";

	FILE *f = fopen(lock_path, "r");
	if (f == NULL)
		return; /* no lock — :0 is free */

	long pid = -1;
	if (fscanf(f, "%10ld", &pid) != 1)
		pid = -1; /* unparsable == garbage == stale */
	fclose(f);

	if (pid > 0 && (kill((pid_t)pid, 0) == 0 || errno != ESRCH))
		return; /* the owner is alive — hands off, :1 it is */

	wlr_log(WLR_INFO, "sweeping stale X :0 lock (dead pid %ld)", pid);
	unlink(lock_path);
	unlink(sock_path);
}

void
vitrine_xwayland_init(struct vitrine_server *server)
{
	wl_list_init(&server->xwindows);

	sweep_stale_x0();

	server->xwayland = wlr_xwayland_create(server->display,
		server->compositor, true /* lazy: spawn on first client */);
	if (server->xwayland == NULL) {
		wlr_log(WLR_ERROR,
			"wlr_xwayland_create failed — X11 apps unavailable");
		return;
	}

	wlr_xwayland_set_seat(server->xwayland, server->seat);
	setenv("DISPLAY", server->xwayland->display_name, 1);
	if (getenv("VITRINE_DEBUG") != NULL)
		server->xwayland->user_event_handler = xwm_event_tap;

	server->xw_ready.notify = handle_ready;
	wl_signal_add(&server->xwayland->events.ready, &server->xw_ready);
	server->xw_new_surface.notify = handle_new_surface;
	wl_signal_add(&server->xwayland->events.new_surface,
		&server->xw_new_surface);

	wlr_log(WLR_INFO, "xwayland: lazy server on DISPLAY=%s",
		server->xwayland->display_name);
}

void
vitrine_xwayland_destroy(struct vitrine_server *server)
{
	if (server->xwayland == NULL)
		return;
	wlr_xwayland_destroy(server->xwayland);
	server->xwayland = NULL;
}
