/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * bewindow_win.cpp — the window side of the Vitrine BeOS shim: the
 * framebuffer view, the host window with all its input/echo contracts, and
 * the BeWindow operations. Split out of bewindow.cpp (H0 seam) so a future
 * per-window helper process can host the very same classes: everything here
 * talks to the compositor only through a BeEventSink (self-pipe fd + drop
 * counter + shutdown flag) instead of the full BeShim.
 *
 * The behavioural contracts (MODIFIER KEYS, BLIT STRATEGY, SELF-PIPE
 * OVERFLOW, ROOTLESS MODE, ECHO SUPPRESSION) are documented in
 * bewindow.cpp's file header and bewindow.h — the code below is a verbatim
 * move, the comments at the call sites still hold.
 */
#include "bewindow_win.h"

#include <Application.h>
#include <Window.h>
#include <View.h>
#include <Bitmap.h>
#include <Message.h>
#include <AppDefs.h>
#include <InterfaceDefs.h>
#include <OS.h>

#include <unistd.h>
#include <errno.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>

/* One record straight down the self-pipe. write() of a small struct is
 * atomic (sizeof(BeInputEvent) == 24 < PIPE_BUF), so the X reader always
 * sees whole records. Called from window looper threads; the fd is
 * O_NONBLOCK, so a full pipe drops the event instead of stalling the
 * window thread (see file header). */
void bewin_push_event(BeEventSink* sink, const BeInputEvent& ev)
{
    if (sink == nullptr || sink->pipe_w < 0)
        return;

    ssize_t n = write(sink->pipe_w, &ev, sizeof(ev));
    if (n == (ssize_t)sizeof(ev))
        return;

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        /* X server stalled; drop rather than block the window thread. */
        if ((sink->droppedEvents++ & 0xff) == 0) {
            fprintf(stderr, "vitrine: input pipe full, dropped %lld "
                "events so far\n", (long long)sink->droppedEvents);
        }
    }
    /* Short writes cannot happen for < PIPE_BUF payloads; other errors
     * (EPIPE during teardown) are intentionally ignored. */
}

/* ------------------------------------------------------------------ */
/* Framebuffer view + window                                          */
/* ------------------------------------------------------------------ */

// Draws the shared BBitmap shadow framebuffer on demand (exposes/resizes).
class XFramebufferView : public BView {
public:
    XFramebufferView(BRect frame, BBitmap* bmp, bool snoopPointer)
        : BView(frame, "xfb", B_FOLLOW_ALL, B_WILL_DRAW),
          fBitmap(bmp), fSnoopPointer(snoopPointer) {}

    void AttachedToWindow() override {
        // Never let app_server pre-fill our area with a background color:
        // every visible pixel comes from the bitmap, so a background fill
        // would only cause flicker on updates.
        SetViewColor(B_TRANSPARENT_COLOR);
        // Focus so B_(UNMAPPED_)KEY_* messages target this window/view
        // while it is the active window.
        MakeFocus(true);
        if (fSnoopPointer) {
            // Override-redirect windows (open X menus/popups): permanently
            // snoop pointer events desktop-wide. Unlike the per-click
            // temporary mask in MouseDown() below, SetEventMask installs a
            // PERMANENT EventDispatcher listener (View.cpp:1779-1802 sends
            // AS_VIEW_SET_EVENT_MASK; ServerWindow.cpp:1327-1345 ->
            // EventDispatcher().AddListener). Required so X pointer grabs
            // (open menus) see clicks/motion outside our windows
            // (bewindow.h contract); duplicate deliveries are harmless
            // because button events carry the full mask. Legal here:
            // AttachedToWindow runs with the window locked and fOwner set,
            // which SetEventMask needs to reach app_server.
            SetEventMask(B_POINTER_EVENTS, 0);
        }
    }

    void Draw(BRect update) override {
        // Expose/resize repaint: blit only the damaged region. Source and
        // destination rects are identical — bitmap pixels map 1:1 to view
        // pixels.
        if (fBitmap != nullptr)
            DrawBitmapAsync(fBitmap, update, update);
    }

    void MouseDown(BPoint where) override {
        (void)where;
        // While any button is held, keep receiving B_MOUSE_MOVED and the
        // final B_MOUSE_UP even when the pointer leaves the window — X
        // clients rely on drag-outside/release-outside semantics. This
        // must be called while processing B_MOUSE_DOWN
        // (View.cpp SetMouseEventMask checks CurrentMessage()); app_server
        // installs a temporary listener that expires on mouse-up
        // (ServerWindow.cpp AS_VIEW_SET_MOUSE_EVENT_MASK ->
        // EventDispatcher::AddTemporaryListener). We deliberately do NOT
        // use the permanent SetEventMask(B_POINTER_EVENTS): that would
        // snoop pointer events for the whole desktop even when the cursor
        // is over unrelated BeOS windows.
        SetMouseEventMask(B_POINTER_EVENTS, 0);
    }

    // Called with the window locked (window thread during resize swap).
    void SetBitmap(BBitmap* bmp) { fBitmap = bmp; }

private:
    BBitmap* fBitmap;
    bool     fSnoopPointer;   /* override-redirect: desktop-wide snoop */
};

// Top-level window; forwards BeOS input to the self-pipe as BeInputEvent's.
// Serves both modes: rootful (the single screen window, token = screen
// index) and rootless (one instance per top-level X window, token =
// spec->win_id). The token is echoed as BeInputEvent.screen in every event.
class XHostWindow : public BWindow {
public:
    XHostWindow(BRect frame, const char* title, window_look look,
                window_feel feel, uint32 flags, BeEventSink* sink,
                XFramebufferView* view, int token,
                int width, int height, bool rootless)
        : BWindow(frame, title, look, feel, flags),
          fSink(sink), fView(view), fToken(token),
          fWidth(width), fHeight(height),
          fRootless(rootless),
          /* Seed the echo-suppression state with the creation geometry so
           * the initial B_WINDOW_MOVED/RESIZED notifications app_server
           * sends while placing the new window are swallowed too. */
          fExpectedX((int)frame.left), fExpectedY((int)frame.top),
          fExpectedW(width), fExpectedH(height),
          fWheelRemainderX(0.0f), fWheelRemainderY(0.0f) {}

    // All input interception lives here. task_looper has already run
    // _SanitizeMessage() (Window.cpp:3038-3052), so mouse messages carry
    // "be:view_where" when targeted at a view. Key messages are consumed;
    // mouse messages are also forwarded to BWindow::DispatchMessage so
    // default bookkeeping (activation, transit tracking, view MouseDown ->
    // SetMouseEventMask) keeps working.
    void DispatchMessage(BMessage* msg, BHandler* target) override {
        /* XVITRUVIAN_DEBUG=1: trace every key transition that reaches the
         * window, for diagnosing lost releases (stuck-autorepeat class). */
        static int debugKeys = -1;
        if (debugKeys < 0)
            debugKeys = getenv("XVITRUVIAN_DEBUG") != nullptr
                || getenv("VITRINE_DEBUG") != nullptr;
        if (debugKeys
            && (msg->what == B_MOUSE_DOWN || msg->what == B_MOUSE_UP)) {
            int32 dbgButtons = -1;
            msg->FindInt32("buttons", &dbgButtons);
            fprintf(stderr, "beshim: %s buttons=0x%x win=%d\n",
                msg->what == B_MOUSE_DOWN ? "MOUSE_DOWN" : "MOUSE_UP",
                (unsigned)dbgButtons, fToken);
        }
        if (debugKeys
            && (msg->what == B_KEY_DOWN || msg->what == B_KEY_UP
                || msg->what == B_UNMAPPED_KEY_DOWN
                || msg->what == B_UNMAPPED_KEY_UP)) {
            int32 dbgKey = -1;
            msg->FindInt32("key", &dbgKey);
            fprintf(stderr, "beshim: key msg what=0x%" B_PRIx32
                " key=%" B_PRId32 " win=%d\n",
                msg->what, dbgKey, fToken);
        }
        switch (msg->what) {
            case B_KEY_DOWN:
            case B_UNMAPPED_KEY_DOWN:
                emitKey(msg, true);
                return;     // consumed: keep BWindow shortcuts out of X
            case B_KEY_UP:
            case B_UNMAPPED_KEY_UP:
                emitKey(msg, false);
                return;
            case B_MODIFIERS_CHANGED:
                // Pure state notification; the per-key transitions already
                // arrived as B_(UNMAPPED_)KEY_* (see file header). Let the
                // default path see it (harmless) and emit nothing.
                break;
            case B_MOUSE_MOVED:
                emitMotion(msg, target);
                break;
            case B_MOUSE_DOWN:
                emitButton(msg, true);
                break;
            case B_MOUSE_UP:
                emitButton(msg, false);
                break;
            case B_MOUSE_WHEEL_CHANGED:
                emitWheel(msg);
                return;     // no useful default handling for a plain view
            default:
                break;
        }
        BWindow::DispatchMessage(msg, target);
    }

    bool QuitRequested() override {
        if (atomic_get(&fSink->shuttingDown) != 0) {
            // beshim_shutdown() in progress: BApplication::QuitRequested ->
            // _QuitAllWindows() polls every window; consent so the app can
            // actually quit.
            return true;
        }
        // User clicked the close box: forward as a close request so the X
        // side can hand it to the session (WM_DELETE_WINDOW semantics);
        // never tear the window down underneath the X server.
        BeInputEvent ev = BeInputEvent();
        ev.type = BE_INPUT_WINDOW;
        ev.code = BE_WINDOW_CLOSE;
        ev.screen = fToken;
        bewin_push_event(fSink, ev);
        return false;
    }

    /* The three hooks below run on this window's looper thread from within
     * the message dispatch (B_WINDOW_MOVED/RESIZED/ACTIVATED cases,
     * Window.cpp:1007-1100), i.e. with the window locked by task_looper —
     * so reading fExpected* here is serialized against the X thread, which
     * writes those fields inside LockLooper() (see file header). The
     * BWindow base hooks are empty (Window.cpp:1344-1356), so not chaining
     * up loses nothing. Rootful contract: CLOSE plus FOCUS_IN/OUT are
     * emitted (bewindow.h BeWindowEvent docs) — MOVED/RESIZED stay gated on
     * fRootless (the rootful screen window is fixed-size and its desktop
     * position is meaningless to X). */

    void FrameMoved(BPoint newPosition) override {
        if (!fRootless)
            return;
        /* app_server echoes programmatic MoveTo()s too (see file header).
         * MoveTo() rounds its arguments (Window.cpp:2397-2398) and the
         * desktop only ever moves by integer deltas, so the frame origin is
         * integral and this compare is exact. */
        int x = (int)newPosition.x;
        int y = (int)newPosition.y;
        if (x == fExpectedX && y == fExpectedY)
            return;     /* programmatic echo or duplicate notification */
        fExpectedX = x;
        fExpectedY = y;
        BeInputEvent ev = BeInputEvent();
        ev.type = BE_INPUT_WINDOW;
        ev.code = BE_WINDOW_MOVED;
        ev.x = x;
        ev.y = y;
        ev.screen = fToken;
        bewin_push_event(fSink, ev);
    }

    void FrameResized(float newWidth, float newHeight) override {
        if (!fRootless)
            return;
        /* The hook arguments are FRAME units, i.e. pixels - 1: the server
         * attaches frame.IntegerWidth()/IntegerHeight() (server
         * Window.cpp:391-395) and the dispatch passes them through
         * unchanged (Window.cpp:1010-1045). Convert to content pixels for
         * the X side. */
        int w = (int)newWidth + 1;
        int h = (int)newHeight + 1;
        if (w == fExpectedW && h == fExpectedH)
            return;     /* programmatic echo or duplicate notification */
        fExpectedW = w;
        fExpectedH = h;
        BeInputEvent ev = BeInputEvent();
        ev.type = BE_INPUT_WINDOW;
        ev.code = BE_WINDOW_RESIZED;
        ev.x = w;
        ev.y = h;
        ev.screen = fToken;
        bewin_push_event(fSink, ev);
    }

    void WindowActivated(bool active) override {
        /* Driven by the desktop focus change: server Window::Activated()
         * sends B_WINDOW_ACTIVATED (server Window.cpp:1050-1055), dispatch
         * dedupes pending activations and calls this hook only on actual
         * state flips (Window.cpp:1082-1100). Emitted in BOTH modes: in
         * rootless the DDX maps it to X focus; in either mode FOCUS_OUT
         * makes the DDX release all held X keys — once we are deactivated
         * the B_KEY_UP for a held key goes to the newly focused window,
         * never to us, and xkb soft autorepeat would repeat the stuck key
         * forever (bewindow.h BeWindowEvent docs). */
        BeInputEvent ev = BeInputEvent();
        ev.type = BE_INPUT_WINDOW;
        ev.code = active ? BE_WINDOW_FOCUS_IN : BE_WINDOW_FOCUS_OUT;
        ev.screen = fToken;
        bewin_push_event(fSink, ev);
    }

    // Called with the window locked (resize swap on the X thread).
    void SetContentSize(int width, int height) {
        fWidth = width;
        fHeight = height;
    }

    /* Echo-suppression state; called by the X thread inside LockLooper()
     * immediately BEFORE the programmatic MoveTo/ResizeTo, because the
     * B_WINDOW_MOVED/RESIZED echo arrives asynchronously on the window
     * thread after the lock is released (see file header). */
    void SetExpectedPosition(int x, int y) {
        fExpectedX = x;
        fExpectedY = y;
    }
    void SetExpectedSize(int w, int h) {
        fExpectedW = w;
        fExpectedH = h;
    }

private:
    static int clampi(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    void emitKey(BMessage* msg, bool down) {
        // app_server hands us the raw Linux evdev keycode in "key"
        // (KeyboardInputDevice.cpp:924). X keycode = evdev + 8, which the
        // xkb "evdev" ruleset already expects, so pass it straight through.
        int32 key = 0;
        if (msg->FindInt32("key", &key) != B_OK)
            return;
        BeInputEvent ev = BeInputEvent();
        ev.type = BE_INPUT_KEY;
        ev.code = key;
        ev.buttons = down ? 1 : 0;
        ev.screen = fToken;
        bewin_push_event(fSink, ev);

        /* Follow up with the physical key-state bitmap so the X side can
         * reconcile lost releases (bewindow.h BE_INPUT_KEY_STATES docs).
         * Every B_(UNMAPPED_)KEY_* message carries the input_server's
         * "states" array — 16 bytes, added unconditionally in
         * KeyboardInputDevice.cpp:926 — reflecting the state AFTER this
         * event. The 16 bytes are packed over the contiguous
         * code/x/y/buttons int32 fields of the POD wire struct. */
        const void* states = NULL;
        ssize_t statesSize = 0;
        if (msg->FindData("states", B_UINT8_TYPE, &states, &statesSize)
                == B_OK && statesSize == 16) {
            BeInputEvent st = BeInputEvent();
            st.type = BE_INPUT_KEY_STATES;
            st.screen = fToken;
            memcpy(&st.code, states, 16);
            bewin_push_event(fSink, st);
        }
    }

    void emitMotion(BMessage* msg, BHandler* target) {
        // "be:view_where" is view-local and can be negative / beyond the
        // frame while dragging outside the window (temporary pointer
        // grab). It is only attached when the dispatch target is a BView
        // (Window.cpp:3465); fall back to window-local "where" otherwise —
        // our view sits at (0,0), so window coordinates equal view
        // coordinates.
        BPoint where;
        if (msg->FindPoint("be:view_where", &where) != B_OK
            && msg->FindPoint("where", &where) != B_OK)
            return;
        (void)target;

        BeInputEvent ev = BeInputEvent();
        ev.type = BE_INPUT_MOTION;
        if (fRootless) {
            // Rootless contract: desktop-absolute coordinates.
            // "be:view_where" was derived from the message's desktop
            // "screen_where" via ConvertFromScreen (_SanitizeMessage,
            // Window.cpp:3444-3465) — this holds for events snooped from
            // other windows through the override-redirect SetEventMask
            // listener too — so converting back through the view is exact.
            // Legal here: DispatchMessage runs on the window thread with
            // the window locked, and fView is attached. Do NOT clamp: the
            // DDX clamps to the X screen bounds.
            BPoint screen = fView->ConvertToScreen(where);
            ev.x = (int)floorf(screen.x);
            ev.y = (int)floorf(screen.y);
        } else {
            // Rootful: window-local framebuffer pixels, clamped into the
            // framebuffer (dragging outside the window yields coordinates
            // beyond the frame).
            ev.x = clampi((int)floorf(where.x), 0, fWidth - 1);
            ev.y = clampi((int)floorf(where.y), 0, fHeight - 1);
        }
        ev.screen = fToken;
        bewin_push_event(fSink, ev);
    }

    void emitButton(BMessage* msg, bool down) {
        int32 buttons = 0;
        msg->FindInt32("buttons", &buttons);
        // B_MOUSE_UP reports the buttons still held *after* release on
        // some paths; the X side tracks transitions from the full mask,
        // so just pass the mask + direction through unchanged.
        BeInputEvent ev = BeInputEvent();
        ev.type = BE_INPUT_BUTTON;
        ev.buttons = buttons;
        ev.code = down ? 1 : 0;
        ev.screen = fToken;
        bewin_push_event(fSink, ev);
    }

    void emitWheel(BMessage* msg) {
        float dy = 0.0f, dx = 0.0f;
        msg->FindFloat("be:wheel_delta_y", &dy);
        msg->FindFloat("be:wheel_delta_x", &dx);
        // Deltas are floats (typically ±1.0 per notch, but high-resolution
        // wheels can send fractions). Accumulate remainders so slow
        // fractional scrolling is not lost to truncation.
        fWheelRemainderX += dx;
        fWheelRemainderY += dy;
        int stepsX = (int)truncf(fWheelRemainderX);
        int stepsY = (int)truncf(fWheelRemainderY);
        if (stepsX == 0 && stepsY == 0)
            return;
        fWheelRemainderX -= (float)stepsX;
        fWheelRemainderY -= (float)stepsY;

        BeInputEvent ev = BeInputEvent();
        ev.type = BE_INPUT_WHEEL;
        ev.x = stepsX;
        ev.y = stepsY;
        ev.screen = fToken;
        bewin_push_event(fSink, ev);
    }

    BeEventSink*      fSink;
    XFramebufferView* fView;      /* owned by BWindow; outlives all hooks */
    int     fToken;               /* echoed as BeInputEvent.screen: screen
                                   * index (rootful) / win_id (rootless)  */
    int     fWidth, fHeight;      /* framebuffer size, for clamping */
    bool    fRootless;            /* snapshot of shim->rootless     */
    /* Echo suppression (see file header): X thread writes under
     * LockLooper(), hooks read with the window locked by task_looper. */
    int     fExpectedX, fExpectedY;   /* frame left/top, desktop coords */
    int     fExpectedW, fExpectedH;   /* content size in pixels         */
    float   fWheelRemainderX, fWheelRemainderY;
};

struct BeWindow {
    XHostWindow*      window = nullptr;
    XFramebufferView* view = nullptr;
    BBitmap*          bitmap = nullptr;
    BeEventSink*      sink = nullptr;
    int               width = 0, height = 0;
};

BeWindow* bewin_create_screen(BeEventSink* sink, int w, int h,
                              const char* title)
{
    if (!sink || w <= 0 || h <= 0)
        return nullptr;

    BeWindow* be = new BeWindow();
    be->sink = sink;
    be->width = w;
    be->height = h;

    BRect frame(0, 0, w - 1, h - 1);
    /* B_RGB32 shadow framebuffer: X renders here, we blit it to the view.
     * Non-view-accepting (flat) bitmap => Bits() is a plain linear buffer. */
    be->bitmap = new BBitmap(frame, B_RGB32, false);
    if (be->bitmap->InitCheck() != B_OK || be->bitmap->Bits() == nullptr) {
        delete be->bitmap;
        delete be;
        return nullptr;
    }

    /* Created from the X thread: legal, be_app exists (beshim_start
     * handshake). Show() spawns the window's own looper thread.
     * B_TITLED_WINDOW_LOOK + B_NORMAL_WINDOW_FEEL is exactly what the old
     * B_TITLED_WINDOW type decomposed to (Window.cpp:3141-3145). */
    be->view = new XFramebufferView(frame, be->bitmap,
                                    /*snoopPointer*/ false);
    be->window = new XHostWindow(frame.OffsetToCopy(100, 100), title,
                                 B_TITLED_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
                                 B_NOT_ZOOMABLE
                                     | B_NOT_RESIZABLE /* phase 1: fixed size */,
                                 sink, be->view, /*token: screen index*/ 0,
                                 w, h, /*rootless*/ false);
    be->window->AddChild(be->view);
    be->window->Show();
    return be;
}

BeWindow* bewin_create_x(BeEventSink* sink, const BeWindowSpec* spec)
{
    if (!sink || !spec || spec->w <= 0 || spec->h <= 0)
        return nullptr;

    BRect content(0, 0, spec->w - 1, spec->h - 1);
    /* Same flat B_RGB32 shadow framebuffer as the rootful path. */
    BBitmap* bitmap = new BBitmap(content, B_RGB32, false);
    if (bitmap->InitCheck() != B_OK || bitmap->Bits() == nullptr) {
        delete bitmap;
        return nullptr;
    }
    return bewin_create_x_with_bitmap(sink, spec, bitmap);
}

/* Assembly half of bewin_create_x, taking a caller-supplied framebuffer —
 * the per-window helper wraps a BBitmap around a cloned nexus area and
 * hands it in here. Ownership transfers: the BeWindow deletes it. */
BeWindow* bewin_create_x_with_bitmap(BeEventSink* sink,
                                     const BeWindowSpec* spec,
                                     BBitmap* bitmap)
{
    if (!sink || !spec || !bitmap)
        return nullptr;

    BeWindow* be = new BeWindow();
    be->sink = sink;
    be->width = spec->w;
    be->height = spec->h;
    be->bitmap = bitmap;

    BRect content(0, 0, spec->w - 1, spec->h - 1);

    /* Look/feel/flags per the bewindow.h contract; all constants verified
     * in headers/os/interface/Window.h:36-70. */
    window_look look;
    uint32 flags;
    if (spec->override_redirect) {
        /* X menus/tooltips: undecorated, never become the focus window —
         * B_AVOID_FOCUS is enforced by the desktop when it picks a focus
         * candidate (Desktop.cpp:2068, :3117) — and immune to user WM
         * interference. The view additionally snoops desktop-wide pointer
         * events (snoopPointer below, see XFramebufferView). */
        look = B_NO_BORDER_WINDOW_LOOK;
        flags = B_AVOID_FOCUS | B_NOT_MOVABLE | B_NOT_CLOSABLE
            | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_RESIZABLE;
    } else if (spec->borderless) {
        /* CSD Wayland toplevel: the client paints its own decorations, so
         * no BeOS tab — but it is a normal, focusable window (contrast
         * with override_redirect above). Movable/resizable stays enabled:
         * the compositor drives both programmatically from the client's
         * interactive move/resize requests. */
        look = B_NO_BORDER_WINDOW_LOOK;
        flags = B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE;
        if (!spec->resizable)
            flags |= B_NOT_RESIZABLE;
    } else {
        look = B_TITLED_WINDOW_LOOK;
        /* Zoom/minimize have no X-side plumbing yet; resizability follows
         * what the X window advertises (WM_NORMAL_HINTS, decided by the
         * DDX). */
        flags = B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE;
        if (!spec->resizable)
            flags |= B_NOT_RESIZABLE;
    }

    /* BWindow's frame is the CONTENT area in desktop coordinates — the
     * decorator lies entirely outside fFrame (server Window.cpp:209-217:
     * the full region is the border region plus fFrame) — so spec->x/y map
     * 1:1 to X window geometry, no decoration offset needed. */
    be->view = new XFramebufferView(content, be->bitmap,
        /*snoopPointer*/ spec->override_redirect != 0);
    be->window = new XHostWindow(content.OffsetToCopy(spec->x, spec->y),
                                 spec->title != nullptr ? spec->title : "",
                                 look, B_NORMAL_WINDOW_FEEL, flags,
                                 sink, be->view, spec->win_id,
                                 spec->w, spec->h, /*rootless*/ true);
    be->window->AddChild(be->view);
    be->window->Show();
    return be;
}

void bewin_set_title(BeWindow* win, const char* title)
{
    if (!win || !win->window)
        return;
    /* SetTitle() only locks around the app_server notification but mutates
     * fTitle before taking the lock (Window.cpp:2047-2064); wrap the whole
     * call in the looper lock so the window thread can never observe the
     * intermediate state. BWindow::SetTitle maps NULL to "" itself
     * (Window.cpp:2049-2050), but the contract pins it, so be explicit. */
    if (win->window->LockLooper()) {
        win->window->SetTitle(title != nullptr ? title : "");
        win->window->UnlockLooper();
    }
}

void bewin_move_window(BeWindow* win, int x, int y)
{
    if (!win || !win->window)
        return;
    if (win->window->LockLooper()) {
        /* No-op guard: frame origins are integral (MoveTo rounds its
         * arguments, Window.cpp:2397-2398), so this compare is exact.
         * BWindow::MoveTo would skip the app_server request itself
         * (Window.cpp:2400 compares against fFrame), but skipping here
         * also keeps the expected position untouched. */
        BRect frame = win->window->Frame();
        if ((int)frame.left != x || (int)frame.top != y) {
            /* Record the target BEFORE MoveTo: the B_WINDOW_MOVED echo is
             * asynchronous (server Window.cpp:326-330 -> client dispatch ->
             * FrameMoved) and lands on the window thread after we release
             * the lock; FrameMoved swallows it by this comparison. MoveTo
             * Lock()s again internally — recursive, same thread, fine. */
            win->window->SetExpectedPosition(x, y);
            win->window->MoveTo((float)x, (float)y);
        }
        win->window->UnlockLooper();
    }
}

void* bewin_window_bits(BeWindow* win, int* bytes_per_row)
{
    if (!win || !win->bitmap)
        return nullptr;
    if (bytes_per_row)
        *bytes_per_row = win->bitmap->BytesPerRow();
    return win->bitmap->Bits();
}

void bewin_blit(BeWindow* win, int x, int y, int w, int h)
{
    if (!win || !win->window || !win->view || w <= 0 || h <= 0)
        return;

    /* Direct cross-thread drawing (see file header for the latency
     * rationale vs. Invalidate): lock the window looper from the X thread
     * — the standard BeOS pattern for drawing from a foreign thread — and
     * stream one clipped blit of the damaged rect. */
    if (win->window->LockLooper()) {
        BRect r(x, y, x + w - 1, y + h - 1);
        win->view->DrawBitmapAsync(win->bitmap, r, r);
        win->view->Flush();
        win->window->UnlockLooper();
    }
}

/* Shared swap body: adopt `newBitmap` as the window's framebuffer and
 * resize the window to match. Always swaps — even at an unchanged size —
 * because the helper path hands in a bitmap over a fresh cloned area whose
 * predecessor is about to be deleted. Ownership transfers: the old bitmap
 * is deleted here, the new one belongs to the BeWindow afterwards. */
static void bewin_swap_framebuffer(BeWindow* win, int w, int h,
                                   BBitmap* newBitmap)
{
    BBitmap* oldBitmap = win->bitmap;
    if (win->window->LockLooper()) {
        /* Swap under the window lock: the window thread only touches the
         * bitmap (Draw()) while holding this lock, so after the swap no
         * one can still reference oldBitmap. */
        win->view->SetBitmap(newBitmap);
        win->window->SetContentSize(w, h);
        /* Programmatic resize must not echo BE_WINDOW_RESIZED (bewindow.h
         * contract): record the target before ResizeTo, same pattern as
         * beshim_move_window — the echo arrives asynchronously on the
         * window thread and FrameResized swallows it. */
        win->window->SetExpectedSize(w, h);
        win->bitmap = newBitmap;
        win->width = w;
        win->height = h;

        /* B_NOT_RESIZABLE window: pin the size limits to the new size so
         * ResizeTo() — which clamps against the min/max limits
         * (Window.cpp:2433-2443) — can't be constrained by stale limits.
         * A user-resizable rootless window must NOT be pinned (min == max
         * would make user resizing impossible); its client-side limits
         * stay at the permissive 0/32768 defaults (Window.cpp:2819-2822),
         * so ResizeTo passes through unclamped. Frame units = pixels - 1. */
        if ((win->window->Flags() & B_NOT_RESIZABLE) != 0)
            win->window->SetSizeLimits(w - 1, w - 1, h - 1, h - 1);
        win->window->ResizeTo(w - 1, h - 1);
        win->window->UnlockLooper();
    } else {
        /* Window already gone (teardown race): just adopt the new bitmap
         * so beshim_window_bits stays coherent. */
        win->bitmap = newBitmap;
        win->width = w;
        win->height = h;
    }
    delete oldBitmap;

    /* The X side calls beshim_window_bits() again after this returns; the
     * fresh bitmap content is undefined until the first full-screen blit
     * (X repaints everything after a RANDR resize). */
}

void bewin_resize_window(BeWindow* win, int w, int h)
{
    if (!win || !win->window || w <= 0 || h <= 0)
        return;
    if (w == win->width && h == win->height)
        return;

    /* Allocate the new shadow framebuffer first (unlocked — may be slow). */
    BBitmap* newBitmap = new BBitmap(BRect(0, 0, w - 1, h - 1), B_RGB32,
        false);
    if (newBitmap->InitCheck() != B_OK || newBitmap->Bits() == nullptr) {
        delete newBitmap;
        return;     /* keep the old framebuffer; X side keeps old size */
    }
    bewin_swap_framebuffer(win, w, h, newBitmap);
}

/* Helper-process resize half (H2): the caller supplies the replacement
 * framebuffer (a BBitmap over a freshly cloned nexus area). Unlike
 * bewin_resize_window there is no same-size early-out — the point is the
 * bitmap swap itself. Returns 0 once the window owns the new bitmap; on
 * failure the caller keeps ownership. */
int bewin_resize_window_with_bitmap(BeWindow* win, int w, int h,
                                    BBitmap* bitmap)
{
    if (!win || !win->window || w <= 0 || h <= 0 || bitmap == nullptr)
        return -1;
    bewin_swap_framebuffer(win, w, h, bitmap);
    return 0;
}

void bewin_destroy_window(BeWindow* win)
{
    if (!win)
        return;

    if (win->window) {
        /* BWindow::Quit() from a foreign thread: Lock() then Quit().
         * BLooper::Quit() posts _QUIT_ and waits for the window thread,
         * which deletes the BWindow and its children (our view).
         * QuitRequested() is NOT consulted on this path (Looper.cpp:578),
         * so the close-veto in XHostWindow doesn't apply. */
        if (win->window->Lock())
            win->window->Quit();
        win->window = nullptr;
        win->view = nullptr;    /* owned + deleted by the window */
    }
    delete win->bitmap;         /* nothing references it anymore */
    delete win;
}

void bewin_set_size_limits(BeWindow* win, int min_w, int min_h,
                                       int max_w, int max_h)
{
    if (!win || !win->window)
        return;

    /* Frame units = pixels - 1 (see beshim_resize_window); BeOS's
     * permissive defaults are 0/32768 (Window.cpp:2819-2822), so map
     * "no limit" onto exactly those. SetSizeLimits also re-clamps the
     * current size against the new limits itself. */
    float minW = min_w > 0 ? (float)(min_w - 1) : 0.0f;
    float minH = min_h > 0 ? (float)(min_h - 1) : 0.0f;
    float maxW = max_w > 0 ? (float)(max_w - 1) : 32768.0f;
    float maxH = max_h > 0 ? (float)(max_h - 1) : 32768.0f;

    if (win->window->LockLooper()) {
        win->window->SetSizeLimits(minW, maxW, minH, maxH);
        win->window->UnlockLooper();
    }
}

void bewin_activate(BeWindow* win)
{
    if (!win || !win->window)
        return;
    if (win->window->LockLooper()) {
        win->window->Activate(true);
        win->window->UnlockLooper();
    }
}

void bewin_send_behind(BeWindow* win, BeWindow* behind_of)
{
    if (!win || !win->window || !behind_of || !behind_of->window)
        return;
    if (win->window->LockLooper()) {
        win->window->SendBehind(behind_of->window);
        win->window->UnlockLooper();
    }
}
