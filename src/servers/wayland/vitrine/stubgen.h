/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * stubgen.h — identity stubs for per-window helper teams (H3). C-callable;
 * the implementation is C++ (libbe BNode for attributes) but nothing BeOS
 * leaks through this interface.
 */
#ifndef VITRINE_STUBGEN_H
#define VITRINE_STUBGEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Build (or reuse) the identity stub for one guest app: a private copy of
 * the helper binary whose file name, signature and icon attributes give
 * the helper team its Deskbar identity. Returns a malloc'd path to spawn
 * and, via *sig_out, a malloc'd MIME signature the helper must register
 * under (both freed by the caller). NULL = no stub could be made (no
 * app_id, no writable cache): spawn the generic helper instead. */
char *vitrine_stub_for_app(const char *app_id, const char *helper_path,
	char **sig_out);

/* Startup GC: delete cached stubs whose VOS:WH_SRC does not match this
 * helper binary build — stale copies would silently host windows with
 * yesterday's code. */
void vitrine_stub_gc(const char *helper_path);

#ifdef __cplusplus
}
#endif

#endif	/* VITRINE_STUBGEN_H */
