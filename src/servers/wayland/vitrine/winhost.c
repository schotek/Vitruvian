/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * winhost.c — compositor side of the per-window helper (phase H1).
 *
 * H1 scope: ONE generic helper process hosting Wayland toplevels, spawned
 * lazily on the first hosted window. Pixels go through a nexus shared area
 * (created here with B_CLONEABLE_AREA; the id rides in WIN_CREATE and the
 * helper clones it), damage goes as WH_WIN_DAMAGE messages, and the
 * helper's windows report input back as the same 24-byte BeInputEvent
 * records the in-process shim pipe carries — drained here into
 * vitrine_input_dispatch(), so everything downstream is shared.
 *
 * Identity stubs, respawn and the crash-storm fallback are later phases;
 * in H1 a dead helper just logs, its windows go dark and close normally
 * via the client path.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>

/* Plain-C consumer of the BeOS kernel API: SupportDefs.h expects the
 * __haiku_* base types to be predeclared (the C++ sources get them via
 * LinuxBuildCompatibility.h). */
#include <config/types.h>
#include <kernel/OS.h>
#include <kernel/image.h>

#include "vitrine.h"
#include "winhost.h"
#include "hostproto.h"

#define WH_HELPER_PATH "/system/servers/vitrine_window_host"

struct winhost {
	struct vitrine_server *server;
	int listen_fd;
	int fd;				/* helper connection; -1 until HELLO   */
	uint64_t token;
	pid_t pid;
	struct wl_event_source *source;
	struct wl_list windows;		/* vitrine_hosted_window.link */
};

/* A framebuffer area superseded by a resize, parked until the helper acks
 * the swap (winhost.h old_areas docs). */
struct winhost_old_area {
	int32_t area;
	struct wl_list link;
};

bool
winhost_enabled(struct vitrine_server *server)
{
	(void)server;
	static int enabled = -1;
	if (enabled < 0) {
		const char *env = getenv("VITRINE_WINHOST");
		enabled = (env != NULL && env[0] == '1') ? 1 : 0;
		if (enabled && access(WH_HELPER_PATH, X_OK) != 0) {
			wlr_log(WLR_ERROR, "winhost: %s missing, gate forced off",
				WH_HELPER_PATH);
			enabled = 0;
		}
	}
	return enabled == 1;
}

static const char *
winhost_socket_path(void)
{
	static char path[256];
	if (path[0] == '\0') {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		snprintf(path, sizeof(path), "%s/vitrine-host.sock",
			rt != NULL ? rt : "/tmp");
	}
	return path;
}

/* WH_BE_RESIZE_DONE: the helper swapped to the area sent in the matching
 * WH_WIN_RESIZE and dropped its clone of the previous one — the oldest
 * parked source area is now unreferenced on the helper side. Acks arrive
 * in send order (single SEQPACKET stream), so FIFO matching is exact. */
static void
winhost_handle_resize_done(struct winhost *host, const BeInputEvent *ev)
{
	struct vitrine_hosted_window *hosted;
	wl_list_for_each(hosted, &host->windows, link) {
		if (hosted->win_id != (int)ev->screen)
			continue;
		if (wl_list_empty(&hosted->old_areas)) {
			wlr_log(WLR_ERROR,
				"winhost: stray RESIZE_DONE for win %d",
				hosted->win_id);
			return;
		}
		struct winhost_old_area *old = wl_container_of(
			hosted->old_areas.next, old, link);
		wl_list_remove(&old->link);
		delete_area(old->area);
		free(old);
		wlr_log(WLR_DEBUG, "winhost: win %d resize acked (%dx%d)",
			hosted->win_id, ev->x, ev->y);
		return;
	}
	/* Window already destroyed — its areas went with it. */
}

/* One helper→compositor record. Type >= WH_BE_BASE is host control
 * (RESIZE_DONE; HELLO is handled in the accept path); everything else is
 * window input for the shared dispatch. */
static int
winhost_handle_fd(int fd, uint32_t mask, void *data)
{
	struct winhost *host = data;
	struct vitrine_server *server = host->server;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		wlr_log(WLR_ERROR, "winhost: helper pid %d went away",
			(int)host->pid);
		wl_event_source_remove(host->source);
		host->source = NULL;
		close(host->fd);
		host->fd = -1;
		/* H1: no respawn. Proxies keep their mapped area (commits still
		 * render into it harmlessly); the guest windows die with the
		 * helper's team and the clients see their surfaces closed. */
		return 0;
	}

	BeInputEvent ev;
	ssize_t n;
	while ((n = recv(fd, &ev, sizeof(ev), MSG_DONTWAIT))
			== (ssize_t)sizeof(ev)) {
		if (ev.type == WH_BE_RESIZE_DONE) {
			winhost_handle_resize_done(host, &ev);
			continue;
		}
		if (ev.type >= WH_BE_BASE)
			continue;	/* unknown control record */
		vitrine_input_dispatch(server, &ev);
	}
	return 0;
}

static int
winhost_send(struct winhost *host, uint32_t type, uint32_t win_id,
	const void *payload, uint32_t len)
{
	if (host == NULL || host->fd < 0)
		return -1;

	uint8_t buf[sizeof(struct wh_header) + 512];
	if (sizeof(struct wh_header) + len > sizeof(buf))
		return -1;
	struct wh_header hdr = { type, win_id, len };
	memcpy(buf, &hdr, sizeof(hdr));
	if (len > 0)
		memcpy(buf + sizeof(hdr), payload, len);

	ssize_t n = send(host->fd, buf, sizeof(hdr) + len, MSG_DONTWAIT);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			/* H1: drop; damage self-heals on the next frame, create/
			 * destroy loss would only matter under a wedged helper. */
			wlr_log(WLR_ERROR, "winhost: send queue full, dropped %u",
				type);
			return -1;
		}
		return -1;
	}
	return 0;
}

/* Spawn the helper and wait for its HELLO (blocking, bounded): H1 keeps
 * the handshake synchronous because it only ever runs once, on the first
 * hosted window. */
static int
winhost_spawn(struct winhost *host)
{
	struct vitrine_server *server = host->server;

	host->token = ((uint64_t)getpid() << 32) ^ (uint64_t)system_time();

	char sockenv[300], tokenv[64];
	snprintf(sockenv, sizeof(sockenv), WH_SOCKET_ENV "=%s",
		winhost_socket_path());
	snprintf(tokenv, sizeof(tokenv), WH_TOKEN_ENV "=%llu",
		(unsigned long long)host->token);

	/* load_image (not fork/exec): the child needs the nexus team records
	 * registrar/app_server key on. Environment: ours plus the socket
	 * coordinates. */
	extern char **environ;
	int envc = 0;
	while (environ[envc] != NULL)
		envc++;
	const char **envp = calloc(envc + 3, sizeof(char *));
	if (envp == NULL)
		return -1;
	for (int i = 0; i < envc; i++)
		envp[i] = environ[i];
	envp[envc] = sockenv;
	envp[envc + 1] = tokenv;
	envp[envc + 2] = NULL;

	const char *argv[] = { WH_HELPER_PATH, NULL };
	thread_id team = load_image(1, argv, envp);
	free(envp);
	if (team < 0) {
		wlr_log(WLR_ERROR, "winhost: load_image(%s): %s",
			WH_HELPER_PATH, strerror(team));
		return -1;
	}
	resume_thread(team);
	host->pid = (pid_t)team;

	/* Accept + HELLO with a 5 s budget. */
	struct pollfd pfd = { .fd = host->listen_fd, .events = POLLIN };
	if (poll(&pfd, 1, 5000) <= 0) {
		wlr_log(WLR_ERROR, "winhost: helper did not connect");
		return -1;
	}
	int fd = accept4(host->listen_fd, NULL, NULL, SOCK_CLOEXEC);
	if (fd < 0)
		return -1;

	struct ucred cred;
	socklen_t credlen = sizeof(cred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &credlen) != 0
		|| cred.uid != getuid()) {
		wlr_log(WLR_ERROR, "winhost: rejecting foreign-uid connection");
		close(fd);
		return -1;
	}

	pfd.fd = fd;
	if (poll(&pfd, 1, 5000) <= 0) {
		close(fd);
		return -1;
	}
	BeInputEvent hello;
	if (recv(fd, &hello, sizeof(hello), 0) != (ssize_t)sizeof(hello)
		|| hello.type != WH_BE_HELLO
		|| hello.code != WH_PROTO_VERSION
		|| ((uint32_t)hello.x
			!= (uint32_t)(host->token & 0xffffffffu))
		|| ((uint32_t)hello.y != (uint32_t)(host->token >> 32))) {
		wlr_log(WLR_ERROR, "winhost: bad HELLO");
		close(fd);
		return -1;
	}

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	host->fd = fd;
	host->source = wl_event_loop_add_fd(server->event_loop, fd,
		WL_EVENT_READABLE, winhost_handle_fd, host);
	wlr_log(WLR_INFO, "winhost: helper up, pid %d", (int)host->pid);
	return 0;
}

void
winhost_init(struct vitrine_server *server)
{
	if (!winhost_enabled(server))
		return;

	struct winhost *host = calloc(1, sizeof(*host));
	if (host == NULL)
		return;
	host->server = server;
	host->fd = -1;
	host->listen_fd = -1;
	wl_list_init(&host->windows);

	const char *path = winhost_socket_path();
	unlink(path);
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		free(host);
		return;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0
		|| listen(fd, 4) != 0) {
		wlr_log(WLR_ERROR, "winhost: cannot listen on %s: %s", path,
			strerror(errno));
		close(fd);
		free(host);
		return;
	}
	host->listen_fd = fd;
	server->winhost = host;
	wlr_log(WLR_INFO, "winhost: enabled, socket %s", path);
}

void
winhost_finish(struct vitrine_server *server)
{
	struct winhost *host = server->winhost;
	if (host == NULL)
		return;
	if (host->fd >= 0) {
		winhost_send(host, WH_HOST_QUIT, 0, NULL, 0);
		if (host->source != NULL)
			wl_event_source_remove(host->source);
		close(host->fd);
	}
	if (host->listen_fd >= 0)
		close(host->listen_fd);
	unlink(winhost_socket_path());
	free(host);
	server->winhost = NULL;
}

struct vitrine_hosted_window *
winhost_create_window(struct vitrine_server *server, const BeWindowSpec *spec)
{
	struct winhost *host = server->winhost;
	if (host == NULL || spec == NULL || spec->w <= 0 || spec->h <= 0)
		return NULL;

	if (host->fd < 0 && winhost_spawn(host) != 0)
		return NULL;

	struct vitrine_hosted_window *hosted = calloc(1, sizeof(*hosted));
	if (hosted == NULL)
		return NULL;

	hosted->win_id = spec->win_id;
	hosted->width = spec->w;
	hosted->height = spec->h;
	hosted->stride = spec->w * 4;
	hosted->host = host;
	wl_list_init(&hosted->old_areas);

	char name[64];
	snprintf(name, sizeof(name), "vitrine fb %d", spec->win_id);
	hosted->area = create_area(name, &hosted->bits, B_ANY_ADDRESS,
		(size_t)hosted->stride * spec->h, B_NO_LOCK,
		B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (hosted->area < 0) {
		wlr_log(WLR_ERROR, "winhost: create_area: %s",
			strerror(hosted->area));
		free(hosted);
		return NULL;
	}
	memset(hosted->bits, 0, (size_t)hosted->stride * spec->h);

	struct wh_create msg = {
		.x = spec->x, .y = spec->y,
		.w = spec->w, .h = spec->h,
		.stride = hosted->stride,
		.area = (int32_t)hosted->area,
		.resizable = spec->resizable,
		.borderless = spec->borderless,
	};
	snprintf(msg.title, sizeof(msg.title), "%s",
		spec->title != NULL ? spec->title : "");

	if (winhost_send(host, WH_WIN_CREATE, spec->win_id, &msg,
			sizeof(msg)) != 0) {
		delete_area(hosted->area);
		free(hosted);
		return NULL;
	}
	wl_list_insert(&host->windows, &hosted->link);
	return hosted;
}

void
winhost_destroy_window(struct vitrine_hosted_window *hosted)
{
	if (hosted == NULL)
		return;
	if (hosted->host != NULL)
		winhost_send(hosted->host, WH_WIN_DESTROY, hosted->win_id,
			NULL, 0);
	struct winhost_old_area *old, *tmp;
	wl_list_for_each_safe(old, tmp, &hosted->old_areas, link) {
		wl_list_remove(&old->link);
		delete_area(old->area);
		free(old);
	}
	wl_list_remove(&hosted->link);
	if (hosted->area >= 0)
		delete_area(hosted->area);
	free(hosted);
}

void
winhost_send_damage(struct vitrine_hosted_window *hosted, int x, int y,
	int w, int h)
{
	if (hosted == NULL || hosted->host == NULL)
		return;
	struct wh_rect rect = { x, y, w, h };
	winhost_send(hosted->host, WH_WIN_DAMAGE, hosted->win_id, &rect,
		sizeof(rect));
}

/* ---- H2 window operations ---- */

void
winhost_move_window(struct vitrine_hosted_window *hosted, int x, int y)
{
	if (hosted == NULL || hosted->host == NULL)
		return;
	struct wh_move msg = { x, y };
	winhost_send(hosted->host, WH_WIN_MOVE, hosted->win_id, &msg,
		sizeof(msg));
}

int
winhost_resize_window(struct vitrine_hosted_window *hosted, int w, int h)
{
	if (hosted == NULL || hosted->host == NULL || w <= 0 || h <= 0)
		return -1;
	struct winhost *host = hosted->host;
	if (host->fd < 0)
		return -1;

	struct winhost_old_area *old = calloc(1, sizeof(*old));
	if (old == NULL)
		return -1;

	char name[64];
	snprintf(name, sizeof(name), "vitrine fb %d", hosted->win_id);
	void *bits = NULL;
	int32_t stride = w * 4;
	area_id area = create_area(name, &bits, B_ANY_ADDRESS,
		(size_t)stride * h, B_NO_LOCK,
		B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (area < 0) {
		wlr_log(WLR_ERROR, "winhost: resize create_area: %s",
			strerror(area));
		free(old);
		return -1;
	}
	memset(bits, 0, (size_t)stride * h);

	struct wh_resize msg = { w, h, stride, (int32_t)area };
	if (winhost_send(host, WH_WIN_RESIZE, hosted->win_id, &msg,
			sizeof(msg)) != 0) {
		/* Dropped (queue full / helper dead): stay at the old size —
		 * the caller must not resize the wlr output either. */
		delete_area(area);
		free(old);
		return -1;
	}

	/* Flip the compositor side immediately: SEQPACKET ordering guarantees
	 * the helper processes the swap before any damage we send afterwards.
	 * The superseded source area is parked until RESIZE_DONE. */
	old->area = hosted->area;
	wl_list_insert(hosted->old_areas.prev, &old->link);
	hosted->area = area;
	hosted->bits = bits;
	hosted->stride = stride;
	hosted->width = w;
	hosted->height = h;
	return 0;
}

void
winhost_set_title(struct vitrine_hosted_window *hosted, const char *title)
{
	if (hosted == NULL || hosted->host == NULL)
		return;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", title != NULL ? title : "");
	winhost_send(hosted->host, WH_WIN_SET_TITLE, hosted->win_id, buf,
		(uint32_t)strlen(buf) + 1);
}

void
winhost_set_limits(struct vitrine_hosted_window *hosted, int min_w, int min_h,
	int max_w, int max_h)
{
	if (hosted == NULL || hosted->host == NULL)
		return;
	struct wh_limits msg = { min_w, min_h, max_w, max_h };
	winhost_send(hosted->host, WH_WIN_SET_LIMITS, hosted->win_id, &msg,
		sizeof(msg));
}

void
winhost_activate(struct vitrine_hosted_window *hosted)
{
	if (hosted == NULL || hosted->host == NULL)
		return;
	winhost_send(hosted->host, WH_WIN_ACTIVATE, hosted->win_id, NULL, 0);
}

void
winhost_send_behind(struct vitrine_hosted_window *hosted,
	struct vitrine_hosted_window *behind_of)
{
	if (hosted == NULL || behind_of == NULL || hosted->host == NULL)
		return;
	if (hosted->host != behind_of->host)
		return;	/* different helper teams cannot restack (app_server
			 * SendBehind is same-team); cannot happen while H1
			 * runs a single helper */
	struct wh_behind msg = { behind_of->win_id };
	winhost_send(hosted->host, WH_WIN_SEND_BEHIND, hosted->win_id, &msg,
		sizeof(msg));
}
