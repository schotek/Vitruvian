/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * hostproto.h — wire protocol between the Vitrine compositor and its
 * per-window helper processes (per-window-helper plan, phase H1).
 *
 * Transport: one SOCK_SEQPACKET unix connection per helper, duplex.
 *  - compositor → helper: one message per packet, fixed header + payload.
 *  - helper → compositor: raw 24-byte BeInputEvent records (bewindow.h) —
 *    the windows' own event stream, win_id in .screen. Control replies use
 *    the same record shape with .type >= WH_BE_BASE so the compositor can
 *    split the stream on the type field alone.
 * The pixel plane travels OUTSIDE the socket: a nexus area created by the
 * compositor (B_CLONEABLE_AREA) whose id rides in wh_create; nexus area
 * ids are global, the helper clone_area()s it and wraps a BBitmap around
 * the clone.
 */
#ifndef VITRINE_HOSTPROTO_H
#define VITRINE_HOSTPROTO_H

#include <stdint.h>

#define WH_SOCKET_ENV	"VITRINE_HOST_SOCKET"
#define WH_TOKEN_ENV	"VITRINE_HOST_TOKEN"
#define WH_SIG_ENV	"VITRINE_HOST_SIG"	/* app signature (H3 stubs) */
#define WH_PROTO_VERSION	2

/* compositor → helper message types */
enum {
	WH_WIN_CREATE = 1,	/* payload: struct wh_create                  */
	WH_WIN_DESTROY,		/* no payload                                 */
	WH_WIN_DAMAGE,		/* payload: wh_rect[header.len/sizeof(wh_rect)] */
	WH_HOST_QUIT,		/* no payload; helper exits                   */
	/* H2: the compositor-driven window operations. */
	WH_WIN_MOVE,		/* payload: struct wh_move                    */
	WH_WIN_RESIZE,		/* payload: struct wh_resize — a NEW framebuffer
				 * area; the helper clones it, swaps its BBitmap
				 * under the window lock, drops the old clone and
				 * acks with WH_BE_RESIZE_DONE (the compositor
				 * deletes the old source area only then).      */
	WH_WIN_SET_TITLE,	/* payload: NUL-terminated title (len incl. NUL) */
	WH_WIN_SET_LIMITS,	/* payload: struct wh_limits                  */
	WH_WIN_ACTIVATE,	/* no payload                                 */
	WH_WIN_SEND_BEHIND,	/* payload: struct wh_behind; header.win_id
				 * goes behind that window (same helper team) */
};

struct wh_header {
	uint32_t type;
	uint32_t win_id;
	uint32_t len;		/* payload bytes following the header */
};

struct wh_create {
	int32_t x, y, w, h;
	int32_t stride;		/* bytes per row of the shared area */
	int32_t area;		/* nexus area_id of the framebuffer */
	int32_t resizable;
	int32_t borderless;
	char title[128];
};

struct wh_rect {
	int32_t x, y, w, h;
};

struct wh_move {
	int32_t x, y;
};

struct wh_resize {
	int32_t w, h;
	int32_t stride;		/* bytes per row of the new area */
	int32_t area;		/* nexus area_id of the new framebuffer */
};

struct wh_limits {
	int32_t min_w, min_h;	/* <= 0 = unconstrained */
	int32_t max_w, max_h;
};

struct wh_behind {
	int32_t behind_win_id;
};

/* helper → compositor: BeInputEvent.type values >= WH_BE_BASE are host
 * control records, not input. */
enum {
	WH_BE_BASE = 100,
	WH_BE_HELLO = 100,	/* code = WH_PROTO_VERSION, x/y = token lo/hi */
	WH_BE_RESIZE_DONE,	/* screen = win_id, x/y = acked w/h; the old
				 * framebuffer area may be deleted now       */
};

#endif	/* VITRINE_HOSTPROTO_H */
