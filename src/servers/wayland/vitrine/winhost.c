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
#include <ctype.h>
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
#include "stubgen.h"

#define WH_HELPER_PATH "/system/servers/vitrine_window_host"

/* One helper process = one guest app identity (H3). All toplevels with
 * the same app_id share it — BWindow::SendBehind is same-team only, so
 * dialog-over-parent stacking needs the app's windows in one team; and
 * one team is precisely one Deskbar row. app_id "" is the generic bucket
 * for clients that never set one. */
struct winhost {
	struct winhost_mgr *mgr;
	char *app_id;
	char *stub;			/* spawn path (identity stub); may be
					 * WH_HELPER_PATH for the generic bucket */
	char *sig;			/* signature to register; NULL = default */
	int fd;				/* helper connection; -1 until HELLO   */
	uint64_t token;
	pid_t pid;
	struct wl_event_source *source;
	struct wl_list windows;		/* vitrine_hosted_window.link */
	struct wl_list link;		/* winhost_mgr.helpers */
};

struct winhost_mgr {
	struct vitrine_server *server;
	int listen_fd;
	struct wl_list helpers;		/* winhost.link */
	/* Crash-storm limiter (H4): ring of the last helper crash times.
	 * Three crashes inside a minute flip `fallback` permanently — new
	 * windows then take the in-process beshim path (compiled in forever
	 * precisely for this), because a helper that keeps dying would
	 * otherwise take every guest window down with it in a loop. */
	int64_t crash_times[3];
	int crash_idx;
	bool fallback;
};

/* A framebuffer area superseded by a resize, parked until the helper acks
 * the swap (winhost.h old_areas docs). */
struct winhost_old_area {
	int32_t area;
	struct wl_list link;
};

/* Used by the respawn path in the fd handler before their definitions. */
static int winhost_spawn(struct winhost *host);
static int winhost_send(struct winhost *host, uint32_t type, uint32_t win_id,
	const void *payload, uint32_t len);

/* The user's "Show windows separately" preference: `separate_windows` in
 * ~/config/settings/vitrine (written by the VitrineSettings panel, same
 * key=value shape janus parses for `autostart`). Default OFF — the legacy
 * in-process mode stays first class. */
static bool
settings_separate_windows(void)
{
	const char *home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return false;

	char path[512];
	snprintf(path, sizeof(path), "%s/config/settings/vitrine", home);
	FILE *file = fopen(path, "r");
	if (file == NULL)
		return false;

	bool enabled = false;
	char line[256];
	while (fgets(line, sizeof(line), file) != NULL) {
		char *p = line;
		while (isspace((unsigned char)*p))
			p++;
		if (*p == '#' || strncmp(p, "separate_windows", 16) != 0)
			continue;
		p += 16;
		while (isspace((unsigned char)*p))
			p++;
		if (*p != '=')
			continue;
		p++;
		while (isspace((unsigned char)*p))
			p++;
		enabled = strncasecmp(p, "true", 4) == 0
			|| strncasecmp(p, "yes", 3) == 0
			|| strncasecmp(p, "on", 2) == 0
			|| *p == '1';
	}
	fclose(file);
	return enabled;
}

bool
winhost_enabled(struct vitrine_server *server)
{
	(void)server;
	static int enabled = -1;
	if (enabled < 0) {
		/* VITRINE_WINHOST stays as the explicit developer override in
		 * both directions; without it the user's panel setting decides. */
		const char *env = getenv("VITRINE_WINHOST");
		if (env != NULL)
			enabled = env[0] == '1' ? 1 : 0;
		else
			enabled = settings_separate_windows() ? 1 : 0;
		if (enabled && access(WH_HELPER_PATH, X_OK) != 0) {
			wlr_log(WLR_ERROR, "winhost: %s missing, gate forced off",
				WH_HELPER_PATH);
			enabled = 0;
		}
	}
	return enabled == 1;
}

/* Release one helper record. The process itself either already died
 * (hangup path) or was told to quit (empty/finish paths). */
static void
winhost_helper_free(struct winhost *host)
{
	if (host->source != NULL)
		wl_event_source_remove(host->source);
	if (host->fd >= 0)
		close(host->fd);
	wl_list_remove(&host->link);
	free(host->app_id);
	free(host->stub);
	free(host->sig);
	free(host);
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
	struct vitrine_server *server = host->mgr->server;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		wlr_log(WLR_ERROR, "winhost: helper '%s' pid %d went away",
			host->app_id, (int)host->pid);
		wl_event_source_remove(host->source);
		host->source = NULL;
		close(host->fd);
		host->fd = -1;

		if (wl_list_empty(&host->windows)) {
			winhost_helper_free(host);
			return 0;
		}

		/* Crash with live windows: respawn once, immediately, and
		 * recreate every window from its snapshot — the framebuffer
		 * areas still exist compositor-side, so the fresh helper
		 * clones them and the content is back with the first blit. */
		struct winhost_mgr *mgr = host->mgr;
		mgr->crash_times[mgr->crash_idx % 3] = (int64_t)system_time();
		mgr->crash_idx++;
		if (mgr->crash_idx >= 3) {
			int64_t oldest = mgr->crash_times[mgr->crash_idx % 3];
			if ((int64_t)system_time() - oldest < 60 * 1000000LL
					&& !mgr->fallback) {
				mgr->fallback = true;
				wlr_log(WLR_ERROR, "winhost: 3 helper crashes "
					"in a minute — new windows fall back "
					"to in-process hosting");
			}
		}
		if (mgr->fallback || winhost_spawn(host) != 0) {
			/* Husk: windows keep their mapped areas (commits render
			 * into them harmlessly) and hosted->host stays valid;
			 * the guest surfaces just have no BeOS window anymore. */
			return 0;
		}

		struct vitrine_hosted_window *hosted;
		int nwindows = 0;
		wl_list_for_each(hosted, &host->windows, link) {
			/* Areas parked for an in-flight resize ack: the only
			 * consumer died with the old team, delete them now. */
			struct winhost_old_area *old, *tmp;
			wl_list_for_each_safe(old, tmp, &hosted->old_areas,
					link) {
				wl_list_remove(&old->link);
				delete_area(old->area);
				free(old);
			}
			struct wh_create msg = {
				.x = hosted->x, .y = hosted->y,
				.w = hosted->width, .h = hosted->height,
				.stride = hosted->stride,
				.area = hosted->area,
				.resizable = hosted->resizable,
				.borderless = hosted->borderless,
			};
			memcpy(msg.title, hosted->title, sizeof(msg.title));
			winhost_send(host, WH_WIN_CREATE, hosted->win_id, &msg,
				sizeof(msg));
			winhost_send_damage(hosted, 0, 0, hosted->width,
				hosted->height);
			nwindows++;
		}
		wlr_log(WLR_INFO, "winhost: helper '%s' respawned as pid %d "
			"with %d windows", host->app_id, (int)host->pid,
			nwindows);
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

/* Spawn one helper and wait for its HELLO (blocking, bounded): spawns are
 * serialized on the event-loop thread and happen once per app, so the
 * synchronous handshake stays the simplest correct thing. */
static int
winhost_spawn(struct winhost *host)
{
	struct winhost_mgr *mgr = host->mgr;
	struct vitrine_server *server = mgr->server;

	host->token = ((uint64_t)getpid() << 32) ^ (uint64_t)system_time();

	char sockenv[300], tokenv[64], sigenv[320];
	snprintf(sockenv, sizeof(sockenv), WH_SOCKET_ENV "=%s",
		winhost_socket_path());
	snprintf(tokenv, sizeof(tokenv), WH_TOKEN_ENV "=%llu",
		(unsigned long long)host->token);
	if (host->sig != NULL)
		snprintf(sigenv, sizeof(sigenv), WH_SIG_ENV "=%s", host->sig);

	/* load_image (not fork/exec): the child needs the nexus team records
	 * registrar/app_server key on. Environment: ours plus the socket
	 * coordinates (and the stub signature, H3). */
	extern char **environ;
	int envc = 0;
	while (environ[envc] != NULL)
		envc++;
	const char **envp = calloc(envc + 4, sizeof(char *));
	if (envp == NULL)
		return -1;
	for (int i = 0; i < envc; i++)
		envp[i] = environ[i];
	int extra = envc;
	envp[extra++] = sockenv;
	envp[extra++] = tokenv;
	if (host->sig != NULL)
		envp[extra++] = sigenv;
	envp[extra] = NULL;

	const char *argv[] = { host->stub, NULL };
	thread_id team = load_image(1, argv, envp);
	free(envp);
	if (team < 0) {
		wlr_log(WLR_ERROR, "winhost: load_image(%s): %s",
			host->stub, strerror(team));
		return -1;
	}
	resume_thread(team);
	host->pid = (pid_t)team;

	/* Accept + HELLO with a 5 s budget. */
	struct pollfd pfd = { .fd = mgr->listen_fd, .events = POLLIN };
	if (poll(&pfd, 1, 5000) <= 0) {
		wlr_log(WLR_ERROR, "winhost: helper did not connect");
		return -1;
	}
	int fd = accept4(mgr->listen_fd, NULL, NULL, SOCK_CLOEXEC);
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

	struct winhost_mgr *mgr = calloc(1, sizeof(*mgr));
	if (mgr == NULL)
		return;
	mgr->server = server;
	mgr->listen_fd = -1;
	wl_list_init(&mgr->helpers);

	const char *path = winhost_socket_path();
	unlink(path);
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		free(mgr);
		return;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0
		|| listen(fd, 4) != 0) {
		wlr_log(WLR_ERROR, "winhost: cannot listen on %s: %s", path,
			strerror(errno));
		close(fd);
		free(mgr);
		return;
	}
	mgr->listen_fd = fd;
	server->winhost = mgr;
	vitrine_stub_gc(WH_HELPER_PATH);
	wlr_log(WLR_INFO, "winhost: enabled, socket %s", path);
}

void
winhost_finish(struct vitrine_server *server)
{
	struct winhost_mgr *mgr = server->winhost;
	if (mgr == NULL)
		return;
	struct winhost *host, *tmp;
	wl_list_for_each_safe(host, tmp, &mgr->helpers, link) {
		if (host->fd >= 0)
			winhost_send(host, WH_HOST_QUIT, 0, NULL, 0);
		winhost_helper_free(host);
	}
	if (mgr->listen_fd >= 0)
		close(mgr->listen_fd);
	unlink(winhost_socket_path());
	free(mgr);
	server->winhost = NULL;
}

/* The helper team hosting `app_id` — live one reused, otherwise spawned
 * from its identity stub (H3). NULL when spawning failed. */
static struct winhost *
winhost_helper_for_app(struct winhost_mgr *mgr, const char *app_id)
{
	if (app_id == NULL)
		app_id = "";

	struct winhost *host;
	wl_list_for_each(host, &mgr->helpers, link) {
		if (host->fd >= 0 && strcmp(host->app_id, app_id) == 0)
			return host;
	}

	host = calloc(1, sizeof(*host));
	if (host == NULL)
		return NULL;
	host->mgr = mgr;
	host->fd = -1;
	host->app_id = strdup(app_id);
	host->stub = vitrine_stub_for_app(app_id, WH_HELPER_PATH, &host->sig);
	if (host->stub == NULL) {
		/* No app_id or no writable cache: the generic bucket. */
		host->stub = strdup(WH_HELPER_PATH);
	}
	wl_list_init(&host->windows);
	wl_list_insert(&mgr->helpers, &host->link);
	if (host->app_id == NULL || host->stub == NULL
			|| winhost_spawn(host) != 0) {
		winhost_helper_free(host);
		return NULL;
	}
	return host;
}

struct vitrine_hosted_window *
winhost_create_window(struct vitrine_server *server, const BeWindowSpec *spec,
	const char *app_id)
{
	struct winhost_mgr *mgr = server->winhost;
	if (mgr == NULL || spec == NULL || spec->w <= 0 || spec->h <= 0)
		return NULL;
	if (mgr->fallback)
		return NULL;	/* crash storm: callers take the beshim path */

	struct winhost *host = winhost_helper_for_app(mgr, app_id);
	if (host == NULL)
		return NULL;

	struct vitrine_hosted_window *hosted = calloc(1, sizeof(*hosted));
	if (hosted == NULL)
		return NULL;

	hosted->win_id = spec->win_id;
	hosted->width = spec->w;
	hosted->height = spec->h;
	hosted->stride = spec->w * 4;
	hosted->x = spec->x;
	hosted->y = spec->y;
	hosted->resizable = spec->resizable;
	hosted->borderless = spec->borderless;
	snprintf(hosted->title, sizeof(hosted->title), "%s",
		spec->title != NULL ? spec->title : "");
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

	/* Last window of the app gone → retire its helper team, and with it
	 * the Deskbar row (H3). The next window of the same app_id spawns a
	 * fresh helper from the cached stub. */
	struct winhost *host = hosted->host;
	if (host != NULL && wl_list_empty(&host->windows)) {
		if (host->fd >= 0)
			winhost_send(host, WH_HOST_QUIT, 0, NULL, 0);
		winhost_helper_free(host);
	}
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
	hosted->x = x;
	hosted->y = y;
	struct wh_move msg = { x, y };
	winhost_send(hosted->host, WH_WIN_MOVE, hosted->win_id, &msg,
		sizeof(msg));
}

void
winhost_note_move(struct vitrine_hosted_window *hosted, int x, int y)
{
	if (hosted == NULL)
		return;
	hosted->x = x;
	hosted->y = y;
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
	snprintf(hosted->title, sizeof(hosted->title), "%s", buf);
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
