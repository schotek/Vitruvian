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
#define WH_PROTO_VERSION	1

/* compositor → helper message types */
enum {
	WH_WIN_CREATE = 1,	/* payload: struct wh_create                  */
	WH_WIN_DESTROY,		/* no payload                                 */
	WH_WIN_DAMAGE,		/* payload: wh_rect[header.len/sizeof(wh_rect)] */
	WH_HOST_QUIT,		/* no payload; helper exits                   */
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

/* helper → compositor: BeInputEvent.type values >= WH_BE_BASE are host
 * control records, not input. */
enum {
	WH_BE_BASE = 100,
	WH_BE_HELLO = 100,	/* code = WH_PROTO_VERSION, x/y = token lo/hi */
};

#endif	/* VITRINE_HOSTPROTO_H */
