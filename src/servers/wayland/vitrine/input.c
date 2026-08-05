/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * BeOS input → wl_seat translation, with the PoC's hard-won stuck-key
 * defenses. Events arrive as BeInputEvent records on the shim's
 * O_NONBLOCK self-pipe (bewindow.h); this file drains them and feeds a
 * virtual wlr_keyboard plus direct seat pointer notifications (no
 * wlr_cursor — the pointer image belongs to app_server, positions come
 * in absolute).
 *
 * Keymap: the SAME source of truth as the BeOS side. VitruvianOS is
 * xkb-native — the keyboard input-server add-on reads RMLVO from
 * ~/config/settings/input/xkb_layout (KeyboardInputDevice.cpp) and
 * GENERATES the BeOS keymap from it (BKeymap::PopulateFromXkb). Reading
 * the same file with the same parser and defaults makes the Wayland and
 * BeOS layouts identical by construction; an inotify watch reloads both
 * worlds in step.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/util/log.h>

#include "vitrine.h"

/* BeOS button bitmask (InterfaceDefs.h): B_PRIMARY_MOUSE_BUTTON = 0x1
 * (left), B_SECONDARY_MOUSE_BUTTON = 0x2 (RIGHT), B_TERTIARY_MOUSE_BUTTON
 * = 0x4 (MIDDLE) — the 0x2/0x4 pair maps crosswise to BTN_RIGHT/BTN_MIDDLE. */
#define BE_BUTTON_LEFT   0x1
#define BE_BUTTON_RIGHT  0x2
#define BE_BUTTON_MIDDLE 0x4

static const struct wlr_keyboard_impl keyboard_impl = {
	.name = "vitrine-keyboard",
};

static uint32_t
time_msec(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

/* ---- pressed-key bookkeeping (evdev codes 0..255, LSB-first) ---- */

static bool
pressed_get(struct vitrine_input *input, int code)
{
	return (input->pressed[code >> 3] & (1 << (code & 7))) != 0;
}

static void
pressed_set(struct vitrine_input *input, int code, bool down)
{
	if (down)
		input->pressed[code >> 3] |= 1 << (code & 7);
	else
		input->pressed[code >> 3] &= ~(1 << (code & 7));
}

/* ---- keymap: shared source of truth with the input_server add-on ---- */

/* Mirrors KeyboardInputDevice.cpp's parser exactly: key=value lines,
 * defaults rules=evdev model=pc105, empty layout/variant/options fall back
 * to xkbcommon defaults (us). */
static struct xkb_keymap *
load_keymap(struct xkb_context *context)
{
	char rules[64] = "evdev";
	char model[64] = "pc105";
	char layout[64] = "";
	char variant[64] = "";
	char options[256] = "";

	const char *home = getenv("HOME");
	if (home != NULL) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path),
			"%s/config/settings/input/xkb_layout", home);
		FILE *f = fopen(path, "r");
		if (f != NULL) {
			char line[512];
			while (fgets(line, sizeof(line), f) != NULL) {
				char *nl = strchr(line, '\n');
				if (nl != NULL)
					*nl = '\0';
#define PARSE(key, out) \
	if (strncmp(line, key "=", sizeof(key)) == 0) \
		snprintf(out, sizeof(out), "%s", line + sizeof(key))
				PARSE("rules", rules);
				PARSE("model", model);
				PARSE("layout", layout);
				PARSE("variant", variant);
				PARSE("options", options);
#undef PARSE
			}
			fclose(f);
		}
	}

	struct xkb_rule_names names = {
		rules[0] != '\0' ? rules : NULL,
		model[0] != '\0' ? model : NULL,
		layout[0] != '\0' ? layout : NULL,
		variant[0] != '\0' ? variant : NULL,
		options[0] != '\0' ? options : NULL,
	};
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap == NULL) {
		wlr_log(WLR_ERROR, "xkb_layout '%s/%s/%s' failed to compile — "
			"falling back to evdev/pc105/us",
			rules, model, layout);
		struct xkb_rule_names fallback =
			{ "evdev", "pc105", "us", "", "" };
		keymap = xkb_keymap_new_from_names(context, &fallback,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	} else {
		wlr_log(WLR_INFO, "keymap: rules=%s model=%s layout=%s "
			"variant=%s options=%s", rules, model,
			layout[0] != '\0' ? layout : "(default us)",
			variant, options);
	}
	return keymap;
}

static void
apply_keymap(struct vitrine_input *input)
{
	struct xkb_context *context =
		xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (context == NULL)
		return;
	struct xkb_keymap *keymap = load_keymap(context);
	if (keymap != NULL) {
		wlr_keyboard_set_keymap(&input->keyboard, keymap);
		xkb_keymap_unref(keymap);
	}
	xkb_context_unref(context);
}

static int
handle_inotify(int fd, uint32_t mask, void *data)
{
	struct vitrine_input *input = data;

	char buf[4096];
	ssize_t len;
	bool relevant = false;
	while ((len = read(fd, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;
		while (off < len) {
			const struct inotify_event *ev =
				(const struct inotify_event *)(buf + off);
			if (ev->len > 0
					&& strcmp(ev->name, "xkb_layout") == 0)
				relevant = true;
			off += sizeof(*ev) + ev->len;
		}
	}
	(void)len;

	if (relevant) {
		wlr_log(WLR_INFO, "xkb_layout changed — reloading keymap");
		apply_keymap(input);
	}
	return 0;
}

static void
setup_keymap_watch(struct vitrine_input *input)
{
	const char *home = getenv("HOME");
	if (home == NULL)
		return;

	/* A fresh HOME has no settings/input dir yet, and a watch that is
	 * never armed never recovers — so create the canonical path up front
	 * (the same place the Keymap preference and the keyboard add-on use;
	 * we run as the session user). EEXIST is the common case. */
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/config", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/config/settings", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/config/settings/input", home);
	mkdir(dir, 0755);

	input->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (input->inotify_fd < 0)
		return;
	if (inotify_add_watch(input->inotify_fd, dir,
			IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
		wlr_log(WLR_INFO, "cannot watch %s — keymap live reload "
			"disabled", dir);
		close(input->inotify_fd);
		input->inotify_fd = -1;
		return;
	}
	wlr_log(WLR_INFO, "keymap live reload armed on %s", dir);
	input->inotify_source = wl_event_loop_add_fd(
		input->server->event_loop, input->inotify_fd,
		WL_EVENT_READABLE, handle_inotify, input);
}

/* ---- keyboard events ---- */

static void
notify_key(struct vitrine_input *input, int code, bool down)
{
	struct wlr_keyboard_key_event event = {
		.time_msec = time_msec(),
		.keycode = (uint32_t)code, /* raw evdev, NO +8 — wlroots and
		                            * clients shift for xkb themselves */
		.update_state = true,
		.state = down ? WL_KEYBOARD_KEY_STATE_PRESSED
			: WL_KEYBOARD_KEY_STATE_RELEASED,
	};
	wlr_keyboard_notify_key(&input->keyboard, &event);
}

static void
handle_key(struct vitrine_input *input, const BeInputEvent *ev)
{
	int code = ev->code;
	bool down = ev->buttons != 0;

	if (code < 0 || code > 255)
		return;

	/* input_server delivers its own autorepeat as repeated B_KEY_DOWNs;
	 * Wayland clients repeat themselves (repeat_info), so forwarding
	 * those would double-repeat. The pressed-set filter drops them. */
	if (down && pressed_get(input, code))
		return;
	if (!down && !pressed_get(input, code))
		return;

	pressed_set(input, code, down);
	notify_key(input, code, down);
}

/* Stuck-key defense #1: releases lost to BeOS focus changes / host grabs
 * would otherwise leave clients auto-repeating forever. Release-only,
 * never synthetic presses. */
static void
release_all_keys(struct vitrine_input *input)
{
	for (int code = 0; code < 256; code++) {
		if (pressed_get(input, code)) {
			pressed_set(input, code, false);
			notify_key(input, code, false);
		}
	}
}

/* Stuck-key defense #2: BE_INPUT_KEY_STATES follows every key event with
 * the input_server's authoritative 16-byte physical bitmap (MSB-first per
 * byte, evdev codes 0..127). Any key we think is down whose bit is clear
 * gets a synthesized release. */
static void
reconcile_key_states(struct vitrine_input *input, const BeInputEvent *ev)
{
	const unsigned char *states = (const unsigned char *)&ev->code;

	for (int code = 0; code < 128; code++) {
		bool phys_down =
			(states[code >> 3] & (1 << (7 - (code & 7)))) != 0;
		if (!phys_down && pressed_get(input, code)) {
			wlr_log(WLR_DEBUG, "reconcile: releasing evdev=%d",
				code);
			pressed_set(input, code, false);
			notify_key(input, code, false);
		}
	}
}

/* wlr_keyboard events → seat (tinywl pattern; xkb state and modifiers are
 * maintained by wlr_keyboard_notify_key above). */
static void
handle_keyboard_key(struct wl_listener *listener, void *data)
{
	struct vitrine_input *input =
		wl_container_of(listener, input, keyboard_key);
	struct wlr_keyboard_key_event *event = data;

	wlr_seat_keyboard_notify_key(input->server->seat, event->time_msec,
		event->keycode, event->state);
}

static void
handle_keyboard_modifiers(struct wl_listener *listener, void *data)
{
	struct vitrine_input *input =
		wl_container_of(listener, input, keyboard_modifiers);

	wlr_seat_keyboard_notify_modifiers(input->server->seat,
		&input->keyboard.modifiers);
}

/* ---- pointer events (rootful: window-local == output-local coords) ---- */

void
vitrine_focus_surface(struct vitrine_server *server,
	struct wlr_surface *surface)
{
	struct wlr_seat *seat = server->seat;

	if (surface == NULL || seat->keyboard_state.focused_surface == surface)
		return;

	wlr_log(WLR_DEBUG, "keyboard focus -> surface %p (was %p)",
		(void *)surface, (void *)seat->keyboard_state.focused_surface);
	wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0,
		&server->input.keyboard.modifiers);
}

static struct wlr_surface *
scene_surface_at(struct wlr_scene *scene, double x, double y,
	double *sx, double *sy)
{
	struct wlr_scene_node *node = wlr_scene_node_at(
		&scene->tree.node, x, y, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(buffer);
	if (scene_surface == NULL)
		return NULL;
	return scene_surface->surface;
}

static struct wlr_surface *
surface_at(struct vitrine_server *server, double x, double y,
	double *sx, double *sy)
{
	return scene_surface_at(server->scene, x, y, sx, sy);
}

static void
handle_motion(struct vitrine_input *input, const BeInputEvent *ev)
{
	struct vitrine_server *server = input->server;

	/* Zero-delta "motion" carries no user intent — it is a state snapshot
	 * (a freshly created override-redirect snoop view emits one). Acting
	 * on it re-focuses whatever just appeared under the STATIONARY
	 * cursor, and both toolkits punish that: GTK menus treat the opening
	 * click's release as a selection (F3 lesson), and Xt spring menus
	 * pop down on the synthetic crossing events (X11 window 2 died 3 ms
	 * after map — timeline in the F4 notes). Pointer focus follows real
	 * motion only. */
	if (ev->x == (int)input->pointer_x && ev->y == (int)input->pointer_y)
		return;

	input->pointer_x = ev->x;
	input->pointer_y = ev->y;

	double sx, sy;
	struct wlr_surface *surface;
	if (server->rootful) {
		/* Window-local coords, single global scene. */
		surface = surface_at(server, ev->x, ev->y, &sx, &sy);
	} else if (input->button_mask != 0
			&& server->seat->pointer_state.focused_surface != NULL) {
		/* Implicit grab: while a button is held, ALL motion belongs to
		 * the surface that owns the press — never re-hit-test, never
		 * enter/leave. Retargeting mid-hold is a protocol violation
		 * that Xwayland turns into NotifyNormal crossing events, and
		 * Xt spring menus pop down on them (xterm's Ctrl menu died on
		 * the first real hover motion). Menu-item highlighting happens
		 * inside Xwayland via the client's own X grab. Coordinates may
		 * legitimately leave the surface bounds during the hold. */
		wlr_seat_pointer_notify_motion(server->seat, time_msec(),
			ev->x - input->focus_origin_x,
			ev->y - input->focus_origin_y);
		wlr_seat_pointer_notify_frame(server->seat);
		return;
	} else {
		/* X override-redirect windows (menus) snoop desktop-wide just
		 * like Wayland popups — the same consume-only-the-snoop rule
		 * applies (finding #10). Wayland popups win when both exist. */
		int top_popup = vitrine_popup_top_win_id(server);
		if (top_popup < 0)
			top_popup = vitrine_xwayland_top_or_win_id(server);
		if (top_popup >= 0) {
			/* Popup grab: its override-redirect view snoops
			 * desktop-wide, so the same physical motion arrives
			 * once per window with different win_ids — consume
			 * ONLY the snoop stream and hit-test desktop coords
			 * ourselves (finding #10; anything else flaps
			 * enter/leave and breaks menu hover). */
			if (ev->screen != top_popup)
				return;
			surface = vitrine_desktop_surface_at(server,
				ev->x, ev->y, &sx, &sy);
		} else {
			/* Desktop-absolute coords; the emitting window's
			 * win_id rides in ev->screen — app_server already did
			 * the hit-test between windows, we only translate
			 * into that window's private scene. */
			struct vitrine_rootless_window *window =
				vitrine_rootless_window_by_id(server,
					ev->screen);
			if (window != NULL && window->window != NULL) {
				surface = scene_surface_at(window->scene,
					ev->x - window->x, ev->y - window->y,
					&sx, &sy);
			} else {
				/* Not a Wayland window — an X window's? */
				surface = vitrine_xwayland_surface_at_win(
					server, ev->screen, ev->x, ev->y,
					&sx, &sy);
			}
			if (surface == NULL) {
				wlr_seat_pointer_notify_clear_focus(
					server->seat);
				wlr_seat_pointer_notify_frame(server->seat);
				return;
			}
		}
	}

	if (surface != server->seat->pointer_state.focused_surface)
		wlr_log(WLR_DEBUG, "pointer focus -> %p (win=%d at %d,%d)",
			(void *)surface, ev->screen, ev->x, ev->y);
	if (surface != NULL) {
		input->focus_origin_x = ev->x - sx;
		input->focus_origin_y = ev->y - sy;
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time_msec(),
			sx, sy);
	} else {
		wlr_seat_pointer_notify_clear_focus(server->seat);
	}
	wlr_seat_pointer_notify_frame(server->seat);
}

/* Do NOT hand pointer focus to a popup the moment it maps under a
 * stationary cursor. It looks like the protocol-correct thing to do, and it
 * is what a "menu flashed and vanished" symptom tempts you into, but the
 * wl_pointer.enter makes the opening click's RELEASE land on the menu —
 * which GTK reads as press-drag-release, i.e. "pick the item under the
 * cursor", and closes the menu it just opened. Measured with injected
 * clicks (90 ms hold, cursor pinned): 8/8 menus self-closed with the enter,
 * ~50 % without it, 0 % with a real mouse. The remaining synthetic failures
 * are that same toolkit gesture, not a compositor bug — pointer focus
 * follows real motion (handle_motion) and that is enough. */

static void
handle_button(struct vitrine_input *input, const BeInputEvent *ev)
{
	struct vitrine_server *server = input->server;
	uint32_t now = time_msec();
	uint32_t new_mask = (uint32_t)ev->buttons;
	uint32_t changed = new_mask ^ input->button_mask;

	/* Popup-grab dedupe (see handle_motion): the snoop stream wins. */
	if (!server->rootful) {
		int top_popup = vitrine_popup_top_win_id(server);
		if (top_popup < 0)
			top_popup = vitrine_xwayland_top_or_win_id(server);
		wlr_log(WLR_DEBUG,
			"button: win=%d buttons=0x%x at %d,%d top_popup=%d%s",
			ev->screen, (unsigned)ev->buttons, ev->x, ev->y,
			top_popup,
			(top_popup >= 0 && ev->screen != top_popup)
				? " DROPPED" : "");
		if (top_popup >= 0 && ev->screen != top_popup)
			return;
	}

	static const struct {
		uint32_t be_bit;
		uint32_t btn;
	} map[] = {
		{ BE_BUTTON_LEFT, BTN_LEFT },
		{ BE_BUTTON_RIGHT, BTN_RIGHT },
		{ BE_BUTTON_MIDDLE, BTN_MIDDLE },
	};

	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (!(changed & map[i].be_bit))
			continue;
		bool down = (new_mask & map[i].be_bit) != 0;
		if (down && server->rootful) {
			/* Rootful click-to-focus; in rootless the BeOS focus
			 * change delivers BE_WINDOW_FOCUS_IN instead. */
			double sx, sy;
			struct wlr_surface *surface = surface_at(server,
				input->pointer_x, input->pointer_y, &sx, &sy);
			vitrine_focus_surface(server, surface);
		}
		wlr_seat_pointer_notify_button(server->seat, now, map[i].btn,
			down ? WL_POINTER_BUTTON_STATE_PRESSED
				: WL_POINTER_BUTTON_STATE_RELEASED);
	}
	input->button_mask = new_mask;
	wlr_seat_pointer_notify_frame(server->seat);
}

static void
handle_wheel(struct vitrine_input *input, const BeInputEvent *ev)
{
	struct wlr_seat *seat = input->server->seat;
	uint32_t now = time_msec();

	/* Popup-grab dedupe (see handle_motion). */
	if (!input->server->rootful) {
		int top_popup = vitrine_popup_top_win_id(input->server);
		if (top_popup < 0)
			top_popup = vitrine_xwayland_top_or_win_id(
				input->server);
		if (top_popup >= 0 && ev->screen != top_popup)
			return;
	}

	/* BeOS wheel deltas are already quantized to steps; positive y =
	 * scroll down, positive x = scroll right — same sign convention as
	 * Wayland axis values. One step = 15.0 axis units / 120 v120 units
	 * (the libinput convention clients calibrate against). */
	if (ev->y != 0) {
		wlr_seat_pointer_notify_axis(seat, now,
			WL_POINTER_AXIS_VERTICAL_SCROLL, ev->y * 15.0,
			ev->y * 120, WL_POINTER_AXIS_SOURCE_WHEEL,
			WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
	}
	if (ev->x != 0) {
		wlr_seat_pointer_notify_axis(seat, now,
			WL_POINTER_AXIS_HORIZONTAL_SCROLL, ev->x * 15.0,
			ev->x * 120, WL_POINTER_AXIS_SOURCE_WHEEL,
			WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
	}
	wlr_seat_pointer_notify_frame(seat);
}

static void
handle_window_event(struct vitrine_input *input, const BeInputEvent *ev)
{
	struct vitrine_server *server = input->server;

	/* Stuck-key defense runs in BOTH modes, on every focus loss (in
	 * rootless that includes inter-window switches — releases are
	 * harmless, lost releases are fatal). */
	if (ev->code == BE_WINDOW_FOCUS_OUT)
		release_all_keys(input);

	if (server->rootful) {
		if (ev->code == BE_WINDOW_CLOSE) {
			wlr_log(WLR_INFO, "screen window closed — terminating");
			wl_display_terminate(server->display);
		}
		return;
	}

	if (!vitrine_rootless_handle_window_event(server, ev))
		vitrine_xwayland_handle_window_event(server, ev);
}

/* ---- shim pipe drain ---- */

static int
handle_shim_input(int fd, uint32_t mask, void *data)
{
	struct vitrine_server *server = data;
	struct vitrine_input *input = &server->input;

	BeInputEvent ev;
	while (beshim_next_event(server->shim, &ev)) {
		switch (ev.type) {
		case BE_INPUT_KEY:
			handle_key(input, &ev);
			break;
		case BE_INPUT_KEY_STATES:
			reconcile_key_states(input, &ev);
			break;
		case BE_INPUT_MOTION:
			handle_motion(input, &ev);
			break;
		case BE_INPUT_BUTTON:
			handle_button(input, &ev);
			break;
		case BE_INPUT_WHEEL:
			handle_wheel(input, &ev);
			break;
		case BE_INPUT_WINDOW:
			handle_window_event(input, &ev);
			break;
		case BE_INPUT_CLIPBOARD:
			vitrine_clipboard_handle_be_change(server);
			break;
		default:
			break;
		}
	}
	return 0;
}

/* ---- selections ---- */

static void
handle_request_set_selection(struct wl_listener *listener, void *data)
{
	struct vitrine_input *input =
		wl_container_of(listener, input, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;

	wlr_seat_set_selection(input->server->seat, event->source,
		event->serial);
}

static void
handle_request_set_primary_selection(struct wl_listener *listener, void *data)
{
	struct vitrine_input *input = wl_container_of(listener, input,
		request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;

	wlr_log(WLR_DEBUG, "primary selection request: source=%p serial=%u",
		(void *)event->source, event->serial);
	wlr_seat_set_primary_selection(input->server->seat, event->source,
		event->serial);
}

/* ---- init ---- */

void
vitrine_input_init(struct vitrine_server *server)
{
	struct vitrine_input *input = &server->input;
	input->server = server;
	input->inotify_fd = -1;

	wlr_keyboard_init(&input->keyboard, &keyboard_impl,
		"vitrine-keyboard");
	apply_keymap(input);
	wlr_keyboard_set_repeat_info(&input->keyboard, 25, 600);
	setup_keymap_watch(input);

	input->keyboard_key.notify = handle_keyboard_key;
	wl_signal_add(&input->keyboard.events.key, &input->keyboard_key);
	input->keyboard_modifiers.notify = handle_keyboard_modifiers;
	wl_signal_add(&input->keyboard.events.modifiers,
		&input->keyboard_modifiers);

	wlr_seat_set_keyboard(server->seat, &input->keyboard);
	wlr_seat_set_capabilities(server->seat,
		WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);

	/* Cursors (F7 decision): wl_pointer.set_cursor is deliberately NOT
	 * handled — the desktop keeps one app_server cursor for every window,
	 * native or not, which is what makes hosted clients feel like part of
	 * the system. A client asking for its own bitmap is answered by our
	 * silence; wlroots requires no acknowledgement. Mapping xcursor names
	 * onto BeOS cursors (resize arrows, I-beam) is the stretch goal. */

	input->request_set_selection.notify = handle_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection,
		&input->request_set_selection);
	input->request_set_primary_selection.notify =
		handle_request_set_primary_selection;
	wl_signal_add(&server->seat->events.request_set_primary_selection,
		&input->request_set_primary_selection);

	server->input_source = wl_event_loop_add_fd(server->event_loop,
		beshim_input_fd(server->shim), WL_EVENT_READABLE,
		handle_shim_input, server);
}
