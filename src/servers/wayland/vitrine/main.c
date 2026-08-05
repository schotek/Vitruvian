/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Vitrine — nested Wayland compositor for VitruvianOS. Renders into
 * app_server BWindows through the bewindow shim and (from phase 4) hosts
 * rootless XWayland for legacy X11 applications.
 *
 * Phase 1: rootful first light — one desktop-sized BWindow is the single
 * output; wl_shm clients composite via wlr_scene + pixman.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/render/pixman.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>
#include <wlr/version.h>

#include "vitrine.h"

#define VITRINE_VERSION "0.2.0"

int
main(int argc, char** argv)
{
	static struct vitrine_server server = {0};

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0) {
			printf("vitrine %s (wlroots %s)\n", VITRINE_VERSION,
				WLR_VERSION_STR);
			return 0;
		} else if (strcmp(argv[i], "--rootful") == 0) {
			server.rootful = true;
		} else {
			fprintf(stderr, "usage: vitrine [--rootful] [--version]\n");
			return 1;
		}
	}

	wlr_log_init(getenv("VITRINE_DEBUG") != NULL ? WLR_DEBUG : WLR_INFO,
		NULL);

	/* The renderer choice is load-bearing (P0 spike): our backend only
	 * advertises DATA_PTR|SHM, which autocreate maps to pixman — the env
	 * pin is belt and braces, kept overridable for experiments. */
	setenv("WLR_RENDERER", "pixman", 0);

	/* Direct scanout MUST stay off: a rootless toplevel exactly covers
	 * its internal output, so wlr_scene would commit the client's own
	 * wlr_client_buffer — which forbids begin_data_ptr_access — and the
	 * blit to the BBitmap fails on every frame (verified live). We always
	 * need the composited swapchain buffer. Revisit with dmabuf (F8). */
	setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);

	server.shim = beshim_start("application/x-vnd.vos-Vitrine");
	if (server.shim == NULL) {
		fprintf(stderr, "vitrine: cannot reach app_server "
			"(beshim_start failed)\n");
		return 1;
	}

	server.display = wl_display_create();
	server.event_loop = wl_display_get_event_loop(server.display);

	server.backend = vitrine_backend_create(&server);
	if (server.backend == NULL) {
		fprintf(stderr, "vitrine: backend creation failed\n");
		goto err_shim;
	}

	server.renderer = wlr_renderer_autocreate(&server.backend->base);
	if (server.renderer == NULL) {
		fprintf(stderr, "vitrine: wlr_renderer_autocreate failed "
			"(P0 spike FAILED — vendor-wlroots fallback applies)\n");
		goto err_shim;
	}
	wlr_log(WLR_INFO, "P0 spike: pixman renderer = %s",
		wlr_renderer_is_pixman(server.renderer) ? "YES" : "NO (!)");
	wlr_renderer_init_wl_display(server.renderer, server.display);

	server.allocator = wlr_allocator_autocreate(&server.backend->base,
		server.renderer);
	if (server.allocator == NULL) {
		fprintf(stderr, "vitrine: wlr_allocator_autocreate failed\n");
		goto err_shim;
	}

	server.compositor = wlr_compositor_create(server.display, 5,
		server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);
	/* xterm's mouse-select copy goes to PRIMARY, not CLIPBOARD — without
	 * this manager the middle-click paste path can never work (F7). */
	wlr_primary_selection_v1_device_manager_create(server.display);

	server.output_layout = wlr_output_layout_create(server.display);
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene,
		server.output_layout);

	vitrine_shell_init(&server);

	/* Keyboard/pointer capabilities arrive with phase 2; advertising an
	 * empty seat keeps clients that hard-require wl_seat happy. */
	server.seat = wlr_seat_create(server.display, "seat0");

	int screen_w = 0, screen_h = 0;
	if (beshim_screen_size(server.shim, &screen_w, &screen_h) != 0
			|| screen_w <= 0 || screen_h <= 0) {
		wlr_log(WLR_ERROR,
			"beshim_screen_size failed — defaulting to 1024x768");
		screen_w = 1024;
		screen_h = 768;
	}

	if (server.rootful) {
		/* Single desktop-sized window, everything in the global scene.
		 * BeOS desktop grey under it all: every output pixel opaque,
		 * which lets the blit treat ARGB and XRGB identically. */
		wlr_scene_rect_create(&server.scene->tree, screen_w, screen_h,
			(float[4]){ 0.85f, 0.85f, 0.85f, 1.0f });

		server.rootful_output = vitrine_output_create(&server,
			screen_w, screen_h, "Vitrine");
		if (server.rootful_output == NULL) {
			fprintf(stderr,
				"vitrine: cannot create the rootful output\n");
			goto err_shim;
		}

		struct wlr_output_layout_output *l_output =
			wlr_output_layout_add_auto(server.output_layout,
				&server.rootful_output->base);
		server.rootful_output->scene_output = wlr_scene_output_create(
			server.scene, &server.rootful_output->base);
		wlr_scene_output_layout_add_output(server.scene_layout,
			l_output, server.rootful_output->scene_output);
	} else {
		/* Rootless (default): per-toplevel windows come from
		 * rootless.c; here only the mode switch + one desktop-sized
		 * metadata wl_output global for clients. */
		vitrine_rootless_init(&server);
		vitrine_popup_init(&server);
		server.virtual_output = vitrine_output_create_virtual(&server,
			screen_w, screen_h);
		if (server.virtual_output == NULL) {
			fprintf(stderr,
				"vitrine: cannot create the virtual output\n");
			goto err_shim;
		}
	}

	vitrine_input_init(&server);

	/* CLIPBOARD ⇄ BClipboard text bridge (F7). */
	vitrine_clipboard_init(&server);

	/* Lazy XWayland (phase 4): the Xwayland process only spawns when the
	 * first X client connects. Rootless policy lives in xwayland.c. */
	if (!server.rootful)
		vitrine_xwayland_init(&server);

	/* Deterministic socket name (wayland-0) so the static session env
	 * works; fall back to auto allocation if it is taken. */
	const char *socket = "wayland-0";
	if (wl_display_add_socket(server.display, socket) != 0) {
		socket = wl_display_add_socket_auto(server.display);
		if (socket == NULL) {
			fprintf(stderr, "vitrine: cannot create a wayland "
				"socket (XDG_RUNTIME_DIR unset or unusable?)\n");
			goto err_shim;
		}
	}

	if (!wlr_backend_start(&server.backend->base)) {
		fprintf(stderr, "vitrine: backend start failed\n");
		goto err_shim;
	}

	wlr_log(WLR_INFO, "vitrine %s up: WAYLAND_DISPLAY=%s, output %dx%d",
		VITRINE_VERSION, socket, screen_w, screen_h);
	wl_display_run(server.display);

	/* Teardown order is load-bearing (the generalized UnrealizeWindow
	 * lesson): Xwayland first (it is a wayland client of ours AND owns a
	 * child process), then the other clients, then scene, then
	 * renderer/allocator/backend, then the display; the shim
	 * (BApplication thread) goes last. */
	vitrine_xwayland_destroy(&server);
	wl_display_destroy_clients(server.display);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(&server.backend->base);
	wl_display_destroy(server.display);
	beshim_shutdown(server.shim);
	return 0;

err_shim:
	beshim_shutdown(server.shim);
	return 1;
}
