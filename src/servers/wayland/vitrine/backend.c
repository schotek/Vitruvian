/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Custom wlroots backend: no hardware, no session — outputs are
 * app_server BWindows (see output.c), input comes from the bewindow
 * shim's self-pipe. Modeled line-by-line on wlroots' headless backend,
 * which is the reference for a DATA_PTR-only software backend.
 */

#include <stdlib.h>

#include <wlr/backend/interface.h>
#include <wlr/util/log.h>

#include "vitrine.h"

static struct vitrine_backend *backend_from_wlr(struct wlr_backend *wlr_backend);

static bool
backend_start(struct wlr_backend *wlr_backend)
{
	struct vitrine_backend *backend = backend_from_wlr(wlr_backend);
	backend->started = true;

	/* First frame per output: without this initial kick wlr_scene never
	 * renders and no client ever gets a frame callback. */
	struct vitrine_output *output;
	wl_list_for_each(output, &backend->outputs, link)
		wl_event_source_timer_update(output->frame_timer, 1);

	wlr_log(WLR_INFO, "vitrine backend started (%d output(s))",
		wl_list_length(&backend->outputs));
	return true;
}

static void
backend_destroy(struct wlr_backend *wlr_backend)
{
	struct vitrine_backend *backend = backend_from_wlr(wlr_backend);

	struct vitrine_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &backend->outputs, link)
		wlr_output_destroy(&output->base);

	wlr_backend_finish(wlr_backend);
	free(backend);
}

static int
backend_get_drm_fd(struct wlr_backend *wlr_backend)
{
	return -1;
}

static uint32_t
backend_get_buffer_caps(struct wlr_backend *wlr_backend)
{
	/* DATA_PTR steers wlr_renderer_autocreate to the pixman renderer and
	 * the allocator to CPU-accessible buffers; SHM admits client wl_shm
	 * buffers. No DMABUF — there is no GPU path (yet). */
	return WLR_BUFFER_CAP_DATA_PTR | WLR_BUFFER_CAP_SHM;
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_drm_fd = backend_get_drm_fd,
	.get_buffer_caps = backend_get_buffer_caps,
};

static struct vitrine_backend *
backend_from_wlr(struct wlr_backend *wlr_backend)
{
	struct vitrine_backend *backend =
		wl_container_of(wlr_backend, backend, base);
	return backend;
}

struct vitrine_backend *
vitrine_backend_create(struct vitrine_server *server)
{
	struct vitrine_backend *backend = calloc(1, sizeof(*backend));
	if (backend == NULL)
		return NULL;

	backend->server = server;
	wl_list_init(&backend->outputs);
	wlr_backend_init(&backend->base, &backend_impl);
	return backend;
}
