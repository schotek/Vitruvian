/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * xdg-shell entry shared by both modes. Rootful: toplevels land in the
 * global scene at 0,0 (single-output debug mode). Rootless: everything
 * per-toplevel is delegated to rootless.c (private scene + BeOS window).
 */

#include <stdlib.h>

#include <wlr/util/log.h>

#include "vitrine.h"
#include "winhost.h"

static void
toplevel_handle_commit(struct wl_listener *listener, void *data)
{
	struct vitrine_toplevel *toplevel =
		wl_container_of(listener, toplevel, commit);

	/* The initial commit expects a configure; 0x0 lets the client pick
	 * its own size. A pre-created decoration object is configured here
	 * too — the earliest legal moment (surface now initialized). */
	if (toplevel->toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_size(toplevel->toplevel, 0, 0);
		if (!toplevel->server->rootful)
			vitrine_rootless_apply_decoration(toplevel);
		return;
	}

	if (!toplevel->server->rootful)
		vitrine_rootless_toplevel_commit(toplevel);
}

static void
toplevel_handle_map(struct wl_listener *listener, void *data)
{
	struct vitrine_toplevel *toplevel =
		wl_container_of(listener, toplevel, map);

	if (toplevel->server->rootful) {
		/* Newest window takes keyboard focus (click-to-focus handles
		 * the rest — input.c). */
		vitrine_focus_surface(toplevel->server,
			toplevel->toplevel->base->surface);
	} else {
		vitrine_rootless_toplevel_map(toplevel);
	}
}

static void
toplevel_handle_unmap(struct wl_listener *listener, void *data)
{
	struct vitrine_toplevel *toplevel =
		wl_container_of(listener, toplevel, unmap);

	if (!toplevel->server->rootful)
		vitrine_rootless_toplevel_unmap(toplevel);
}

static void
toplevel_handle_set_title(struct wl_listener *listener, void *data)
{
	struct vitrine_toplevel *toplevel =
		wl_container_of(listener, toplevel, set_title);

	if (toplevel->rootless == NULL)
		return;
	if (toplevel->rootless->hosted != NULL)
		winhost_set_title(toplevel->rootless->hosted,
			toplevel->toplevel->title);
	else if (toplevel->rootless->window != NULL)
		beshim_set_title(toplevel->rootless->window,
			toplevel->toplevel->title);
}

static void
toplevel_handle_set_parent(struct wl_listener *listener, void *data)
{
	struct vitrine_toplevel *toplevel =
		wl_container_of(listener, toplevel, set_parent);

	if (!toplevel->server->rootful)
		vitrine_rootless_toplevel_set_parent(toplevel);
}

static void
toplevel_handle_destroy(struct wl_listener *listener, void *data)
{
	struct vitrine_toplevel *toplevel =
		wl_container_of(listener, toplevel, destroy);

	/* Normally unmap already scheduled the teardown; this is the safety
	 * net for destroy-without-unmap paths. */
	if (!toplevel->server->rootful)
		vitrine_rootless_toplevel_unmap(toplevel);

	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->set_title.link);
	wl_list_remove(&toplevel->set_parent.link);
	wl_list_remove(&toplevel->destroy.link);
	free(toplevel);
}

static void
handle_new_toplevel(struct wl_listener *listener, void *data)
{
	struct vitrine_server *server =
		wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct vitrine_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (toplevel == NULL)
		return;
	toplevel->server = server;
	toplevel->toplevel = xdg_toplevel;
	/* Back-pointer for the xdg-decoration handler (rootless.c). */
	xdg_toplevel->base->data = toplevel;

	if (server->rootful) {
		toplevel->scene_tree = wlr_scene_xdg_surface_create(
			&server->scene->tree, xdg_toplevel->base);
		wlr_scene_node_set_position(&toplevel->scene_tree->node, 0, 0);
	} else {
		vitrine_rootless_new_toplevel(toplevel);
	}

	toplevel->commit.notify = toplevel_handle_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit,
		&toplevel->commit);
	toplevel->map.notify = toplevel_handle_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map,
		&toplevel->map);
	toplevel->unmap.notify = toplevel_handle_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap,
		&toplevel->unmap);
	toplevel->set_title.notify = toplevel_handle_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
	toplevel->set_parent.notify = toplevel_handle_set_parent;
	wl_signal_add(&xdg_toplevel->events.set_parent, &toplevel->set_parent);
	toplevel->destroy.notify = toplevel_handle_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	wlr_log(WLR_INFO, "new xdg_toplevel: %s",
		xdg_toplevel->title != NULL ? xdg_toplevel->title : "(untitled)");
}

void
vitrine_shell_init(struct vitrine_server *server)
{
	server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
	server->new_xdg_toplevel.notify = handle_new_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel,
		&server->new_xdg_toplevel);
}
