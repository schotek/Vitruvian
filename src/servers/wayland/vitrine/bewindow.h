/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * bewindow.h — pure C bridge to the BeOS/Haiku app_server client API.
 *
 * The kdrive DDX (vitruvian.c, vitruvian_input.c) is plain C and must not see
 * any C++/BeAPI types. This header is the ONLY surface it talks to; the
 * implementation (bewindow.cpp) is C++ and owns the BApplication/BWindow/
 * BView/BBitmap objects.
 *
 * Threading: beshim_start() spawns the BApplication message loop on its own
 * thread (BeOS loopers are port-based, not select()-based, so they cannot be
 * folded into the X server's select() loop). Input events observed on that
 * thread are serialized into a self-pipe whose read end the X dispatch loop
 * polls — see beshim_input_fd().
 */
#ifndef VITRUVIAN_BEWINDOW_H
#define VITRUVIAN_BEWINDOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles — real types live in bewindow.cpp. */
typedef struct BeShim   BeShim;    /* owns BApplication + its thread */
typedef struct BeWindow BeWindow;  /* one BWindow + BView + BBitmap  */

/* Wire format of one input record read from the self-pipe. Kept POD and
 * fixed-size so the X side can read() whole records. `code`/`x`/`y`/`buttons`
 * meaning depends on `type`.
 *
 * Coordinate space of BE_INPUT_MOTION differs by mode (beshim_set_rootless):
 *   rootful  — window-local framebuffer pixels (the single screen BWindow)
 *   rootless — DESKTOP-absolute pixels (BView::ConvertToScreen), because the
 *              X root coordinate system is mapped 1:1 onto the BeOS desktop */
enum BeInputType {
    BE_INPUT_KEY = 1,     /* code = evdev keycode, buttons = down?1:0        */
    BE_INPUT_MOTION,      /* x,y = absolute position (see above)             */
    BE_INPUT_BUTTON,      /* buttons = BeOS button bitmask, code = down?1:0  */
    BE_INPUT_WHEEL,       /* x,y = wheel delta (already quantized to steps)  */
    BE_INPUT_WINDOW,      /* code = BeWindowEvent (resize/close/focus/move)  */
    BE_INPUT_KEY_STATES,  /* code/x/y/buttons = 16-byte physical key bitmap  */
    BE_INPUT_CLIPBOARD,   /* system clipboard changed (no payload — consumer
                           * re-reads via beshim_clipboard_get_text)         */
    BE_INPUT_QUIT,        /* B_QUIT_REQUESTED arrived at the shim's
                           * BApplication (Deskbar tray "Quit", `hey ...
                           * quit`). No payload; the compositor answers with
                           * wl_display_terminate() so the whole process
                           * tears down through the normal main() exit path
                           * instead of the BApplication dying on its own. */
};

/* `code` values for BE_INPUT_WINDOW. CLOSE/RESIZED/MOVED are rootless-only
 * (the rootful screen window is fixed-size and CLOSE shuts the server down);
 * FOCUS_IN/OUT are emitted in BOTH modes — the X side uses FOCUS_OUT to
 * release all held keys, because a B_KEY_UP that happens after our window
 * lost focus is delivered to the newly focused window, never to us, and the
 * X server's software autorepeat (xkb/xkbAccessX.c) would otherwise repeat
 * the stuck key forever. */
enum BeWindowEvent {
    BE_WINDOW_CLOSE = 0,      /* user clicked the window close button       */
    BE_WINDOW_RESIZED = 1,    /* x,y = new width/height in pixels           */
    BE_WINDOW_FOCUS_IN = 2,   /* window was activated (BeOS focus)          */
    BE_WINDOW_FOCUS_OUT = 3,
    BE_WINDOW_MOVED = 4,      /* x,y = new frame left/top in desktop coords */
};

/* BE_INPUT_KEY_STATES: emitted immediately AFTER every BE_INPUT_KEY event.
 * The four fields code/x/y/buttons together hold the 16-byte "states" bitmap
 * that app_server attaches to every key message (the physical key state as
 * the input_server tracks it, evdev codes 0..127; bit layout
 * states[code >> 3] & (1 << (7 - (code & 7))), see
 * src/add-ons/input_server/devices/keyboard/KeyboardInputDevice.cpp:926).
 * Copy the message's 16 "states" bytes over &ev.code with memcpy — the wire
 * struct is POD and the four int32 fields are contiguous.
 *
 * The X side reconciles: any key the X server believes is down whose bit is
 * clear here gets a synthesized KeyRelease. This self-heals key state after
 * a release was lost before reaching us (QEMU/host grab transitions, BeOS
 * focus changes) — otherwise xkb soft autorepeat repeats the key forever. */

typedef struct {
    int32_t type;         /* enum BeInputType     */
    int32_t code;
    int32_t x, y;
    int32_t buttons;
    int32_t screen;       /* rootful: screen index of the emitting BWindow.
                           * rootless: the win_id token from BeWindowSpec
                           * (the DDX passes the X window's XID here), for
                           * BE_INPUT_WINDOW events. Pointer/key events are
                           * global in rootless and ignore this field.       */
} BeInputEvent;

/* ---- lifecycle ---- */

/* Create the BApplication and start its looper thread. Returns NULL on error.
 * `signature` is the app_server MIME signature, e.g. "application/x-vnd.vos-Xvitruvian". */
BeShim* beshim_start(const char* signature);

/* Read end of the self-pipe. Add this fd to the X server's fd set
 * (SetNotifyFd / AddEnabledDevice); when readable, drain it with
 * beshim_next_event() and forward to KdEnqueue*. */
int beshim_input_fd(BeShim* shim);

/* Pop one queued input event. Returns 1 on success, 0 if none pending. */
int beshim_next_event(BeShim* shim, BeInputEvent* out);

void beshim_shutdown(BeShim* shim);

/* Rootless mode switch. Call once right after beshim_start(), before any
 * window is created. In rootless mode BE_INPUT_MOTION events carry
 * desktop-absolute coordinates (see BeInputType docs). */
void beshim_set_rootless(BeShim* shim, int rootless);

/* Size of the BeOS desktop (BScreen frame) in pixels. Returns 0 on success,
 * -1 on error. Used as the X screen size in rootless mode so X root
 * coordinates == desktop coordinates. */
int beshim_screen_size(BeShim* shim, int* w, int* h);

/* ---- windows / framebuffer ---- */

/* Create a top-level window of `w`x`h` pixels backed by a B_RGB32 BBitmap that
 * serves as the X shadow framebuffer. Returns NULL on error.
 * (Rootful mode only — the single screen window; fixed size, screen id 0.) */
BeWindow* beshim_create_window(BeShim* shim, int w, int h, const char* title);

/* Rootless: one BWindow per top-level X window. */
typedef struct {
    int x, y;             /* frame left/top in desktop coordinates. BWindow's
                           * frame is the CONTENT area (decorations lie
                           * outside it), so X window geometry maps 1:1.     */
    int w, h;             /* content size in pixels                          */
    const char* title;    /* UTF-8; may be NULL (empty title)                */
    int override_redirect;/* nonzero => borderless, non-focusable popup
                           * (menu/tooltip): B_NO_BORDER_WINDOW_LOOK,
                           * B_AVOID_FOCUS, not movable/resizable by the
                           * user. While such a window exists its view must
                           * snoop desktop-wide pointer events
                           * (SetEventMask(B_POINTER_EVENTS)) so X pointer
                           * grabs (open menus) see motion/clicks outside
                           * our windows; duplicate events are harmless
                           * because button events carry the full mask.      */
    int resizable;        /* nonzero => user-resizable (emits
                           * BE_WINDOW_RESIZED); ignored for
                           * override_redirect windows                       */
    int win_id;           /* token echoed as BeInputEvent.screen in every
                           * event emitted by this window                    */
    int borderless;       /* nonzero => undecorated toplevel that stays
                           * FOCUSABLE (CSD Wayland client draws its own
                           * decorations): B_NO_BORDER_WINDOW_LOOK with
                           * normal feel — unlike override_redirect there is
                           * no B_AVOID_FOCUS and no pointer snooping.
                           * Ignored when override_redirect is set.          */
} BeWindowSpec;

/* Create a per-X-window BWindow backed by a B_RGB32 BBitmap of spec->w x
 * spec->h. The window is shown immediately (on top). Returns NULL on error. */
BeWindow* beshim_create_xwindow(BeShim* shim, const BeWindowSpec* spec);

/* Update the window title (UTF-8). Safe from the X thread. */
void beshim_set_title(BeWindow* win, const char* title);

/* Move the window frame (content area) to desktop coordinates x,y.
 * Must NOT emit BE_WINDOW_MOVED for the resulting no-op/echo FrameMoved —
 * only user-initiated moves may be reported (guard by comparing against the
 * last position set through this call). */
void beshim_move_window(BeWindow* win, int x, int y);

/* Direct pointer to the BBitmap pixel buffer (BBitmap::Bits()). *bytes_per_row
 * receives the stride. This is the linear buffer the shadow layer draws into. */
void* beshim_window_bits(BeWindow* win, int* bytes_per_row);

/* Push the given damaged rectangle to screen: BView::DrawBitmapAsync + Flush.
 * Coordinates are in framebuffer pixels. */
void beshim_blit(BeWindow* win, int x, int y, int w, int h);

/* Resize the window + swap in a freshly allocated backing BBitmap of w x h.
 * The X side must re-fetch beshim_window_bits() afterwards and repaint fully.
 * Like beshim_move_window, a programmatic resize must NOT echo a
 * BE_WINDOW_RESIZED event back (only user-initiated resizes are reported). */
void beshim_resize_window(BeWindow* win, int w, int h);
void beshim_destroy_window(BeWindow* win);

/* Constrain user resizing to the client's min/max (xdg_toplevel min/max
 * size, X WM_NORMAL_HINTS). 0 or negative max = unlimited in that axis;
 * min <= 0 means no minimum. Safe from the compositor thread. */
void beshim_set_size_limits(BeWindow* win, int min_w, int min_h,
                            int max_w, int max_h);

/* Raise + focus the window (BWindow::Activate) — X request_activate /
 * restack-to-top mapping. Safe from the compositor thread. */
void beshim_activate(BeWindow* win);

/* Stack `win` directly behind `behind_of` (BWindow::SendBehind); both must
 * be live windows. Used to keep transients above their parent. */
void beshim_send_behind(BeWindow* win, BeWindow* behind_of);

/* ---- clipboard bridge (F7) ---- */

/* Read the system clipboard's text. Returns a malloc'd UTF-8 string the
 * caller frees, or NULL when the clipboard holds no text. *from_bridge is
 * set nonzero when the clip was last written by beshim_clipboard_set_text
 * (echo guard: the bridge skips its own writes when the change
 * notification bounces back). Safe from the compositor thread. */
char* beshim_clipboard_get_text(BeShim* shim, int* from_bridge);

/* Replace the system clipboard with UTF-8 text, tagged "vos:wl-bridge" so
 * the change consumer can tell bridge writes from native ones. Returns 0
 * on success. Safe from the compositor thread. */
int beshim_clipboard_set_text(BeShim* shim, const char* text);

/* ---- acceleration hook (phase 4, currently a no-op stub) ---- */

/* Import a GPU dmabuf as this window's backing store for future GLX/DRI3
 * acceleration. Returns -1 (unsupported) today; see plan §"Cesta k akceleraci". */
int beshim_import_dmabuf(BeWindow* win, int dmabuf_fd, int stride,
                         uint32_t fourcc, uint64_t modifier);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VITRUVIAN_BEWINDOW_H */
