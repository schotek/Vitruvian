/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Rootless xdg_popup handling: each popup (menu, combo, tooltip) gets an
 * override-redirect BeOS window + private scene + internal output, same
 * pattern as toplevels (rootless.c). Popup positions are protocol-relative
 * to the parent's geometry origin, which by construction equals the
 * parent's BeOS window origin — so desktop position = parent origin +
 * popup geometry offset. Constraint adjustment (flip/slide at screen
 * edges) runs through wlr_xdg_popup_unconstrain_from_box against the
 * desktop box translated into parent coordinates.
 *
 * Grab semantics (dismiss on outside click, keyboard redirection) are
 * wlroots' built-in xdg popup seat grab — our only duty is delivering
 * pointer events with correct surface targeting (input.c + the hit-test
 * helpers below). Teardown uses the same deferred idle pattern as
 * rootless.c.
 */

#include <stdlib.h>

#include <wlr/util/log.h>

#include "vitrine.h"

/* Resolve the desktop-absolute origin of a popup's parent surface (the
 * parent is either a toplevel or another popup). Returns false if the
 * parent has no window (yet). */
static bool
parent_origin(struct vitrine_server *server, struct wlr_surface *parent,
	int *x, int *y)
{
	struct vitrine_popup *popup;
	wl_list_for_each(popup, &server->popups, link) {
		if (popup->popup != NULL
				&& popup->popup->base->surface == parent) {
			*x = popup->x;
			*y = popup->y;
			return true;
		}
	}

	struct vitrine_rootless_window *window;
	wl_list_for_each(window, &server->rootless_windows, link) {
		if (window->toplevel != NULL
				&& window->toplevel->toplevel->base->surface
					== parent) {
			*x = window->x;
			*y = window->y;
			return true;
		}
	}
	return false;
}

int
vitrine_popup_top_win_id(struct vitrine_server *server)
{
	struct vitrine_popup *popup;
	wl_list_for_each(popup, &server->popups, link) {
		if (popup->window != NULL)
			return popup->win_id;
	}
	return -1;
}

struct wlr_surface *
vitrine_desktop_surface_at(struct vitrine_server *server, double x, double y,
	double *sx, double *sy)
{
	/* Popups first (newest first == topmost by construction). */
	struct vitrine_popup *popup;
	wl_list_for_each(popup, &server->popups, link) {
		if (popup->window == NULL)
			continue;
		double lx = x - popup->x;
		double ly = y - popup->y;
		if (lx < 0 || ly < 0 || lx >= popup->width
				|| ly >= popup->height)
			continue;
		struct wlr_scene_node *node = wlr_scene_node_at(
			&popup->scene->tree.node, lx, ly, sx, sy);
		if (node != NULL && node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_surface *ss =
				wlr_scene_surface_try_from_buffer(
					wlr_scene_buffer_from_node(node));
			if (ss != NULL)
				return ss->surface;
		}
	}

	/* Then X windows (newest first — override-redirect menus live at the
	 * top of the BeOS stack by construction). */
	struct wlr_surface *xsurface = vitrine_xwayland_desktop_surface_at(
		server, x, y, sx, sy);
	if (xsurface != NULL)
		return xsurface;

	/* Then toplevels. Their mutual stacking is BeOS-side and unknown to
	 * us; prefer the keyboard-focused one, else first hit — during a
	 * popup grab the practical target is the popup's own parent anyway. */
	struct wlr_surface *focused =
		server->seat->keyboard_state.focused_surface;
	struct vitrine_rootless_window *window;
	for (int pass = 0; pass < 2; pass++) {
		wl_list_for_each(window, &server->rootless_windows, link) {
			if (window->window == NULL
					|| window->toplevel == NULL)
				continue;
			struct wlr_surface *surface =
				window->toplevel->toplevel->base->surface;
			if (pass == 0 && surface != focused)
				continue;
			double lx = x - window->x;
			double ly = y - window->y;
			if (lx < 0 || ly < 0 || lx >= window->width
					|| ly >= window->height)
				continue;
			struct wlr_scene_node *node = wlr_scene_node_at(
				&window->scene->tree.node, lx, ly, sx, sy);
			if (node != NULL
					&& node->type == WLR_SCENE_NODE_BUFFER) {
				struct wlr_scene_surface *ss =
					wlr_scene_surface_try_from_buffer(
						wlr_scene_buffer_from_node(
							node));
				if (ss != NULL)
					return ss->surface;
			}
		}
	}
	return NULL;
}

static void
popup_teardown_idle(void *data)
{
	struct vitrine_popup *popup = data;

	if (popup->output != NULL)
		wlr_output_destroy(&popup->output->base);
	if (popup->scene != NULL)
		wlr_scene_node_destroy(&popup->scene->tree.node);
	wl_list_remove(&popup->link);
	free(popup);
}

static void
popup_schedule_teardown(struct vitrine_popup *popup)
{
	if (popup->teardown_scheduled)
		return;
	wlr_log(WLR_INFO, "popup %d torn down", popup->win_id);
	popup->teardown_scheduled = true;
	popup->window = NULL;
	wl_event_loop_add_idle(popup->server->event_loop, popup_teardown_idle,
		popup);
}

static void
popup_update_position(struct vitrine_popup *popup)
{
	struct wlr_xdg_popup *xdg_popup = popup->popup;

	int px = 0, py = 0;
	if (!parent_origin(popup->server, xdg_popup->parent, &px, &py))
		return;

	int x = px + xdg_popup->current.geometry.x;
	int y = py + xdg_popup->current.geometry.y;
	if (x != popup->x || y != popup->y) {
		popup->x = x;
		popup->y = y;
		if (popup->window != NULL)
			beshim_move_window(popup->window, x, y);
	}
}

static void
popup_handle_map(struct wl_listener *listener, void *data)
{
	struct vitrine_popup *popup = wl_container_of(listener, popup, map);
	struct vitrine_server *server = popup->server;
	struct wlr_xdg_popup *xdg_popup = popup->popup;

	struct wlr_box geo = xdg_popup->base->current.geometry;
	if (geo.width <= 0 || geo.height <= 0) {
		geo.x = 0;
		geo.y = 0;
		geo.width = xdg_popup->base->surface->current.width;
		geo.height = xdg_popup->base->surface->current.height;
	}
	if (geo.width <= 0 || geo.height <= 0)
		return;

	int px = 0, py = 0;
	if (!parent_origin(server, xdg_popup->parent, &px, &py))
		return;

	popup->win_id = ++server->next_win_id;
	popup->width = geo.width;
	popup->height = geo.height;
	popup->x = px + xdg_popup->current.geometry.x;
	popup->y = py + xdg_popup->current.geometry.y;

	BeWindowSpec spec = {
		.x = popup->x,
		.y = popup->y,
		.w = geo.width,
		.h = geo.height,
		.title = NULL,
		.override_redirect = 1,
		.resizable = 0,
		.win_id = popup->win_id,
		.borderless = 0,
	};
	popup->window = beshim_create_xwindow(server->shim, &spec);
	if (popup->window == NULL)
		return;

	popup->output = vitrine_output_create_from_window(server,
		popup->window, geo.width, geo.height);
	if (popup->output == NULL) {
		beshim_destroy_window(popup->window);
		popup->window = NULL;
		return;
	}

	popup->scene = wlr_scene_create();
	/* Menus routinely carry alpha — matte like toplevels (finding #8). */
	wlr_scene_rect_create(&popup->scene->tree, geo.width, geo.height,
		(float[4]){ 0.85f, 0.85f, 0.85f, 1.0f });
	/* NOT wlr_scene_xdg_surface_create: for ROLE_POPUP it also moves the
	 * returned tree to the popup's parent-relative position on every
	 * commit (scene_xdg_surface_update_position, types/scene/xdg_shell.c)
	 * — correct in tinywl's single scene, but in a private per-popup
	 * scene it shoves the content outside the internal output entirely.
	 * The BeOS window already sits at the popup's desktop position, so
	 * build the subsurface tree directly and anchor the window-geometry
	 * origin at (0,0) ourselves (re-applied on commit, geometry moves). */
	popup->surface_tree = wlr_scene_subsurface_tree_create(
		&popup->scene->tree, xdg_popup->base->surface);
	wlr_scene_node_set_position(&popup->surface_tree->node,
		-geo.x, -geo.y);
	popup->output->scene_output = wlr_scene_output_create(popup->scene,
		&popup->output->base);

	wlr_log(WLR_INFO, "popup %d: %dx%d at %d,%d", popup->win_id,
		geo.width, geo.height, popup->x, popup->y);
}

static void
popup_handle_commit(struct wl_listener *listener, void *data)
{
	struct vitrine_popup *popup = wl_container_of(listener, popup, commit);
	struct wlr_xdg_popup *xdg_popup = popup->popup;

	if (xdg_popup->base->initial_commit) {
		/* Flip/slide at screen edges: the desktop box translated into
		 * the parent-geometry coordinate space the positioner uses. */
		int px = 0, py = 0;
		if (parent_origin(popup->server, xdg_popup->parent,
				&px, &py)) {
			int screen_w = 0, screen_h = 0;
			if (beshim_screen_size(popup->server->shim, &screen_w,
					&screen_h) != 0
					|| screen_w <= 0 || screen_h <= 0) {
				screen_w = 1024;
				screen_h = 768;
			}
			struct wlr_box box = {
				.x = -px,
				.y = -py,
				.width = screen_w,
				.height = screen_h,
			};
			wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
		}
		return;
	}

	if (popup->window == NULL)
		return;

	/* Follow repositioning (submenus) and late size changes. */
	popup_update_position(popup);

	struct wlr_box geo = xdg_popup->base->current.geometry;
	if (popup->surface_tree != NULL)
		wlr_scene_node_set_position(&popup->surface_tree->node,
			-geo.x, -geo.y);
	if (geo.width > 0 && geo.height > 0
			&& (geo.width != popup->width
				|| geo.height != popup->height)) {
		popup->width = geo.width;
		popup->height = geo.height;
		beshim_resize_window(popup->window, geo.width, geo.height);
		vitrine_output_resize(popup->output, geo.width, geo.height);
	}
}

static void
popup_handle_unmap(struct wl_listener *listener, void *data)
{
	struct vitrine_popup *popup = wl_container_of(listener, popup, unmap);
	popup_schedule_teardown(popup);
}

static void
popup_handle_destroy(struct wl_listener *listener, void *data)
{
	struct vitrine_popup *popup =
		wl_container_of(listener, popup, destroy);

	popup_schedule_teardown(popup);
	popup->popup = NULL;
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->map.link);
	wl_list_remove(&popup->unmap.link);
	wl_list_remove(&popup->destroy.link);
}

static void
handle_new_popup(struct wl_listener *listener, void *data)
{
	struct vitrine_server *server =
		wl_container_of(listener, server, new_xdg_popup);
	struct wlr_xdg_popup *xdg_popup = data;

	struct vitrine_popup *popup = calloc(1, sizeof(*popup));
	if (popup == NULL)
		return;
	popup->server = server;
	popup->popup = xdg_popup;

	/* Newest first == topmost (BeOS shows new windows on top too). */
	wl_list_insert(&server->popups, &popup->link);

	popup->commit.notify = popup_handle_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit,
		&popup->commit);
	popup->map.notify = popup_handle_map;
	wl_signal_add(&xdg_popup->base->surface->events.map, &popup->map);
	popup->unmap.notify = popup_handle_unmap;
	wl_signal_add(&xdg_popup->base->surface->events.unmap, &popup->unmap);
	popup->destroy.notify = popup_handle_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

void
vitrine_popup_init(struct vitrine_server *server)
{
	wl_list_init(&server->popups);
	server->new_xdg_popup.notify = handle_new_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup,
		&server->new_xdg_popup);
}
