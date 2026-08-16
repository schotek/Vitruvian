/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * bewindow.cpp — BeOS/Haiku app_server client side of the Vitrine backend.
 *
 * C++ implementation behind the pure-C bewindow.h bridge. Owns the
 * BApplication (running on its own thread), and per-screen BWindow/BView/
 * BBitmap objects. Translates BeOS input messages into BeInputEvent records
 * written to a self-pipe that the X server's select() loop polls.
 *
 * ------------------------------------------------------------------------
 * THREAD MODEL
 *
 * beshim_start() spawns a dedicated thread that BOTH constructs the
 * BApplication AND calls Run() on it, with a semaphore handshake so
 * beshim_start() only returns once `be_app` is fully initialized (or
 * construction failed). This is the only safe shape in this codebase:
 * BLooper::_InitData() ends with gLooperList.AddLooper(this), which locks
 * the looper for the *constructing* thread (Looper.cpp:1051, fOwner set in
 * AddLooper), and BApplication::Run() -> BLooper::Loop() starts with
 * AssertLocked() (Looper.cpp:520), i.e. Run() must be called by the thread
 * that owns the construction lock. Constructing on the X thread and Run()ing
 * on a helper thread would trip the "looper must be locked" debugger() call
 * unless we did a fragile explicit Unlock()/Lock() handoff.
 *
 * Once be_app exists, BWindow/BView/BBitmap may be created from any thread
 * (the X main thread calls beshim_create_window); BWindow::Show() spawns the
 * window's own looper thread, and all input arrives on that window thread.
 * The window thread serializes input into the self-pipe with non-blocking
 * write()s, which are safe from any thread.
 *
 * MODIFIER KEYS / BLIT STRATEGY / SELF-PIPE OVERFLOW / ROOTLESS MODE /
 * ECHO SUPPRESSION: the window-side contracts moved verbatim to
 * bewindow_win.cpp together with the classes that implement them (H0
 * seam for the per-window helper); the sections are documented there.
 */
#include "bewindow.h"
#include "bewindow_win.h"

#include <Application.h>
#include <Clipboard.h>
#include <Window.h>
#include <View.h>
#include <Bitmap.h>
#include <Screen.h>
#include <Message.h>
#include <AppDefs.h>
#include <InterfaceDefs.h>
#include <OS.h>
#include <Roster.h>		/* app-flags words for VOS_APP_FLAGS_OVERRIDE */
#include <cstdlib>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdio>
#include <cstring>
#include <cmath>

/* ------------------------------------------------------------------ */
/* Input plumbing                                                     */
/* ------------------------------------------------------------------ */

struct BeShim {
    thread_id     appThread = -1;
    sem_id        startSem = -1;      /* handshake: app constructed   */
    status_t      startStatus = B_NO_INIT;
    int           pipe_r = -1;        /* X side polls this            */
    BeEventSink   sink = { -1, 0, 0 };/* window-side handle: pipe write
                                       * end + drop counter + shutdown
                                       * flag (bewindow_win.h)        */
    const char*   signature = nullptr;
    int32         rootless = 0;       /* written once before any window
                                       * exists (beshim_set_rootless
                                       * contract), read-only afterwards */
};

/* ------------------------------------------------------------------ */
/* C bridge                                                           */
/* ------------------------------------------------------------------ */

/* Forwards B_CLIPBOARD_CHANGED into the input pipe (F7 clipboard bridge).
 * Parked inside the BApplication looper; dies with it at shutdown. The
 * event carries no payload — the compositor re-reads the clipboard through
 * beshim_clipboard_get_text, which also surfaces the echo-guard tag. */
class ClipboardWatcher : public BHandler {
public:
    explicit ClipboardWatcher(BeShim* shim)
        : BHandler("clipboard_watcher"), fShim(shim) {}

    void MessageReceived(BMessage* msg) override {
        if (msg->what == B_CLIPBOARD_CHANGED) {
            BeInputEvent ev = BeInputEvent();
            ev.type = BE_INPUT_CLIPBOARD;
            bewin_push_event(&fShim->sink, ev);
        } else {
            BHandler::MessageReceived(msg);
        }
    }

private:
    BeShim* fShim;
};

/* The shim's BApplication. An EXTERNAL B_QUIT_REQUESTED (Deskbar tray
 * "Quit", scripting) must not let the BApplication die under the running
 * compositor — that would leave the wl event loop headless. Instead it is
 * forwarded down the input pipe as BE_INPUT_QUIT; the compositor answers
 * with wl_display_terminate() and the whole process exits through main()'s
 * ordinary teardown (which ends in beshim_shutdown(); that path quits the
 * looper via Quit() directly, never consulting QuitRequested —
 * Looper.cpp:578). */
class ShimApp : public BApplication {
public:
    ShimApp(BeShim* shim, status_t* error)
        : BApplication(shim->signature, error), fShim(shim) {}

    bool QuitRequested() override {
        if (atomic_get(&fShim->sink.shuttingDown) != 0)
            return BApplication::QuitRequested();
        BeInputEvent ev = BeInputEvent();
        ev.type = BE_INPUT_QUIT;
        bewin_push_event(&fShim->sink, ev);
        return false;
    }

private:
    BeShim* fShim;
};

/* Constructs the BApplication and runs its message loop. Construction and
 * Run() MUST happen on the same thread (see file header: BLooper's
 * construction lock is owned by the constructing thread and Run()
 * AssertLocked()s). The handshake semaphore is released once construction
 * succeeded or failed, so beshim_start() can report NULL on failure. */
static int32 run_app_thread(void* arg)
{
    BeShim* shim = static_cast<BeShim*>(arg);

    /* At session boot vitrine may be launched before app_server accepts
     * connections; the BApplication constructor then reports the connect
     * failure through &error. Retry for ~10 s before giving up. A failed
     * instance MUST be deleted before the next attempt: _InitData already
     * published be_app = this (Application.cpp:528) and constructing a
     * second live instance trips the "2 BApplication objects" debugger()
     * (Application.cpp:357-358); the destructor resets be_app = NULL
     * (Application.cpp:348), making the retry legal. */
    const int kMaxAttempts = 40;        /* x 250 ms ≈ 10 s */
    status_t error = B_ERROR;
    BApplication* app = nullptr;
    for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
        error = B_ERROR;
        app = new ShimApp(shim, &error);
        if (error == B_OK)
            break;
        delete app;                     /* not Run() yet: plain delete is OK
                                         * (BLooper::Quit handles !fRunCalled,
                                         * but we never registered windows) */
        app = nullptr;
        if (error == B_ALREADY_RUNNING) {
            /* B_EXCLUSIVE_LAUNCH in the rdef: the registrar refuses a
             * second instance and will keep refusing — fail fast instead
             * of burning the 10 s retry budget on it. */
            fprintf(stderr,
                "vitrine: another instance is already running\n");
            break;
        }
        if (attempt == 0) {
            fprintf(stderr, "vitrine: app_server not ready (%s), "
                "retrying for up to 10 s\n", strerror(error));
        }
        if (attempt + 1 < kMaxAttempts)
            snooze(250000);             /* 250 ms between attempts */
    }
    shim->startStatus = error;

    if (error != B_OK) {
        /* All attempts failed; report through the existing handshake so
         * beshim_start() returns NULL with the right diagnostic. */
        release_sem(shim->startSem);
        return error;
    }

    /* Clipboard change notifications (F7 bridge): AddHandler needs the
     * looper locked; be_clipboard exists once the BApplication does. */
    ClipboardWatcher* watcher = new ClipboardWatcher(shim);
    if (app->Lock()) {
        app->AddHandler(watcher);
        app->Unlock();
        be_clipboard->StartWatching(BMessenger(watcher, app));
    }

    release_sem(shim->startSem);        /* beshim_start() may return now */

    app->Run();                         /* blocks until the quit handshake  */

    /* Run() returned; the standard BeOS pattern deletes the app object here
     * (its destructor sets be_app = NULL). Guard on be_app: when
     * beshim_shutdown() had to quit a not-yet-running app, BApplication::
     * Quit() already deleted it (Application.cpp:631-632, the !fRunCalled
     * branch) and this pointer is dangling. */
    if (be_app != nullptr)
        delete app;
    return B_OK;
}

extern "C" BeShim* beshim_start(const char* signature, int background_app)
{
    BeShim* shim = new BeShim();
    shim->signature = signature;

    /* "Separate windows" mode: register this one start as a background
     * app (libbe's VOS_APP_FLAGS_OVERRIDE hook in _InitData) so the
     * compositor has no row among the Deskbar applications — the helper
     * teams carry the user-visible windows. The variable is cleared right
     * after the handshake below: by then the BApplication is registered,
     * and no later child (helpers!) may inherit the override. The rdef
     * keeps B_EXCLUSIVE_LAUNCH only, so the flags word must repeat it. */
    if (background_app) {
        char value[16];
        snprintf(value, sizeof(value), "0x%x",
            B_EXCLUSIVE_LAUNCH | B_BACKGROUND_APP);
        setenv("VOS_APP_FLAGS_OVERRIDE", value, 1);
    }

    int fds[2];
    if (pipe(fds) != 0) {
        delete shim;
        return nullptr;
    }
    /* Read side: drained non-blockingly by the X dispatch loop.
     * Write side: non-blocking so a stalled X server can never wedge the
     * window looper threads (events are dropped instead, counted). */
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    shim->pipe_r = fds[0];
    shim->sink.pipe_w = fds[1];

    shim->startSem = create_sem(0, "vitrine_start");
    if (shim->startSem < 0) {
        close(fds[0]);
        close(fds[1]);
        delete shim;
        return nullptr;
    }

    shim->appThread = spawn_thread(run_app_thread, "vitrine_app",
                                   B_NORMAL_PRIORITY, shim);
    if (shim->appThread < 0 || resume_thread(shim->appThread) != B_OK) {
        delete_sem(shim->startSem);
        close(fds[0]);
        close(fds[1]);
        delete shim;
        return nullptr;
    }

    /* Wait until the app thread constructed the BApplication (or failed).
     * After this, be_app is valid and windows may be created from any
     * thread. */
    while (acquire_sem(shim->startSem) == B_INTERRUPTED)
        ;
    delete_sem(shim->startSem);
    shim->startSem = -1;

    /* Registered (or failed) — either way the override must not leak into
     * any process spawned from here on. */
    if (background_app)
        unsetenv("VOS_APP_FLAGS_OVERRIDE");

    if (shim->startStatus != B_OK) {
        fprintf(stderr, "vitrine: BApplication init failed: %s\n",
            strerror(shim->startStatus));
        status_t ret;
        wait_for_thread(shim->appThread, &ret);
        close(fds[0]);
        close(fds[1]);
        delete shim;
        return nullptr;
    }

    return shim;
}

extern "C" int beshim_input_fd(BeShim* shim)
{
    return shim ? shim->pipe_r : -1;
}

extern "C" int beshim_next_event(BeShim* shim, BeInputEvent* out)
{
    if (!shim || shim->pipe_r < 0 || !out)
        return 0;
    /* Writers emit whole records atomically, so the pipe always holds a
     * multiple of sizeof(BeInputEvent) bytes; a successful read is always
     * a complete record. */
    ssize_t n = read(shim->pipe_r, out, sizeof(*out));
    return (n == (ssize_t)sizeof(*out)) ? 1 : 0;
}

extern "C" void beshim_shutdown(BeShim* shim)
{
    if (!shim)
        return;

    /* Windows normally are already gone (beshim_destroy_window runs in the
     * screen fini path before this), but be robust: flip the flag so any
     * survivor's QuitRequested() consents when BApplication::QuitRequested
     * -> _QuitAllWindows() polls it, instead of vetoing app quit. */
    atomic_set(&shim->sink.shuttingDown, 1);

    if (be_app != nullptr) {
        /* Quit from a non-looper thread is LOCK-then-Quit(), not a posted
         * B_QUIT_REQUESTED. The posted route goes through
         * BApplication::QuitRequested() -> Quit() inside the looper's own
         * dispatch, and in this tree that re-enters the lock bookkeeping
         * (Looper.cpp:1236-1240 asserts the looper is locked exactly once
         * when Loop() starts, task_looper re-locks per message): losing
         * that race trips "looper must be locked before proceeding" and a
         * Guru Meditation. Reproduced on the early-failure path — start
         * without XDG_RUNTIME_DIR, where main() tears down while the app
         * thread has only just entered Run(). Lock() blocks until the
         * looper is in a consistent state, so this ordering is race-free.
         * Quit() from the owner deletes the object when Run() has not been
         * reached yet, so the app thread must NOT delete it again — it
         * checks be_app (nulled by ~BApplication) before doing so. */
        if (be_app->Lock())
            be_app->Quit();
        status_t ret;
        while (wait_for_thread(shim->appThread, &ret) == B_INTERRUPTED)
            ;
    }

    if (shim->pipe_r >= 0)
        close(shim->pipe_r);
    if (shim->sink.pipe_w >= 0)
        close(shim->sink.pipe_w);
    delete shim;
}

extern "C" void beshim_set_rootless(BeShim* shim, int rootless)
{
    if (!shim)
        return;
    /* Contract (bewindow.h): called once right after beshim_start(),
     * before any window exists. Windows snapshot the flag at construction,
     * so a plain store is race-free. */
    shim->rootless = rootless ? 1 : 0;
}

extern "C" int beshim_screen_size(BeShim* shim, int* w, int* h)
{
    if (!shim || !w || !h || be_app == nullptr)
        return -1;      /* BScreen needs the live app_server link */

    BScreen screen;     /* main screen, B_MAIN_SCREEN_ID (Screen.h:25) */
    if (!screen.IsValid())
        return -1;
    /* Frame() round-trips AS_GET_SCREEN_FRAME to app_server
     * (PrivateScreen.cpp:223-238) and degrades to an empty BRect(0,0,0,0)
     * when the private screen is gone (Screen.cpp:71-77) — reject that.
     * BRect units are pixels - 1, hence the +1. */
    BRect frame = screen.Frame();
    if (frame.Width() <= 0 || frame.Height() <= 0)
        return -1;
    *w = (int)frame.Width() + 1;
    *h = (int)frame.Height() + 1;
    return 0;
}

/* ---- window API: thin wrappers over bewindow_win.cpp ---- */

extern "C" BeWindow* beshim_create_window(BeShim* shim, int w, int h,
                                          const char* title)
{
    return shim ? bewin_create_screen(&shim->sink, w, h, title) : nullptr;
}

extern "C" BeWindow* beshim_create_xwindow(BeShim* shim,
                                           const BeWindowSpec* spec)
{
    return shim ? bewin_create_x(&shim->sink, spec) : nullptr;
}

extern "C" void beshim_set_title(BeWindow* win, const char* title)
{
    bewin_set_title(win, title);
}

extern "C" void beshim_move_window(BeWindow* win, int x, int y)
{
    bewin_move_window(win, x, y);
}

extern "C" void* beshim_window_bits(BeWindow* win, int* bytes_per_row)
{
    return bewin_window_bits(win, bytes_per_row);
}

extern "C" void beshim_blit(BeWindow* win, int x, int y, int w, int h)
{
    bewin_blit(win, x, y, w, h);
}

extern "C" void beshim_resize_window(BeWindow* win, int w, int h)
{
    bewin_resize_window(win, w, h);
}

extern "C" void beshim_destroy_window(BeWindow* win)
{
    bewin_destroy_window(win);
}

extern "C" void beshim_set_size_limits(BeWindow* win, int min_w, int min_h,
                                       int max_w, int max_h)
{
    bewin_set_size_limits(win, min_w, min_h, max_w, max_h);
}

extern "C" void beshim_activate(BeWindow* win)
{
    bewin_activate(win);
}

extern "C" void beshim_send_behind(BeWindow* win, BeWindow* behind_of)
{
    bewin_send_behind(win, behind_of);
}

/* ---- clipboard bridge (F7) ---- */

extern "C" char* beshim_clipboard_get_text(BeShim*, int* from_bridge)
{
    if (from_bridge != nullptr)
        *from_bridge = 0;
    if (be_clipboard == nullptr || !be_clipboard->Lock())
        return nullptr;

    char* result = nullptr;
    BMessage* clip = be_clipboard->Data();
    if (clip != nullptr) {
        if (from_bridge != nullptr && clip->HasBool("vos:wl-bridge"))
            *from_bridge = 1;
        const void* text = nullptr;
        ssize_t len = 0;
        if (clip->FindData("text/plain", B_MIME_TYPE, &text, &len) == B_OK
            && text != nullptr && len > 0) {
            result = static_cast<char*>(malloc(len + 1));
            if (result != nullptr) {
                memcpy(result, text, len);
                result[len] = '\0';
            }
        }
    }
    be_clipboard->Unlock();
    return result;
}

extern "C" int beshim_clipboard_set_text(BeShim*, const char* text)
{
    if (be_clipboard == nullptr || text == nullptr)
        return -1;
    if (!be_clipboard->Lock())
        return -1;

    status_t status = B_ERROR;
    if (be_clipboard->Clear() == B_OK) {
        BMessage* clip = be_clipboard->Data();
        if (clip != nullptr) {
            clip->AddData("text/plain", B_MIME_TYPE, text, strlen(text));
            clip->AddBool("vos:wl-bridge", true);   /* echo-guard tag */
            status = be_clipboard->Commit();
        }
    }
    be_clipboard->Unlock();
    return status == B_OK ? 0 : -1;
}

extern "C" int beshim_import_dmabuf(BeWindow*, int, int, uint32_t, uint64_t)
{
    return -1;  // phase 4: unsupported for now (software render only)
}

