/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * vitrine_window_host — the per-window helper process (phase H1).
 *
 * Hosts guest windows OUTSIDE the compositor's team so each guest app can
 * get its own Deskbar entry. The compositor spawns this binary with the
 * control socket coordinates in the environment (hostproto.h); windows are
 * the very same XHostWindow/XFramebufferView objects the in-process shim
 * uses (bewindow_win.cpp) — their BeEventSink simply points at the socket,
 * so every input record reaches the compositor's vitrine_input_dispatch()
 * unchanged. Pixels arrive by reference: WIN_CREATE carries a nexus
 * area_id created by the compositor; the clone wraps a BBitmap and
 * WH_WIN_DAMAGE messages replay the usual LockLooper + DrawBitmapAsync
 * blits.
 *
 * H1 is a single generic helper (one per compositor, no app identity);
 * per-app spawning and identity stubs are H3.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <Application.h>
#include <Bitmap.h>
#include <OS.h>

#include "../bewindow_win.h"
#include "../hostproto.h"

static const char* kSignature = "application/x-vnd.vos-VitrineHost";

struct HostedWindow {
	int win_id = -1;
	BeWindow* window = nullptr;
	area_id clone = -1;
	HostedWindow* next = nullptr;
};

static BeEventSink sSink = { -1, 0, 0 };
static HostedWindow* sWindows = nullptr;
static int sSocket = -1;

static HostedWindow*
window_by_id(int win_id)
{
	for (HostedWindow* w = sWindows; w != nullptr; w = w->next) {
		if (w->win_id == win_id)
			return w;
	}
	return nullptr;
}

static void
handle_create(const struct wh_header* hdr, const struct wh_create* msg)
{
	/* Clone the compositor's framebuffer area. The clone must itself be
	 * cloneable: BBitmap's client-area constructor makes app_server clone
	 * it again (AS_RECONNECT_BITMAP -> BitmapManager::CloneFromClient). */
	void* bits = NULL;
	area_id clone = clone_area("vitrine fb clone", &bits, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA, (area_id)msg->area);
	if (clone < 0) {
		fprintf(stderr, "window_host: clone_area(%d): %s\n",
			(int)msg->area, strerror(clone));
		return;
	}

	BBitmap* bitmap = new BBitmap(clone, 0,
		BRect(0, 0, msg->w - 1, msg->h - 1), 0, B_RGB32, msg->stride);
	if (bitmap->InitCheck() != B_OK) {
		fprintf(stderr, "window_host: BBitmap over area: %s\n",
			strerror(bitmap->InitCheck()));
		delete bitmap;
		delete_area(clone);
		return;
	}

	BeWindowSpec spec = BeWindowSpec();
	spec.x = msg->x;
	spec.y = msg->y;
	spec.w = msg->w;
	spec.h = msg->h;
	spec.title = msg->title;
	spec.override_redirect = 0;
	spec.resizable = msg->resizable;
	spec.win_id = (int)hdr->win_id;
	spec.borderless = msg->borderless;

	BeWindow* window = bewin_create_x_with_bitmap(&sSink, &spec, bitmap);
	if (window == NULL) {
		delete bitmap;
		delete_area(clone);
		return;
	}

	HostedWindow* hosted = new HostedWindow();
	hosted->win_id = (int)hdr->win_id;
	hosted->window = window;
	hosted->clone = clone;
	hosted->next = sWindows;
	sWindows = hosted;
}

static void
handle_destroy(int win_id)
{
	HostedWindow** link = &sWindows;
	while (*link != nullptr && (*link)->win_id != win_id)
		link = &(*link)->next;
	HostedWindow* hosted = *link;
	if (hosted == nullptr)
		return;
	*link = hosted->next;

	bewin_destroy_window(hosted->window);
	delete_area(hosted->clone);
	delete hosted;
}

/* Control record up to the compositor (bounded-blocking, unlike the input
 * stream): losing a RESIZE_DONE would strand the parked source area on the
 * compositor side, so wait out a momentarily full queue instead of
 * dropping. Runs on the reader thread — never a window looper. */
static void
send_control(const BeInputEvent& ev)
{
	for (int attempt = 0; attempt < 40; attempt++) {
		if (send(sSocket, &ev, sizeof(ev), MSG_DONTWAIT)
				== (ssize_t)sizeof(ev))
			return;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			exit(0);	/* compositor gone */
		struct pollfd pfd = { sSocket, POLLOUT, 0 };
		poll(&pfd, 1, 250);
	}
	fprintf(stderr, "window_host: control record type %d stuck, "
		"dropped\n", (int)ev.type);
}

/* WH_WIN_RESIZE (H2): adopt the new framebuffer area the compositor sent —
 * clone, wrap a BBitmap, swap it into the window (bewin_* does the resize
 * under the looper lock), drop the old clone, ack. On any failure keep the
 * old framebuffer and DON'T ack: the compositor then never deletes the old
 * source area, so what we keep showing stays backed. */
static void
handle_resize(HostedWindow* hosted, const struct wh_resize* msg)
{
	void* bits = NULL;
	area_id clone = clone_area("vitrine fb clone", &bits, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA,
		(area_id)msg->area);
	if (clone < 0) {
		fprintf(stderr, "window_host: resize clone_area(%d): %s\n",
			(int)msg->area, strerror(clone));
		return;
	}

	BBitmap* bitmap = new BBitmap(clone, 0,
		BRect(0, 0, msg->w - 1, msg->h - 1), 0, B_RGB32, msg->stride);
	if (bitmap->InitCheck() != B_OK) {
		fprintf(stderr, "window_host: resize BBitmap: %s\n",
			strerror(bitmap->InitCheck()));
		delete bitmap;
		delete_area(clone);
		return;
	}

	if (bewin_resize_window_with_bitmap(hosted->window, msg->w, msg->h,
			bitmap) != 0) {
		delete bitmap;
		delete_area(clone);
		return;
	}
	delete_area(hosted->clone);
	hosted->clone = clone;

	BeInputEvent ack = BeInputEvent();
	ack.type = WH_BE_RESIZE_DONE;
	ack.screen = hosted->win_id;
	ack.x = msg->w;
	ack.y = msg->h;
	send_control(ack);
}

static void
drain_socket()
{
	/* Header + largest payload in one packet (SEQPACKET boundaries). */
	uint8_t buf[sizeof(struct wh_header) + 512];

	for (;;) {
		ssize_t n = recv(sSocket, buf, sizeof(buf), MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			exit(0);	/* compositor gone */
		}
		if (n == 0)
			exit(0);		/* EOF: compositor gone, windows die with us */
		if (n < (ssize_t)sizeof(struct wh_header))
			continue;

		struct wh_header hdr;
		memcpy(&hdr, buf, sizeof(hdr));
		const void* payload = buf + sizeof(hdr);
		if ((ssize_t)(sizeof(hdr) + hdr.len) > n)
			continue;

		switch (hdr.type) {
		case WH_WIN_CREATE:
			if (hdr.len >= sizeof(struct wh_create))
				handle_create(&hdr, (const struct wh_create*)payload);
			break;
		case WH_WIN_DESTROY:
			handle_destroy((int)hdr.win_id);
			break;
		case WH_WIN_DAMAGE: {
			HostedWindow* hosted = window_by_id((int)hdr.win_id);
			if (hosted == nullptr)
				break;
			int nrects = hdr.len / sizeof(struct wh_rect);
			const struct wh_rect* rects =
				(const struct wh_rect*)payload;
			for (int i = 0; i < nrects; i++) {
				bewin_blit(hosted->window, rects[i].x, rects[i].y,
					rects[i].w, rects[i].h);
			}
			break;
		}
		case WH_WIN_MOVE: {
			HostedWindow* hosted = window_by_id((int)hdr.win_id);
			if (hosted == nullptr || hdr.len < sizeof(struct wh_move))
				break;
			const struct wh_move* msg = (const struct wh_move*)payload;
			bewin_move_window(hosted->window, msg->x, msg->y);
			break;
		}
		case WH_WIN_RESIZE: {
			HostedWindow* hosted = window_by_id((int)hdr.win_id);
			if (hosted == nullptr
					|| hdr.len < sizeof(struct wh_resize))
				break;
			handle_resize(hosted, (const struct wh_resize*)payload);
			break;
		}
		case WH_WIN_SET_TITLE: {
			HostedWindow* hosted = window_by_id((int)hdr.win_id);
			if (hosted == nullptr || hdr.len == 0)
				break;
			/* Payload is NUL-terminated by the sender; force it in
			 * case a foreign compositor build disagrees. */
			char title[256];
			uint32_t len = hdr.len < sizeof(title)
				? hdr.len : sizeof(title) - 1;
			memcpy(title, payload, len);
			title[len] = '\0';
			bewin_set_title(hosted->window, title);
			break;
		}
		case WH_WIN_SET_LIMITS: {
			HostedWindow* hosted = window_by_id((int)hdr.win_id);
			if (hosted == nullptr
					|| hdr.len < sizeof(struct wh_limits))
				break;
			const struct wh_limits* msg =
				(const struct wh_limits*)payload;
			bewin_set_size_limits(hosted->window, msg->min_w,
				msg->min_h, msg->max_w, msg->max_h);
			break;
		}
		case WH_WIN_ACTIVATE: {
			HostedWindow* hosted = window_by_id((int)hdr.win_id);
			if (hosted == nullptr)
				break;
			bewin_activate(hosted->window);
			break;
		}
		case WH_WIN_SEND_BEHIND: {
			HostedWindow* hosted = window_by_id((int)hdr.win_id);
			if (hosted == nullptr
					|| hdr.len < sizeof(struct wh_behind))
				break;
			const struct wh_behind* msg =
				(const struct wh_behind*)payload;
			HostedWindow* behind_of =
				window_by_id((int)msg->behind_win_id);
			if (behind_of == nullptr)
				break;	/* already destroyed — restack is moot */
			bewin_send_behind(hosted->window, behind_of->window);
			break;
		}
		case WH_HOST_QUIT:
			exit(0);
		default:
			break;
		}
	}
}

static int32
reader_thread(void* /*arg*/)
{
	struct pollfd pfd = { sSocket, POLLIN, 0 };
	for (;;) {
		if (poll(&pfd, 1, -1) < 0) {
			if (errno == EINTR)
				continue;
			exit(0);
		}
		if (pfd.revents & (POLLHUP | POLLERR))
			exit(0);
		drain_socket();
	}
	return 0;
}

class HostApp : public BApplication {
public:
	HostApp(status_t* error)
		: BApplication(kSignature, error) {}
};

int
main()
{
	const char* path = getenv(WH_SOCKET_ENV);
	const char* token = getenv(WH_TOKEN_ENV);
	if (path == NULL || token == NULL) {
		fprintf(stderr, "window_host: not started by the compositor "
			"(missing %s/%s)\n", WH_SOCKET_ENV, WH_TOKEN_ENV);
		return 1;
	}

	sSocket = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (sSocket < 0)
		return 1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(sSocket, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		fprintf(stderr, "window_host: connect(%s): %s\n", path,
			strerror(errno));
		return 1;
	}

	/* The windows' event sink IS the socket: 24-byte records, one packet
	 * each, non-blocking (bewindow_win.cpp SELF-PIPE OVERFLOW policy). */
	fcntl(sSocket, F_SETFL, fcntl(sSocket, F_GETFL, 0) | O_NONBLOCK);
	sSink.pipe_w = sSocket;

	/* app_server may still be warming up at session boot; same retry as
	 * the compositor shim and the tray applet. */
	const int kMaxAttempts = 40;
	status_t error = B_ERROR;
	HostApp* app = NULL;
	for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
		error = B_ERROR;
		app = new HostApp(&error);
		if (error == B_OK)
			break;
		delete app;
		app = NULL;
		if (attempt + 1 < kMaxAttempts)
			snooze(250000);
	}
	if (app == NULL) {
		fprintf(stderr, "window_host: app_server not reachable (%s)\n",
			strerror(error));
		return 1;
	}

	/* HELLO: version + token, the compositor's spawn/connection match. */
	unsigned long long tok = strtoull(token, NULL, 10);
	BeInputEvent hello = BeInputEvent();
	hello.type = WH_BE_HELLO;
	hello.code = WH_PROTO_VERSION;
	hello.x = (int32_t)(tok & 0xffffffffu);
	hello.y = (int32_t)(tok >> 32);
	if (send(sSocket, &hello, sizeof(hello), 0) != (ssize_t)sizeof(hello)) {
		fprintf(stderr, "window_host: HELLO failed\n");
		return 1;
	}

	thread_id reader = spawn_thread(reader_thread, "host_reader",
		B_DISPLAY_PRIORITY, NULL);
	if (reader < B_OK)
		return 1;
	resume_thread(reader);

	app->Run();
	return 0;
}
