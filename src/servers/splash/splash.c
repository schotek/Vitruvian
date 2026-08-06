/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * vitruvian-splash — minimal DRM boot splash in the BeOS/Haiku spirit:
 * takes over the display as soon as the KMS driver is up, shows the
 * Vitruvian logo with a row of stage icons that light up as the boot
 * progresses, and quietly steps aside once app_server owns the screen.
 *
 * Design notes:
 *  - The DRM master is dropped right after the initial modeset. Pixels
 *    keep flowing because scanout reads the dumb buffer directly, and
 *    janus/libseat/app_server can take the device over at any moment
 *    without fighting us for the master.
 *  - Progress arrives on /run/vos/splash.ctl (a fifo): "stage N",
 *    "quit", "text". Writers are fire-and-forget (janus); a missing
 *    reader must never block the boot.
 *  - If no command arrives for FAILSAFE_SEC the boot is assumed wedged:
 *    the VT is switched back to text so the console is usable again.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "logo_rgba.h"

#define FIFO_PATH		"/run/vos/splash.ctl"
#define TTY_PATH		"/dev/tty1"
#define FAILSAFE_SEC	90
#define NSTAGES			5

/* Steel blue used by the desktop background — visual continuity with
 * the screen app_server paints right after us. */
#define COL_BG			0xFF336698u
#define COL_BOX_DIM		0xFF2A5580u
#define COL_BOX_LIT		0xFFF5F5F5u
#define COL_GLYPH_DIM	0xFF3F6C9Eu
#define COL_GLYPH_LIT	0xFF336698u

static volatile sig_atomic_t sTerm = 0;

struct output {
	int			fd;
	uint32_t	crtc_id;
	uint32_t	conn_id;
	drmModeModeInfo mode;
	uint32_t	fb_id;
	uint32_t	handle;
	uint32_t	pitch;
	uint64_t	size;
	uint8_t*	map;
	int			w, h;
};

static void
on_term(int sig)
{
	(void)sig;
	sTerm = 1;
}

/* ---- pixel helpers ---------------------------------------------------- */

static inline void
put_px(struct output* o, int x, int y, uint32_t argb)
{
	if (x < 0 || y < 0 || x >= o->w || y >= o->h)
		return;
	*(uint32_t*)(o->map + (size_t)y * o->pitch + (size_t)x * 4) = argb;
}

static void
fill_rect(struct output* o, int x, int y, int w, int h, uint32_t argb)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			put_px(o, i, j, argb);
}

static void
fill_round_rect(struct output* o, int x, int y, int w, int h, int r,
	uint32_t argb)
{
	for (int j = 0; j < h; j++) {
		for (int i = 0; i < w; i++) {
			int dx = 0, dy = 0;
			if (i < r && j < r)			{ dx = r - i; dy = r - j; }
			else if (i >= w - r && j < r)		{ dx = i - (w - r - 1); dy = r - j; }
			else if (i < r && j >= h - r)		{ dx = r - i; dy = j - (h - r - 1); }
			else if (i >= w - r && j >= h - r)	{ dx = i - (w - r - 1); dy = j - (h - r - 1); }
			if (dx * dx + dy * dy > r * r)
				continue;
			put_px(o, x + i, y + j, argb);
		}
	}
}

static void
fill_disc(struct output* o, int cx, int cy, int r, uint32_t argb)
{
	for (int j = -r; j <= r; j++)
		for (int i = -r; i <= r; i++)
			if (i * i + j * j <= r * r)
				put_px(o, cx + i, cy + j, argb);
}

static void
blit_logo(struct output* o, int cx, int cy)
{
	int x0 = cx - (int)kLogoW / 2;
	int y0 = cy - (int)kLogoH / 2;
	for (unsigned j = 0; j < kLogoH; j++) {
		for (unsigned i = 0; i < kLogoW; i++) {
			const unsigned char* p = kLogoRGBA + (j * kLogoW + i) * 4;
			unsigned a = p[3];
			if (a == 0)
				continue;
			int x = x0 + (int)i, y = y0 + (int)j;
			if (x < 0 || y < 0 || x >= o->w || y >= o->h)
				continue;
			uint32_t* dst = (uint32_t*)(o->map + (size_t)y * o->pitch
				+ (size_t)x * 4);
			uint32_t bg = *dst;
			unsigned br = (bg >> 16) & 0xff, bgc = (bg >> 8) & 0xff,
				bb = bg & 0xff;
			unsigned r = (p[0] * a + br * (255 - a)) / 255;
			unsigned g = (p[1] * a + bgc * (255 - a)) / 255;
			unsigned b = (p[2] * a + bb * (255 - a)) / 255;
			*dst = 0xFF000000u | (r << 16) | (g << 8) | b;
		}
	}
}

/* Linear blend between two ARGB colors, t = 0 (a) … 255 (b). */
static uint32_t
lerp_argb(uint32_t a, uint32_t b, unsigned t)
{
	unsigned r = (((a >> 16) & 0xff) * (255 - t) + ((b >> 16) & 0xff) * t) / 255;
	unsigned g = (((a >> 8) & 0xff) * (255 - t) + ((b >> 8) & 0xff) * t) / 255;
	unsigned bl = ((a & 0xff) * (255 - t) + (b & 0xff) * t) / 255;
	return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

/* Tiny stage glyphs, drawn from primitives so no artwork is needed:
 * 0 disk, 1 gears, 2 folder, 3 window, 4 rocket. `bg` is the box color
 * behind the glyph (inner accents are punched out with it). */
static void
draw_glyph(struct output* o, int idx, int x, int y, int s, uint32_t col,
	uint32_t bg)
{
	int u = s / 8;
	switch (idx) {
		case 0:	/* disk */
			fill_round_rect(o, x + u, y + 2 * u, 6 * u, 4 * u, u, col);
			fill_disc(o, x + 4 * u, y + 4 * u, u, bg);
			break;
		case 1:	/* gears */
			fill_disc(o, x + 3 * u, y + 3 * u, 2 * u, col);
			fill_disc(o, x + 5 * u, y + 5 * u, 2 * u, col);
			fill_disc(o, x + 3 * u, y + 3 * u, u / 2 + 1, bg);
			fill_disc(o, x + 5 * u, y + 5 * u, u / 2 + 1, bg);
			break;
		case 2:	/* folder */
			fill_rect(o, x + u, y + 2 * u, 3 * u, u, col);
			fill_round_rect(o, x + u, y + 3 * u, 6 * u, 3 * u, u / 2 + 1, col);
			break;
		case 3:	/* window with tab */
			fill_rect(o, x + u, y + 2 * u, 3 * u, u, col);
			fill_rect(o, x + u, y + 3 * u, 6 * u, 3 * u, col);
			break;
		case 4:	/* rocket */
			for (int j = 0; j < 4 * u; j++) {
				int half = (j * 2 * u) / (4 * u) + 1;
				fill_rect(o, x + 4 * u - half, y + u + j, 2 * half, 1, col);
			}
			fill_rect(o, x + 2 * u, y + 5 * u, u, u, col);
			fill_rect(o, x + 5 * u, y + 5 * u, u, u, col);
			break;
	}
}

/* Eight-dot spinner below the icon row — continuous proof of life while
 * slow one-off init work (cold font scan…) holds the desktop back.
 * Positions are (cos,sin) in 45° steps, scaled by 1000. */
static const int kSpinPos[8][2] = {
	{1000, 0}, {707, 707}, {0, 1000}, {-707, 707},
	{-1000, 0}, {-707, -707}, {0, -1000}, {707, -707},
};

static void
draw_spinner(struct output* o, int frame)
{
	int box = o->h / 12;
	if (box < 40)
		box = 40;
	int r = box / 4;
	int cx = o->w / 2;
	/* Centered between the bottom edge of the icon row and the bottom
	 * of the screen. */
	int cy = ((int)(o->h * 0.58) + box + o->h) / 2;
	int dot = box / 20 + 1;

	fill_rect(o, cx - r - dot - 2, cy - r - dot - 2,
		2 * (r + dot + 2), 2 * (r + dot + 2), COL_BG);

	for (int i = 0; i < 8; i++) {
		int x = cx + (kSpinPos[i][0] * r) / 1000;
		int y = cy + (kSpinPos[i][1] * r) / 1000;
		int d = (i - frame) & 7;	/* 0 = head, 7 = faintest tail */
		unsigned t = 255 - (unsigned)d * 30;
		/* blend white over COL_BG by t */
		unsigned br = (0x33 * (255 - t) + 0xF5 * t) / 255;
		unsigned bg = (0x66 * (255 - t) + 0xF5 * t) / 255;
		unsigned bb = (0x98 * (255 - t) + 0xF5 * t) / 255;
		fill_disc(o, x, y, d == 0 ? dot + 1 : dot,
			0xFF000000u | (br << 16) | (bg << 8) | bb);
	}
}

static void
icon_pos(struct output* o, int i, int* x, int* y, int* boxOut)
{
	int box = o->h / 12;
	if (box < 40)
		box = 40;
	int gap = box / 2;
	int total = NSTAGES * box + (NSTAGES - 1) * gap;
	*x = (o->w - total) / 2 + i * (box + gap);
	*y = (int)(o->h * 0.58);
	*boxOut = box;
}

/* One icon box at blend position t: 0 = dim, 255 = lit. */
static void
draw_icon(struct output* o, int i, unsigned t)
{
	int x, y, box;
	icon_pos(o, i, &x, &y, &box);
	uint32_t bc = lerp_argb(COL_BOX_DIM, COL_BOX_LIT, t);
	uint32_t gc = lerp_argb(COL_GLYPH_DIM, COL_GLYPH_LIT, t);
	fill_round_rect(o, x, y, box, box, box / 6, bc);
	draw_glyph(o, i, x, y, box, gc, bc);
}

/* The icon of the stage in progress breathes: triangle wave between dim
 * and lit. While waiting for the desktop's takeover (all stages lit)
 * the last icon keeps breathing near its lit color. */
static void
draw_pulse(struct output* o, int lit, int frame)
{
	int idx = (lit < NSTAGES) ? lit : NSTAGES - 1;
	int ph = frame & 15;
	unsigned t = (ph < 8) ? ph * 32 : (15 - ph) * 32;
	if (t > 255)
		t = 255;
	if (lit >= NSTAGES)
		t = 255 - t / 3;	/* a lit icon only dips, never goes dark */
	draw_icon(o, idx, t);
}

static void
draw(struct output* o, int lit)
{
	fill_rect(o, 0, 0, o->w, o->h, COL_BG);
	blit_logo(o, o->w / 2, (int)(o->h * 0.40));

	for (int i = 0; i < NSTAGES; i++)
		draw_icon(o, i, (i < lit) ? 255 : 0);
}

/* PSR panels re-read the framebuffer only when damage is announced;
 * without this the animation freezes on the first frame (seen on
 * i915 laptop eDP). Master-only ioctl; drivers with an always-running
 * scanout (bochs…) just return an error we can ignore. */
static void
flush_damage(struct output* o, int master)
{
	if (!master)
		return;
	drmModeClip clip = { 0, 0,
		(unsigned short)o->w, (unsigned short)o->h };
	drmModeDirtyFB(o->fd, o->fb_id, &clip, 1);
}

/* ---- DRM bring-up ----------------------------------------------------- */

static int
card_is_boot_vga(int idx)
{
	char path[128];
	char c = 0;
	snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/boot_vga", idx);
	FILE* f = fopen(path, "r");
	if (f == NULL)
		return 0;
	if (fread(&c, 1, 1, f) != 1)
		c = 0;
	fclose(f);
	return c == '1';
}

static drmModeConnector*
find_connector(int fd, drmModeRes* res)
{
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
		if (conn == NULL)
			continue;
		if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
			return conn;
		drmModeFreeConnector(conn);
	}
	return NULL;
}

/* Prefer the firmware-chosen GPU (boot_vga) — on dual-GPU laptops the
 * first enumerated card is not necessarily the one with the panel. */
static int
open_card(void)
{
	int fallback = -1;
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i <= 9; i++) {
			if (pass == 0 && !card_is_boot_vga(i))
				continue;
			char path[64];
			snprintf(path, sizeof(path), "/dev/dri/card%d", i);
			int fd = open(path, O_RDWR | O_CLOEXEC);
			if (fd < 0)
				continue;
			drmModeRes* res = drmModeGetResources(fd);
			if (res != NULL) {
				drmModeConnector* conn = find_connector(fd, res);
				if (conn != NULL) {
					drmModeFreeConnector(conn);
					drmModeFreeResources(res);
					return fd;
				}
				drmModeFreeResources(res);
			}
			if (pass == 1 && fallback < 0)
				fallback = fd;
			else
				close(fd);
		}
	}
	return fallback;
}

static int
setup_output(struct output* o)
{
	drmModeRes* res = drmModeGetResources(o->fd);
	if (res == NULL)
		return -1;
	drmModeConnector* conn = find_connector(o->fd, res);
	if (conn == NULL) {
		drmModeFreeResources(res);
		return -1;
	}
	o->conn_id = conn->connector_id;
	o->mode = conn->modes[0];
	o->w = o->mode.hdisplay;
	o->h = o->mode.vdisplay;

	o->crtc_id = 0;
	if (conn->encoder_id != 0) {
		drmModeEncoder* enc = drmModeGetEncoder(o->fd, conn->encoder_id);
		if (enc != NULL) {
			o->crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);
		}
	}
	if (o->crtc_id == 0) {
		for (int i = 0; i < conn->count_encoders && o->crtc_id == 0; i++) {
			drmModeEncoder* enc = drmModeGetEncoder(o->fd, conn->encoders[i]);
			if (enc == NULL)
				continue;
			for (int c = 0; c < res->count_crtcs; c++) {
				if (enc->possible_crtcs & (1 << c)) {
					o->crtc_id = res->crtcs[c];
					break;
				}
			}
			drmModeFreeEncoder(enc);
		}
	}
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	if (o->crtc_id == 0)
		return -1;

	struct drm_mode_create_dumb creq;
	memset(&creq, 0, sizeof(creq));
	creq.width = o->w;
	creq.height = o->h;
	creq.bpp = 32;
	if (drmIoctl(o->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		return -1;
	o->handle = creq.handle;
	o->pitch = creq.pitch;
	o->size = creq.size;

	if (drmModeAddFB(o->fd, o->w, o->h, 24, 32, o->pitch, o->handle,
			&o->fb_id) != 0)
		return -1;

	struct drm_mode_map_dumb mreq;
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = o->handle;
	if (drmIoctl(o->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
		return -1;
	o->map = mmap(NULL, o->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		o->fd, mreq.offset);
	if (o->map == MAP_FAILED)
		return -1;

	if (drmModeSetCrtc(o->fd, o->crtc_id, o->fb_id, 0, 0, &o->conn_id, 1,
			&o->mode) != 0)
		return -1;
	return 0;
}

/* ---- main ------------------------------------------------------------- */

int
main(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	struct output o;
	memset(&o, 0, sizeof(o));
	/* The KMS driver may still be probing when we are started (there
	 * is no systemd device unit for DRM cards to order against), so
	 * poll for a usable card instead of failing on the first try. */
	o.fd = -1;
	for (int i = 0; i < 100 && !sTerm; i++) {
		o.fd = open_card();
		if (o.fd >= 0)
			break;
		usleep(200 * 1000);
	}
	if (o.fd < 0) {
		fprintf(stderr, "splash: no usable DRM device\n");
		return 1;
	}
	if (setup_output(&o) != 0) {
		fprintf(stderr, "splash: modeset failed\n");
		return 1;
	}

	/* Silence fbcon so it stops repainting the text console over us. */
	int tty = open(TTY_PATH, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (tty >= 0)
		ioctl(tty, KDSETMODE, KD_GRAPHICS);

	int lit = 1;
	draw(&o, lit);

	/* We keep the DRM master while animating: on panels with self
	 * refresh (i915 PSR on laptop eDP) the display stops re-reading
	 * the framebuffer, and only a master may send the DIRTYFB damage
	 * notifications that wake it up. The master is released on the
	 * "handoff" command (sent by app_server right before it claims
	 * the display) or, at the latest, on "quit". */
	int master = 1;

	mkdir("/run/vos", 0755);
	unlink(FIFO_PATH);
	if (mkfifo(FIFO_PATH, 0622) != 0 && errno != EEXIST)
		fprintf(stderr, "splash: mkfifo: %s\n", strerror(errno));
	/* mkfifo's mode goes through the umask (022 under systemd), which
	 * silently strips the write bits — app_server runs unprivileged
	 * and its handoff/quit would never get through. */
	chmod(FIFO_PATH, 0622);
	/* O_RDWR keeps the fifo open across writer generations (no EOF churn). */
	int ctl = open(FIFO_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC);

	time_t last = time(NULL);
	char buf[256];
	size_t fill = 0;
	int restore_text = 0;
	int frame = 0;

	while (!sTerm) {
		draw_spinner(&o, frame);
		draw_pulse(&o, lit, frame);
		frame++;
		struct pollfd pfd = { .fd = ctl, .events = POLLIN };
		int pr = poll(&pfd, 1, 90);
		if (pr > 0 && (pfd.revents & POLLIN)) {
			ssize_t n = read(ctl, buf + fill, sizeof(buf) - 1 - fill);
			if (n > 0) {
				fill += (size_t)n;
				buf[fill] = '\0';
				char* line = buf;
				char* nl;
				while ((nl = strchr(line, '\n')) != NULL) {
					*nl = '\0';
					last = time(NULL);
					if (strncmp(line, "stage ", 6) == 0) {
						int s = atoi(line + 6);
						if (s > lit && s <= NSTAGES) {
							lit = s;
							draw(&o, lit);
						}
					} else if (strcmp(line, "handoff") == 0) {
						/* app_server is about to claim the display. */
						fprintf(stderr, "splash: handoff received "
							"(master=%d)\n", master);
						if (master) {
							drmDropMaster(o.fd);
							master = 0;
						}
					} else if (strcmp(line, "quit") == 0) {
						// Sent by app_server after its first modeset:
						// our framebuffer is off the CRTC already, so
						// exiting now cannot black out the screen.
						goto out;
					} else if (strcmp(line, "text") == 0) {
						restore_text = 1;
						goto out;
					}
					line = nl + 1;
				}
				fill = strlen(line);
				memmove(buf, line, fill + 1);
			}
		}
		flush_damage(&o, master);
		if (time(NULL) - last > FAILSAFE_SEC) {
			/* No signal for a long time. With the chain incomplete the
			 * boot is wedged — hand the console back. With every stage
			 * lit the desktop is up (just an old app_server without
			 * the quit notification) — leave silently instead of
			 * flashing fbcon over a healthy desktop. */
			if (lit < NSTAGES) {
				fprintf(stderr, "splash: no progress for %ds, "
					"restoring text console\n", FAILSAFE_SEC);
				restore_text = 1;
			}
			break;
		}
	}
	if (sTerm)
		restore_text = 1;

out:
	if (tty >= 0) {
		if (restore_text)
			ioctl(tty, KDSETMODE, KD_TEXT);
		close(tty);
	}
	unlink(FIFO_PATH);
	/* Closing the fd destroys our fb; app_server has its own by now. */
	close(o.fd);
	return 0;
}
