/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * winhost.h — compositor side of the per-window helper (phase H1).
 * Gated by VITRINE_WINHOST=1; with the gate off nothing here runs and the
 * in-process beshim path is used unchanged (it stays compiled in forever
 * as the fallback).
 */
#ifndef VITRINE_WINHOST_H
#define VITRINE_WINHOST_H

#include <stdbool.h>
#include <stdint.h>

#include <wayland-server-core.h>

#include "bewindow.h"

struct vitrine_server;

/* A guest window hosted in a helper process: the compositor's proxy. The
 * shared framebuffer stays mapped in the compositor (bits/stride) — the
 * pixman path writes it exactly like a BBitmap's Bits(). `area` is a nexus
 * area_id; kept as a plain int32 here so this header stays free of BeOS
 * kernel headers (the compositor core is plain C). */
struct vitrine_hosted_window {
	int win_id;
	int32_t area;
	void *bits;
	int stride;
	int width, height;
	/* Respawn snapshot (H4): everything a fresh helper needs to recreate
	 * this window after a crash. Geometry/title track the winhost_* ops;
	 * user-driven moves reach us via winhost_note_move() from the
	 * BE_WINDOW_MOVED handlers. */
	int x, y;
	int resizable, borderless;
	char title[128];
	struct winhost *host;		/* NULL after the helper died */
	struct wl_list link;		/* winhost.windows */
	/* Superseded framebuffer areas (H2 resize): each WH_WIN_RESIZE parks
	 * the previous area here and the matching WH_BE_RESIZE_DONE — acks
	 * come back in send order — deletes the oldest. Never deleting a
	 * source area the helper hasn't re-cloned past keeps us independent
	 * of clone-outlives-source semantics. */
	struct wl_list old_areas;	/* winhost_old_area.link, oldest first */
};

bool winhost_enabled(struct vitrine_server *server);
void winhost_init(struct vitrine_server *server);
void winhost_finish(struct vitrine_server *server);

/* app_id keys the helper team the window lands in (one team per guest app,
 * H3); NULL/"" selects the generic bucket helper. */
struct vitrine_hosted_window *winhost_create_window(
	struct vitrine_server *server, const BeWindowSpec *spec,
	const char *app_id);
void winhost_destroy_window(struct vitrine_hosted_window *hosted);
void winhost_send_damage(struct vitrine_hosted_window *hosted,
	int x, int y, int w, int h);

/* H2 window operations — the hosted mirrors of the beshim_* calls. All of
 * them quietly no-op once the helper is gone. */
void winhost_move_window(struct vitrine_hosted_window *hosted, int x, int y);
/* Area-swap resize: creates the new framebuffer, flips the compositor-side
 * mapping and sends WH_WIN_RESIZE. Returns 0 on success — only then may the
 * caller resize the wlr output; on failure the old size stays authoritative. */
int winhost_resize_window(struct vitrine_hosted_window *hosted, int w, int h);
void winhost_set_title(struct vitrine_hosted_window *hosted,
	const char *title);
void winhost_set_limits(struct vitrine_hosted_window *hosted,
	int min_w, int min_h, int max_w, int max_h);
void winhost_activate(struct vitrine_hosted_window *hosted);
void winhost_send_behind(struct vitrine_hosted_window *hosted,
	struct vitrine_hosted_window *behind_of);
/* Bookkeeping only (no message): a user drag moved the window — keep the
 * respawn snapshot current. Call from BE_WINDOW_MOVED handlers. */
void winhost_note_move(struct vitrine_hosted_window *hosted, int x, int y);

#endif	/* VITRINE_WINHOST_H */
