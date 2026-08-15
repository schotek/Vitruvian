/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Vitrine output: one wlr_output whose scanout is a BBitmap inside an
 * app_server BWindow. The renderer composites into a data-ptr buffer;
 * the commit hook copies the damaged rows into the BBitmap and asks the
 * shim to blit them (cross-thread DrawBitmapAsync).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <drm_fourcc.h>
#include <pixman.h>

#include <wlr/interfaces/wlr_output.h>
#include <wlr/util/log.h>

#include "vitrine.h"

static struct vitrine_output *
output_from_wlr(struct wlr_output *wlr_output)
{
	struct vitrine_output *output =
		wl_container_of(wlr_output, output, base);
	return output;
}

/* Frame pacing (see vitrine.h): fixed-cadence heartbeat re-armed from its
 * own handler. Re-arming here — not from the commit hook — means a tick
 * with no damage (wlr_scene_output_commit early-outs) still schedules the
 * next tick, so frame callbacks never starve and clients like foot or
 * QtWebEngine don't freeze after their first frame. */
static int
output_handle_frame_timer(void *data)
{
	struct vitrine_output *output = data;
	wlr_output_send_frame(&output->base);
	wl_event_source_timer_update(output->frame_timer, output->refresh_ms);
	return 0;
}

/* Compositor-side frame handler (wlr_output.events.frame, emitted by the
 * timer above): render the scene, then wake clients waiting on
 * wl_surface.frame callbacks — unconditionally, commit or not. */
static void
output_handle_frame(struct wl_listener *listener, void *data)
{
	struct vitrine_output *output =
		wl_container_of(listener, output, frame);

	if (output->scene_output == NULL)
		return;

	wlr_scene_output_commit(output->scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(output->scene_output, &now);
}

/* Scanout seam (H0): the two spots where pixels leave the compositor for
 * the BeOS window. Today the destination is always the in-process beshim
 * window; the per-window helper (H1+) adds a second branch here — a shared
 * nexus area written in place of Bits() plus a WIN_DAMAGE message in place
 * of the blit call. Keeping the branch point in exactly two helpers means
 * output_commit stays oblivious to who hosts the window. */
static uint8_t *
scanout_bits(struct vitrine_output *output, int *stride)
{
	return beshim_window_bits(output->window, stride);
}

static void
scanout_blit(struct vitrine_output *output, int x, int y, int w, int h)
{
	beshim_blit(output->window, x, y, w, h);
}

static void
blit_box(struct vitrine_output *output, const uint8_t *src, size_t src_stride,
	uint8_t *dst, size_t dst_stride, int width, int height,
	int x1, int y1, int x2, int y2)
{
	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 > width)
		x2 = width;
	if (y2 > height)
		y2 = height;
	if (x1 >= x2 || y1 >= y2)
		return;

	/* XRGB8888 rows byte-for-byte; B_RGB32 ignores the high byte, and in
	 * rootful mode the scene background rect makes every output pixel
	 * opaque anyway, so ARGB8888 can take the same path. (The per-window
	 * matte blend for genuinely translucent content is a rootless/F3
	 * concern.) */
	size_t row_bytes = (size_t)(x2 - x1) * 4;
	const uint8_t *s = src + (size_t)y1 * src_stride + (size_t)x1 * 4;
	uint8_t *d = dst + (size_t)y1 * dst_stride + (size_t)x1 * 4;
	for (int y = y1; y < y2; y++) {
		memcpy(d, s, row_bytes);
		s += src_stride;
		d += dst_stride;
	}

	scanout_blit(output, x1, y1, x2 - x1, y2 - y1);
}

static bool
output_commit(struct wlr_output *wlr_output,
	const struct wlr_output_state *state)
{
	struct vitrine_output *output = output_from_wlr(wlr_output);

	/* Enable/mode-only commits carry no buffer — nothing to display.
	 * Virtual (metadata-only) outputs have no window to blit into. */
	if (!(state->committed & WLR_OUTPUT_STATE_BUFFER)
			|| output->window == NULL)
		return true;

	void *data;
	uint32_t format;
	size_t src_stride;
	if (!wlr_buffer_begin_data_ptr_access(state->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format,
			&src_stride)) {
		wlr_log(WLR_ERROR, "output commit: buffer has no data-ptr access");
		return false;
	}

	if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) {
		wlr_log(WLR_ERROR,
			"output commit: unsupported buffer format 0x%08x", format);
		wlr_buffer_end_data_ptr_access(state->buffer);
		return false;
	}

	int dst_stride = 0;
	uint8_t *dst = scanout_bits(output, &dst_stride);
	if (dst == NULL) {
		wlr_buffer_end_data_ptr_access(state->buffer);
		return false;
	}

	int width = wlr_output->width;
	int height = wlr_output->height;
	if (state->buffer->width < width)
		width = state->buffer->width;
	if (state->buffer->height < height)
		height = state->buffer->height;

	if (state->committed & WLR_OUTPUT_STATE_DAMAGE) {
		int nrects = 0;
		const pixman_box32_t *rects = pixman_region32_rectangles(
			(pixman_region32_t *)&state->damage, &nrects);
		for (int i = 0; i < nrects; i++) {
			blit_box(output, data, src_stride, dst,
				(size_t)dst_stride, width, height,
				rects[i].x1, rects[i].y1,
				rects[i].x2, rects[i].y2);
		}
	} else {
		/* No damage info supplied — treat as full damage. */
		blit_box(output, data, src_stride, dst, (size_t)dst_stride,
			width, height, 0, 0, width, height);
	}

	wlr_buffer_end_data_ptr_access(state->buffer);
	return true;
}

static bool
output_test(struct wlr_output *wlr_output,
	const struct wlr_output_state *state)
{
	return true;
}

static void
output_destroy(struct wlr_output *wlr_output)
{
	struct vitrine_output *output = output_from_wlr(wlr_output);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->link);
	if (output->frame_timer != NULL)
		wl_event_source_remove(output->frame_timer);
	if (output->window != NULL)
		beshim_destroy_window(output->window);
	free(output);
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_destroy,
	.test = output_test,
	.commit = output_commit,
};

/* Shared init: `window` may be NULL (virtual metadata output — commits are
 * accepted but nothing is blitted, no frame timer needed but harmless). */
static struct vitrine_output *
output_create_common(struct vitrine_server *server, BeWindow *window,
	int width, int height)
{
	struct vitrine_backend *backend = server->backend;

	struct vitrine_output *output = calloc(1, sizeof(*output));
	if (output == NULL)
		return NULL;
	output->server = server;
	output->refresh_ms = 16; /* ~60 Hz heartbeat */
	output->window = window;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, width, height, 60000);
	wlr_output_init(&output->base, &backend->base, &output_impl,
		server->event_loop, &state);
	wlr_output_state_finish(&state);

	/* Must happen before the output is advertised: wl_output v4 sends a
	 * name event, and a NULL name fails to marshal — libwayland then
	 * drops every client on bind. Names must be unique per output. */
	static unsigned output_num = 0;
	char name[32];
	snprintf(name, sizeof(name), "VITRINE-%u", ++output_num);
	wlr_output_set_name(&output->base, name);
	wlr_output_set_description(&output->base, "Vitrine output");

	if (!wlr_output_init_render(&output->base, server->allocator,
			server->renderer)) {
		wlr_log(WLR_ERROR, "wlr_output_init_render failed");
		wlr_output_destroy(&output->base);
		return NULL;
	}

	output->frame_timer = wl_event_loop_add_timer(server->event_loop,
		output_handle_frame_timer, output);

	output->frame.notify = output_handle_frame;
	wl_signal_add(&output->base.events.frame, &output->frame);

	wl_list_insert(&backend->outputs, &output->link);

	if (backend->started)
		wl_event_source_timer_update(output->frame_timer, 1);

	return output;
}

struct vitrine_output *
vitrine_output_create(struct vitrine_server *server, int width, int height,
	const char *title)
{
	BeWindow *window = beshim_create_window(server->shim, width, height,
		title);
	if (window == NULL) {
		wlr_log(WLR_ERROR, "beshim_create_window(%dx%d) failed",
			width, height);
		return NULL;
	}
	struct vitrine_output *output = output_create_common(server, window,
		width, height);
	if (output == NULL)
		beshim_destroy_window(window);
	return output;
}

struct vitrine_output *
vitrine_output_create_from_window(struct vitrine_server *server,
	BeWindow *window, int width, int height)
{
	return output_create_common(server, window, width, height);
}

struct vitrine_output *
vitrine_output_create_virtual(struct vitrine_server *server, int width,
	int height)
{
	struct vitrine_output *output = output_create_common(server, NULL,
		width, height);
	if (output != NULL) {
		/* Metadata-only output: advertised to clients (geometry/scale
		 * hints) but never rendered to — no scene_output is attached. */
		wlr_output_create_global(&output->base, server->display);
	}
	return output;
}

/* Compositor-side mode change (window was resized): the renderer swapchain
 * follows the mode on the next frame; our commit hook sees a modeset-only
 * commit (no buffer) and skips the blit. */
void
vitrine_output_resize(struct vitrine_output *output, int width, int height)
{
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, width, height, 60000);
	if (!wlr_output_commit_state(&output->base, &state))
		wlr_log(WLR_ERROR, "output resize %dx%d failed", width, height);
	wlr_output_state_finish(&state);
}
