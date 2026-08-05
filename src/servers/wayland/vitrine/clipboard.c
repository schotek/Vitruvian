/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * CLIPBOARD ⇄ BClipboard text bridge (F7, finding #11).
 *
 * wl → BeOS: every seat selection we did not set ourselves is read (first
 * text mime the source offers) and committed into the BeOS clipboard,
 * tagged "vos:wl-bridge" so the resulting change notification is not
 * bounced back. X clients ride for free: the wlr XWM converts X CLIPBOARD
 * ownership into a seat selection before we ever see it.
 *
 * BeOS → wl: BE_INPUT_CLIPBOARD (system clipboard changed) re-reads the
 * clip; unless it carries our tag, a compositor-local data source serving
 * the text becomes the new seat selection — Wayland clients paste it
 * directly, X clients through the XWM.
 *
 * v1 scope (plan): text only, both selections exist but only CLIPBOARD
 * maps onto BClipboard; DnD is out.
 */

#define _GNU_SOURCE /* pipe2 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wlr/types/wlr_data_device.h>
#include <wlr/util/log.h>

#include "vitrine.h"

/* Refuse to buffer unbounded clipboard payloads (images pasted as text…). */
#define BRIDGE_MAX_TEXT (4u * 1024u * 1024u)

struct vitrine_clipboard_source {
	struct wlr_data_source base;
	struct vitrine_clipboard *clipboard;
	char *text;
};

/* ---- BeOS → wl: the compositor-local data source ---- */

static void
bridge_source_send(struct wlr_data_source *source, const char *mime, int fd)
{
	struct vitrine_clipboard_source *bridge =
		wl_container_of(source, bridge, base);
	(void)mime; /* every offered mime serves the same UTF-8 text */

	/* The receiving client may hand us a non-blocking fd; a short text
	 * write is atomic enough in practice, but loop for correctness. */
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	size_t len = strlen(bridge->text);
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, bridge->text + off, len - off);
		if (n <= 0)
			break;
		off += (size_t)n;
	}
	close(fd);
}

static void
bridge_source_destroy(struct wlr_data_source *source)
{
	struct vitrine_clipboard_source *bridge =
		wl_container_of(source, bridge, base);
	if (bridge->clipboard->bridge_source == bridge)
		bridge->clipboard->bridge_source = NULL;
	free(bridge->text);
	free(bridge);
}

static const struct wlr_data_source_impl bridge_source_impl = {
	.send = bridge_source_send,
	.destroy = bridge_source_destroy,
};

void
vitrine_clipboard_handle_be_change(struct vitrine_server *server)
{
	struct vitrine_clipboard *clipboard = &server->clipboard;

	int from_bridge = 0;
	char *text = beshim_clipboard_get_text(server->shim, &from_bridge);
	if (from_bridge) {
		/* Echo guard: our own wl→BeOS commit bouncing back. */
		free(text);
		return;
	}
	if (text == NULL)
		return; /* no text flavor — out of v1 scope */

	struct vitrine_clipboard_source *bridge =
		calloc(1, sizeof(*bridge));
	if (bridge == NULL) {
		free(text);
		return;
	}
	wlr_data_source_init(&bridge->base, &bridge_source_impl);
	bridge->clipboard = clipboard;
	bridge->text = text;

	static const char *mimes[] = {
		"text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
		"STRING", "TEXT",
	};
	for (size_t i = 0; i < sizeof(mimes) / sizeof(mimes[0]); i++) {
		char **p = wl_array_add(&bridge->base.mime_types,
			sizeof(char *));
		if (p != NULL)
			*p = strdup(mimes[i]);
	}

	/* Replaces (and thereby destroys) any previous bridge source. */
	clipboard->bridge_source = bridge;
	wlr_seat_set_selection(server->seat, &bridge->base,
		wl_display_next_serial(server->display));
	wlr_log(WLR_DEBUG, "clipboard: BeOS -> wl, %zu bytes",
		strlen(bridge->text));
}

/* ---- wl → BeOS: read the client source, commit into BClipboard ---- */

static void
reader_finish(struct vitrine_clipboard *clipboard, bool commit)
{
	if (clipboard->reader != NULL) {
		wl_event_source_remove(clipboard->reader);
		clipboard->reader = NULL;
	}
	if (clipboard->reader_fd >= 0) {
		close(clipboard->reader_fd);
		clipboard->reader_fd = -1;
	}
	if (commit && clipboard->reader_len > 0
			&& clipboard->reader_buf != NULL) {
		clipboard->reader_buf[clipboard->reader_len] = '\0';
		beshim_clipboard_set_text(clipboard->server->shim,
			clipboard->reader_buf);
		wlr_log(WLR_DEBUG, "clipboard: wl -> BeOS, %zu bytes",
			clipboard->reader_len);
	}
	clipboard->reader_len = 0;
}

static int
reader_ready(int fd, uint32_t mask, void *data)
{
	struct vitrine_clipboard *clipboard = data;

	if (mask & WL_EVENT_READABLE) {
		for (;;) {
			if (clipboard->reader_len + 4096 + 1
					> clipboard->reader_cap) {
				size_t cap = clipboard->reader_cap == 0
					? 8192 : clipboard->reader_cap * 2;
				if (cap > BRIDGE_MAX_TEXT) {
					reader_finish(clipboard, false);
					return 0;
				}
				char *buf = realloc(clipboard->reader_buf, cap);
				if (buf == NULL) {
					reader_finish(clipboard, false);
					return 0;
				}
				clipboard->reader_buf = buf;
				clipboard->reader_cap = cap;
			}
			ssize_t n = read(fd,
				clipboard->reader_buf + clipboard->reader_len,
				4096);
			if (n > 0) {
				clipboard->reader_len += (size_t)n;
				continue;
			}
			if (n == 0) {
				reader_finish(clipboard, true); /* EOF: commit */
				return 0;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0; /* drained for now, wait for more */
			reader_finish(clipboard, false); /* read error */
			return 0;
		}
	}

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
		reader_finish(clipboard, clipboard->reader_len > 0);
	return 0;
}

static const char *
pick_text_mime(struct wlr_data_source *source)
{
	static const char *const preferred[] = {
		"text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
		"STRING", "TEXT",
	};
	for (size_t i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++) {
		char **p;
		wl_array_for_each(p, &source->mime_types) {
			if (strcmp(*p, preferred[i]) == 0)
				return *p;
		}
	}
	return NULL;
}

static void
handle_seat_set_selection(struct wl_listener *listener, void *data)
{
	struct vitrine_clipboard *clipboard =
		wl_container_of(listener, clipboard, seat_set_selection);
	struct vitrine_server *server = clipboard->server;
	struct wlr_data_source *source = server->seat->selection_source;

	/* NULL: selection cleared. Bridge impl: our own BeOS-sourced
	 * selection — copying it back would loop through the tag anyway,
	 * skip the round trip entirely. */
	if (source == NULL || source->impl == &bridge_source_impl)
		return;

	const char *mime = pick_text_mime(source);
	if (mime == NULL)
		return; /* no text flavor — out of v1 scope */

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0)
		return;
	fcntl(fds[0], F_SETFL, O_NONBLOCK);

	reader_finish(clipboard, false); /* supersede an unfinished read */

	wlr_data_source_send(source, mime, fds[1]);
	close(fds[1]);

	clipboard->reader_fd = fds[0];
	clipboard->reader = wl_event_loop_add_fd(server->event_loop, fds[0],
		WL_EVENT_READABLE, reader_ready, clipboard);
	if (clipboard->reader == NULL) {
		close(fds[0]);
		clipboard->reader_fd = -1;
	}
}

void
vitrine_clipboard_init(struct vitrine_server *server)
{
	struct vitrine_clipboard *clipboard = &server->clipboard;
	clipboard->server = server;
	clipboard->reader_fd = -1;

	clipboard->seat_set_selection.notify = handle_seat_set_selection;
	wl_signal_add(&server->seat->events.set_selection,
		&clipboard->seat_set_selection);
}
