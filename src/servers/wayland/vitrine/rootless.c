/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Rootless mode: every xdg_toplevel gets its own BeOS window (private
 * wlr_scene + internal, non-advertised wlr_output whose scanout is the
 * window's BBitmap). BeOS decorations replace the WM: close-box sends
 * xdg_toplevel.close, the tab drags the window (position is compositor
 * bookkeeping only — Wayland clients never know it), user resizing runs
 * the configure/ack handshake. SSD vs CSD is negotiated through
 * xdg-decoration; CSD clients get a borderless-but-focusable window.
 *
 * Teardown ordering (the generalized UnrealizeWindow lesson from the X
 * PoC): the internal output is NEVER destroyed from within a listener
 * that may be running inside its own frame/commit dispatch — unmap and
 * destroy only schedule an idle-source, which then destroys output →
 * scene → the bookkeeping struct, in that order.
 */

#include <stdlib.h>

#include <wlr/types/wlr_server_decoration.h>
#include <wlr/util/log.h>

#include "vitrine.h"
#include "winhost.h"

/* Placement policy carried over from the PoC (vitruvian_rootless.c):
 * center the first window, subsequent ones cascade through 8 slots
 * stepping 24 px; clamp so the tab and a grabbable corner stay on
 * screen. */
#define CASCADE_SLOTS	8
#define CASCADE_STEP	24
#define CLAMP_MIN_X	4
#define CLAMP_MIN_Y	28	/* BeOS tab height */
#define CLAMP_CORNER	64

struct vitrine_rootless_window *
vitrine_rootless_window_by_id(struct vitrine_server *server, int win_id)
{
	struct vitrine_rootless_window *window;
	wl_list_for_each(window, &server->rootless_windows, link) {
		if (window->win_id == win_id)
			return window;
	}
	return NULL;
}

void
vitrine_place_window(struct vitrine_server *server, int w, int h, int *x,
	int *y)
{
	static unsigned slot = 0;

	int screen_w = 0, screen_h = 0;
	if (beshim_screen_size(server->shim, &screen_w, &screen_h) != 0
			|| screen_w <= 0 || screen_h <= 0) {
		screen_w = 1024;
		screen_h = 768;
	}

	int cascade = (int)(slot % CASCADE_SLOTS) * CASCADE_STEP;
	slot++;

	*x = (screen_w - w) / 2 + cascade;
	*y = (screen_h - h) / 2 + cascade;

	/* Full-fit clamp AFTER the cascade offset (the cascade must never
	 * push a window off screen); the MIN clamps win when the window is
	 * taller/wider than the screen, keeping the tab reachable. */
	if (*x + w > screen_w)
		*x = screen_w - w;
	if (*y + h > screen_h)
		*y = screen_h - h;
	if (*x < CLAMP_MIN_X)
		*x = CLAMP_MIN_X;
	if (*y < CLAMP_MIN_Y)
		*y = CLAMP_MIN_Y;
}

/* Deferred teardown (idle source): destroy output (which quits the BeOS
 * window via the backend impl), then the private scene, then the struct.
 * Safe by construction: idle callbacks never run inside an output's own
 * frame/commit dispatch. */
static void
teardown_idle(void *data)
{
	struct vitrine_rootless_window *window = data;

	if (window->output != NULL)
		wlr_output_destroy(&window->output->base);
	if (window->scene != NULL)
		wlr_scene_node_destroy(&window->scene->tree.node);
	wl_list_remove(&window->link);
	free(window);
}

static void
schedule_teardown(struct vitrine_rootless_window *window)
{
	if (window->teardown_scheduled)
		return;
	window->teardown_scheduled = true;
	window->window = NULL; /* input events with this win_id now drop */
	window->hosted = NULL; /* owned by the output, destroyed with it */
	if (window->toplevel != NULL) {
		window->toplevel->rootless = NULL;
		window->toplevel = NULL;
	}
	wl_event_loop_add_idle(window->server->event_loop, teardown_idle,
		window);
}

/* Stacking (finding #15). Wayland has no restack request: the only ordering
 * we owe clients is xdg_toplevel.set_parent — a transient (dialog, tool
 * window) must never disappear under the window it belongs to. BeOS keeps a
 * new window on top by itself, so the whole duty is re-asserting that
 * relation whenever the PARENT comes forward: send the parent behind each of
 * its children. Sibling/lower restacking is not supported in v1 — the BeOS
 * stack is the user's, and nothing in xdg-shell asks us to reorder it. */

static struct vitrine_rootless_window *
window_for_xdg_toplevel(struct wlr_xdg_toplevel *xdg_toplevel)
{
	if (xdg_toplevel == NULL)
		return NULL;
	struct vitrine_toplevel *toplevel = xdg_toplevel->base->data;
	if (toplevel == NULL)
		return NULL;
	return toplevel->rootless;
}

static struct vitrine_rootless_window *
parent_window_of(struct vitrine_rootless_window *window)
{
	if (window->toplevel == NULL)
		return NULL;
	return window_for_xdg_toplevel(window->toplevel->toplevel->parent);
}

/* Do the two windows share any screen area? Frames are content boxes; the
 * tab sits above the content, so grow each box upwards by the tab height —
 * a dialog tucked right under the parent's tab is covered by it. */
static bool
frames_overlap(const struct vitrine_rootless_window *a,
	const struct vitrine_rootless_window *b)
{
	int ay = a->y - CLAMP_MIN_Y;
	int by = b->y - CLAMP_MIN_Y;
	return a->x < b->x + b->width && b->x < a->x + a->width
		&& ay < by + b->height + CLAMP_MIN_Y
		&& by < ay + a->height + CLAMP_MIN_Y;
}

/* Put `window` behind every transient child it actually covers, depth-first
 * so a dialog's own dialog keeps its place too. The xdg protocol rejects
 * parent loops (wlr_xdg_toplevel_set_parent), but the depth bound makes
 * that guarantee local rather than assumed.
 *
 * The overlap guard is not an optimization. app_server's SendWindowBehind
 * hands activation to whatever ends up in front (verified in the VM: the
 * parent's FOCUS_IN is immediately followed by the child's), so an
 * unconditional restack would yank the keyboard to a transient the user
 * never clicked. Where the windows overlap that is the right outcome — the
 * transient is a dialog demanding attention. Where they don't, restacking
 * changes nothing visible, so we skip it and the click keeps its focus. */
static void
raise_children_above(struct vitrine_rootless_window *window, int depth)
{
	if (window->window == NULL || depth > 8)
		return;

	struct vitrine_rootless_window *child;
	wl_list_for_each(child, &window->server->rootless_windows, link) {
		if (child == window || child->window == NULL)
			continue;
		if (parent_window_of(child) != window)
			continue;
		if (!frames_overlap(window, child))
			continue;
		wlr_log(WLR_DEBUG, "restack: win %d behind its child %d",
			window->win_id, child->win_id);
		beshim_send_behind(window->window, child->window);
		raise_children_above(child, depth + 1);
	}
}

/* Apply a size to the BeOS window + internal output; the client side is
 * already at (or configured towards) this size. */
static void
apply_size(struct vitrine_rootless_window *window, int w, int h)
{
	if (window->hosted != NULL) {
		/* H1: the shared-area resize handshake (new area + swap ack)
		 * is phase H2; until then hosted windows keep their map size. */
		static bool warned;
		if (!warned) {
			wlr_log(WLR_INFO,
				"winhost: resize deferred to H2, keeping size");
			warned = true;
		}
		return;
	}
	if (w <= 0 || h <= 0 || window->window == NULL)
		return;
	if (w == window->width && h == window->height)
		return;
	window->width = w;
	window->height = h;
	beshim_resize_window(window->window, w, h);
	vitrine_output_resize(window->output, w, h);
}

/* Decoration policy (Vladislav, 2026-08-03): EVERY toplevel gets the BeOS
 * tab — maximum blending with the native system, no client headerbars.
 * Enforced by advertising BOTH negotiation protocols with server-side as
 * the answer: xdg-decoration (Qt, foot, SDL) and KDE server-decoration
 * (the one GTK3 actually implements — with default mode SERVER GTK3 drops
 * its CSD headerbar and shadow margins entirely). */

static void
update_surface_offset(struct vitrine_rootless_window *window)
{
	/* Map the xdg window geometry origin onto output (0,0): CSD shadow
	 * margins fall outside the output and are never blitted. */
	struct wlr_xdg_surface *base = window->toplevel->toplevel->base;
	struct wlr_box *geo = &base->current.geometry;
	wlr_log(WLR_DEBUG,
		"win %d offset: geo=%d,%d %dx%d surface=%dx%d output=%dx%d",
		window->win_id, geo->x, geo->y, geo->width, geo->height,
		base->surface->current.width, base->surface->current.height,
		window->width, window->height);
	/* wlr_scene_xdg_surface_create already anchors the node at the
	 * window-geometry origin (its documented contract) — offsetting by
	 * -geo here again double-shifted the content by exactly the shadow
	 * margin (the "cut-off GTK window" bug). Keep the tree at 0,0. */
	wlr_scene_node_set_position(&window->surface_tree->node, 0, 0);
}

/* Called from shell.c on surface map — the first moment the geometry is
 * known, so this is where the BeOS window is born. */
static void
rootless_map(struct vitrine_toplevel *toplevel)
{
	struct vitrine_server *server = toplevel->server;
	struct wlr_xdg_toplevel *xdg_toplevel = toplevel->toplevel;

	struct wlr_box geo = xdg_toplevel->base->current.geometry;
	if (geo.width <= 0 || geo.height <= 0) {
		geo.x = 0;
		geo.y = 0;
		geo.width = xdg_toplevel->base->surface->current.width;
		geo.height = xdg_toplevel->base->surface->current.height;
	}
	if (geo.width <= 0 || geo.height <= 0)
		return;

	/* WM duty: never map a window larger than the screen (the
	 * widget-factory asks for ~1000x800 and would hang off the bottom).
	 * Clamp, and tell the client so it relayouts to the real size; until
	 * its commit lands the surface shows top-left-cropped. */
	int screen_w = 0, screen_h = 0;
	if (beshim_screen_size(server->shim, &screen_w, &screen_h) != 0
			|| screen_w <= 0 || screen_h <= 0) {
		screen_w = 1024;
		screen_h = 768;
	}
	int max_w = screen_w - 2 * CLAMP_MIN_X;
	int max_h = screen_h - CLAMP_MIN_Y - CLAMP_MIN_X;
	/* The client's minimum wins over the screen clamp: configuring below
	 * it is futile (the client keeps committing its minimum — verified
	 * with gtk3-widget-factory, min width > 1280) and the mismatch makes
	 * the client crop its own layout. An over-screen window is dragged
	 * by its tab instead. */
	struct wlr_xdg_toplevel_state *limits = &xdg_toplevel->current;
	if (limits->min_width > 0 && max_w < limits->min_width)
		max_w = limits->min_width;
	if (limits->min_height > 0 && max_h < limits->min_height)
		max_h = limits->min_height;
	bool clamped = false;
	if (geo.width > max_w) {
		geo.width = max_w;
		clamped = true;
	}
	if (geo.height > max_h) {
		geo.height = max_h;
		clamped = true;
	}
	if (clamped) {
		wlr_log(WLR_INFO,
			"map clamp: %dx%d (client min %dx%d, screen %dx%d)",
			geo.width, geo.height, limits->min_width,
			limits->min_height, screen_w, screen_h);
		wlr_xdg_toplevel_set_size(xdg_toplevel, geo.width, geo.height);
	}

	struct vitrine_rootless_window *window = calloc(1, sizeof(*window));
	if (window == NULL)
		return;
	window->server = server;
	window->toplevel = toplevel;
	window->win_id = ++server->next_win_id;
	window->width = geo.width;
	window->height = geo.height;

	vitrine_place_window(server, geo.width, geo.height, &window->x,
		&window->y);

	BeWindowSpec spec = {
		.x = window->x,
		.y = window->y,
		.w = geo.width,
		.h = geo.height,
		.title = xdg_toplevel->title,
		.override_redirect = 0,
		.resizable = 1,
		.win_id = window->win_id,
		.borderless = 0,	/* policy: always the BeOS tab */
	};
	/* H1 gate: Wayland toplevels can be hosted in the helper process
	 * (own Deskbar team) instead of an in-process BeOS window. Exactly
	 * one of window/hosted ends up set; the beshim_* calls below all
	 * no-op on a NULL BeWindow, and apply_size() skips hosted windows
	 * (resize handshake is H2). */
	if (winhost_enabled(server)) {
		window->hosted = winhost_create_window(server, &spec);
		if (window->hosted == NULL) {
			free(window);
			return;
		}
		window->output = vitrine_output_create_from_hosted(server,
			window->hosted, geo.width, geo.height);
		if (window->output == NULL) {
			winhost_destroy_window(window->hosted);
			free(window);
			return;
		}
	} else {
		window->window = beshim_create_xwindow(server->shim, &spec);
		if (window->window == NULL) {
			free(window);
			return;
		}

		window->output = vitrine_output_create_from_window(server,
			window->window, geo.width, geo.height);
		if (window->output == NULL) {
			beshim_destroy_window(window->window);
			free(window);
			return;
		}
	}

	window->scene = wlr_scene_create();
	/* Matte under the client (finding #8): regions the client leaves
	 * translucent inside its geometry (GTK CSD corners) blend over the
	 * BeOS panel grey instead of black. */
	wlr_scene_rect_create(&window->scene->tree, geo.width, geo.height,
		(float[4]){ 0.85f, 0.85f, 0.85f, 1.0f });
	window->surface_tree = wlr_scene_xdg_surface_create(
		&window->scene->tree, xdg_toplevel->base);
	window->output->scene_output = wlr_scene_output_create(window->scene,
		&window->output->base);
	update_surface_offset(window);

	/* Client min/max → BeOS size limits (min == max would fight the
	 * decorator, but B_NOT_RESIZABLE is only chosen at creation; equal
	 * limits pin the size, which is the same thing in practice). */
	struct wlr_xdg_toplevel_state *state = &xdg_toplevel->current;
	beshim_set_size_limits(window->window, state->min_width,
		state->min_height, state->max_width, state->max_height);

	wl_list_insert(&server->rootless_windows, &window->link);
	toplevel->rootless = window;

	wlr_xdg_toplevel_set_activated(xdg_toplevel, true);
	vitrine_focus_surface(server, xdg_toplevel->base->surface);

	/* Born above its parent: BeOS already puts a new window on top, but
	 * say it explicitly — the parent may be raised between our create and
	 * the first user interaction. */
	struct vitrine_rootless_window *parent = parent_window_of(window);
	if (parent != NULL && parent->window != NULL)
		beshim_send_behind(parent->window, window->window);

	wlr_log(WLR_INFO, "rootless window %d: %dx%d at %d,%d (%s), parent %d",
		window->win_id, geo.width, geo.height, window->x, window->y,
		toplevel->decoration != NULL ? "SSD" : "SSD-forced",
		parent != NULL ? parent->win_id : -1);
}

/* Called from shell.c on every surface commit of a rootless toplevel. */
static void
rootless_commit(struct vitrine_toplevel *toplevel)
{
	struct vitrine_rootless_window *window = toplevel->rootless;
	if (window == NULL || window->window == NULL)
		return;

	struct wlr_xdg_surface *base = toplevel->toplevel->base;
	struct wlr_box geo = base->current.geometry;
	if (geo.width <= 0 || geo.height <= 0) {
		geo.width = base->surface->current.width;
		geo.height = base->surface->current.height;
	}

	if (window->configure_serial != 0
			&& base->current.configure_serial
				>= window->configure_serial) {
		/* Our resize configure is acked and committed. */
		window->configure_serial = 0;
		apply_size(window, geo.width, geo.height);
		if (window->pending_width > 0) {
			/* Coalesced user resize steps: chase the newest. */
			int w = window->pending_width;
			int h = window->pending_height;
			window->pending_width = 0;
			window->pending_height = 0;
			if (w != window->width || h != window->height)
				window->configure_serial =
					wlr_xdg_toplevel_set_size(
						toplevel->toplevel, w, h);
		}
	} else if (window->configure_serial == 0
			&& (geo.width != window->width
				|| geo.height != window->height)) {
		/* Client resized itself (GTK dialog, font change) — follow. */
		apply_size(window, geo.width, geo.height);
	}

	update_surface_offset(window);
}

static void
rootless_unmap(struct vitrine_toplevel *toplevel)
{
	if (toplevel->rootless != NULL)
		schedule_teardown(toplevel->rootless);
}

/* BeOS window events (routed here from input.c by win_id). Returns false
 * when the win_id is not ours at all — the XWayland layer gets it next. */
bool
vitrine_rootless_handle_window_event(struct vitrine_server *server,
	const BeInputEvent *ev)
{
	struct vitrine_rootless_window *window =
		vitrine_rootless_window_by_id(server, ev->screen);
	wlr_log(WLR_DEBUG, "window event code=%d win=%d -> %s", ev->code,
		ev->screen, window == NULL ? "UNKNOWN"
			: (window->toplevel == NULL ? "TORN-DOWN" : "ok"));
	if (window == NULL)
		return false; /* not a rootless Wayland window's id */
	if (window->toplevel == NULL)
		return true; /* stale win_id after teardown — drop by contract */

	struct wlr_xdg_toplevel *toplevel = window->toplevel->toplevel;

	switch (ev->code) {
	case BE_WINDOW_CLOSE:
		/* Close-box → polite request; the client confirms by
		 * destroying the toplevel (or ignores it — its right). */
		wlr_xdg_toplevel_send_close(toplevel);
		break;

	case BE_WINDOW_RESIZED:
		if (window->configure_serial != 0) {
			/* Un-acked configure in flight: coalesce to newest. */
			window->pending_width = ev->x;
			window->pending_height = ev->y;
		} else if (ev->x != window->width
				|| ev->y != window->height) {
			window->configure_serial = wlr_xdg_toplevel_set_size(
				toplevel, ev->x, ev->y);
		}
		break;

	case BE_WINDOW_MOVED:
		/* Bookkeeping only — Wayland clients don't know positions;
		 * this feeds desktop→window pointer translation. */
		window->x = ev->x;
		window->y = ev->y;
		break;

	case BE_WINDOW_FOCUS_IN:
		wlr_xdg_toplevel_set_activated(toplevel, true);
		vitrine_focus_surface(server, toplevel->base->surface);
		/* Activating a parent raises it over its own dialogs — put it
		 * back underneath them (focus stays here, which is what the
		 * user asked for by clicking). */
		raise_children_above(window, 0);
		break;

	case BE_WINDOW_FOCUS_OUT:
		wlr_xdg_toplevel_set_activated(toplevel, false);
		break;

	default:
		break;
	}
	return true;
}

/* xdg-decoration: we always prefer server-side (BeOS tab). The mode may
 * only be configured on an initialized surface — decoration objects are
 * typically created BEFORE the initial commit, so sending immediately
 * trips "configure scheduled for an uninitialized xdg_surface"; defer to
 * the initial commit (shell.c calls vitrine_rootless_apply_decoration). */
static void
handle_new_decoration(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;

	struct vitrine_toplevel *toplevel = decoration->toplevel->base->data;
	if (toplevel != NULL)
		toplevel->decoration = decoration;

	if (decoration->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
vitrine_rootless_apply_decoration(struct vitrine_toplevel *toplevel)
{
	if (toplevel->decoration != NULL)
		wlr_xdg_toplevel_decoration_v1_set_mode(toplevel->decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
vitrine_rootless_init(struct vitrine_server *server)
{
	wl_list_init(&server->rootless_windows);

	beshim_set_rootless(server->shim, 1);

	server->decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server->display);
	server->new_decoration.notify = handle_new_decoration;
	wl_signal_add(&server->decoration_manager->events.new_toplevel_decoration,
		&server->new_decoration);

	/* GTK3 speaks only the KDE protocol; default SERVER makes it drop
	 * CSD (headerbar + shadows) without any per-surface handshake. */
	struct wlr_server_decoration_manager *kde_manager =
		wlr_server_decoration_manager_create(server->display);
	wlr_server_decoration_manager_set_default_mode(kde_manager,
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
}

/* Hooks called from shell.c so both modes share one xdg-shell entry. */
void
vitrine_rootless_new_toplevel(struct vitrine_toplevel *toplevel)
{
	(void)toplevel; /* the BeOS window is created lazily at map */
}

void
vitrine_rootless_toplevel_map(struct vitrine_toplevel *toplevel)
{
	rootless_map(toplevel);
}

void
vitrine_rootless_toplevel_commit(struct vitrine_toplevel *toplevel)
{
	rootless_commit(toplevel);
}

void
vitrine_rootless_toplevel_set_parent(struct vitrine_toplevel *toplevel)
{
	struct vitrine_rootless_window *window = toplevel->rootless;
	if (window == NULL || window->window == NULL)
		return;

	struct vitrine_rootless_window *parent = parent_window_of(window);
	if (parent != NULL && parent->window != NULL)
		beshim_send_behind(parent->window, window->window);
}

void
vitrine_rootless_toplevel_unmap(struct vitrine_toplevel *toplevel)
{
	rootless_unmap(toplevel);
}
