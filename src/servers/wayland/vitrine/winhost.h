/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * winhost.h — compositor side of the per-window helper (phase H1).
 * Gated by VITRINE_WINHOST=1; with the gate off nothing here runs and the
 * in-process beshim path is used unchanged (it stays compiled in forever
 * as the fallback).
 */
#ifndef VITRINE_WINHOST_H
#define VITRINE_WINHOST_H

#include <stdbool.h>
#include <stdint.h>

#include "bewindow.h"

struct vitrine_server;

/* A guest window hosted in a helper process: the compositor's proxy. The
 * shared framebuffer stays mapped in the compositor (bits/stride) — the
 * pixman path writes it exactly like a BBitmap's Bits(). `area` is a nexus
 * area_id; kept as a plain int32 here so this header stays free of BeOS
 * kernel headers (the compositor core is plain C). */
struct vitrine_hosted_window {
	int win_id;
	int32_t area;
	void *bits;
	int stride;
	int width, height;
	struct winhost *host;		/* NULL after the helper died */
};

bool winhost_enabled(struct vitrine_server *server);
void winhost_init(struct vitrine_server *server);
void winhost_finish(struct vitrine_server *server);

struct vitrine_hosted_window *winhost_create_window(
	struct vitrine_server *server, const BeWindowSpec *spec);
void winhost_destroy_window(struct vitrine_hosted_window *hosted);
void winhost_send_damage(struct vitrine_hosted_window *hosted,
	int x, int y, int w, int h);

#endif	/* VITRINE_WINHOST_H */
