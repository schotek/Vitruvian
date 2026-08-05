/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Xvitruvian - a kdrive X server that runs inside a VitruvianOS
 *              app_server window (BWindow).
 *
 * Input path.  The BeShim looper thread serializes BeOS input messages into
 * a self-pipe (bewindow.h); the pipe's read end is registered with the X
 * server's ospoll loop via SetNotifyFd() -- the same mechanism Xephyr uses
 * for its xcb connection fd in MouseEnable().  When the fd becomes readable
 * the notify callback drains every pending BeInputEvent and re-emits it
 * through KdEnqueueKeyboardEvent / KdEnqueuePointerEvent.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "vitruvian.h"

#include "inputstr.h"
#include "scrnintstr.h"
#include "opaque.h"             /* dispatchException, for documentation */

KdKeyboardInfo *vitruvianKbd;
KdPointerInfo *vitruvianMouse;

typedef struct _VitruvianInputPrivate {
    Bool enabled;
} VitruvianKbdPrivate, VitruvianPointerPrivate;

/* Current KD_BUTTON_* state and the last absolute pointer position, so
 * button/wheel events can be enqueued at the current pointer location. */
static int mouseState = 0;
static int mouseX = 0;
static int mouseY = 0;

/* Buttons 6/7 (horizontal wheel) have no KD_BUTTON_* names in kdrive.h;
 * KdEnqueuePointerEvent numbers button n as bit (1 << (n - 1)). */
#define VITRUVIAN_BUTTON_6 (1 << 5)
#define VITRUVIAN_BUTTON_7 (1 << 6)

/* XVITRUVIAN_DEBUG=1 enables key-path tracing (lost-release diagnosis). */
static int
vitruvianKeyDebug(void)
{
    static int debug = -1;

    if (debug < 0)
        debug = getenv("XVITRUVIAN_DEBUG") != NULL;
    return debug;
}

/*
 * BeOS button bitmask (InterfaceDefs.h): B_PRIMARY_MOUSE_BUTTON = 0x1
 * (left), B_SECONDARY_MOUSE_BUTTON = 0x2 (RIGHT), B_TERTIARY_MOUSE_BUTTON =
 * 0x4 (MIDDLE).  X numbers buttons 1 = left, 2 = middle, 3 = right, so the
 * BeOS 0x2/0x4 pair maps crosswise to KD_BUTTON_3/KD_BUTTON_2.
 */
static int
vitruvianTranslateButtons(int beButtons)
{
    int kd = 0;

    if (beButtons & 0x1)
        kd |= KD_BUTTON_1;      /* left    */
    if (beButtons & 0x4)
        kd |= KD_BUTTON_2;      /* middle  */
    if (beButtons & 0x2)
        kd |= KD_BUTTON_3;      /* right   */
    return kd;
}

static void
vitruvianProcessKey(BeInputEvent *ev)
{
    if (!vitruvianKbd ||
        !((VitruvianKbdPrivate *) vitruvianKbd->driverPrivate)->enabled)
        return;

    /*
     * ev->code is a raw Linux evdev keycode (0..255) and X/xkb "evdev"
     * rules expect X keycode == evdev code + 8.  We do NOT add the offset
     * here: KdEnqueueKeyboardEvent() computes
     *
     *     key_code = scan_code + KD_MIN_KEYCODE - ki->minScanCode
     *
     * and our keyboard driver reports minScanCode = 0, so the kdrive core
     * itself adds KD_MIN_KEYCODE (8) and delivers evdev+8 to DIX/xkb.
     * (Xephyr differs: it receives X keycodes from the host server --
     * already evdev+8 -- and sets minScanCode to the host keymap's
     * minKeyCode (8), which makes the same formula an identity there.)
     * Adding 8 ourselves would double-shift every key.
     *
     * Codes above maxScanCode (247) cannot be represented as X keycodes
     * (KD_MAX_KEYCODE is 255); drop them here instead of tripping the
     * ErrorF in KdEnqueueKeyboardEvent.
     */
    if (ev->code < 0 || ev->code > vitruvianKbd->maxScanCode)
        return;

    if (vitruvianKeyDebug())
        ErrorF("xvit: key evdev=%d %s\n", ev->code,
               ev->buttons ? "down" : "UP");

    KdEnqueueKeyboardEvent(vitruvianKbd, ev->code,
                           ev->buttons ? FALSE : TRUE /* is_up */);
}

/*
 * Stuck-key defense.  We are a nested server: a KeyRelease reaches us only
 * if app_server delivers it to one of OUR windows.  Releases get lost when
 * the BeOS focus moves away mid-press (the B_KEY_UP goes to the new focus
 * window) or when the host/QEMU drops the event before the guest sees it
 * (the input_server has its own repair for that class,
 * KeyboardInputDevice.cpp:698-705).  A lost release is fatal for usability
 * here because xkb soft autorepeat -- unconditionally enabled in kdrive
 * (XkbDDXUsesSoftRepeat() == 1, xkb/ddxCtrls.c:56) -- re-arms its timer
 * until a KeyRelease of the repeating key arrives (xkb/xkbAccessX.c:650),
 * i.e. one lost release repeats the key forever.
 *
 * Two complementary recovery paths, both synthesizing only releases (never
 * presses -- a synthesized press would type characters into whatever
 * window now holds the X focus):
 *
 *  1. vitruvianReleaseHeldKeys(): on BE_WINDOW_FOCUS_OUT release every key
 *     the device still holds.  Immediate, covers the focus-change case.
 *  2. vitruvianReconcileKeyStates(): every key event is followed by a
 *     BE_INPUT_KEY_STATES record carrying the input_server's physical
 *     "states" bitmap (evdev codes 0..127); any key the X device thinks is
 *     down but the bitmap says is up gets a release.  Self-heals all other
 *     divergence at the latest on the next keystroke.
 *
 * key_is_down(..., KEY_POSTED) is the queue-level state set/cleared
 * synchronously inside GetKeyboardEvents (dix/getevents.c set_key_down/up),
 * so a release we enqueue here immediately clears the bit and repeated
 * reconcile passes cannot spam duplicate releases.  KdEnqueueKeyboardEvent
 * takes a driver scan code: X keycode - KD_MIN_KEYCODE (minScanCode == 0,
 * see vitruvianProcessKey).
 */
static void
vitruvianReleaseHeldKeys(void)
{
    DeviceIntPtr dev;
    int key;

    if (!vitruvianKbd ||
        !((VitruvianKbdPrivate *) vitruvianKbd->driverPrivate)->enabled)
        return;
    dev = vitruvianKbd->dixdev;
    if (!dev || !dev->key)
        return;

    for (key = KD_MIN_KEYCODE; key <= KD_MAX_KEYCODE; key++) {
        if (key_is_down(dev, key, KEY_POSTED)) {
            if (vitruvianKeyDebug())
                ErrorF("xvit: focus-out release evdev=%d\n",
                       key - KD_MIN_KEYCODE);
            KdEnqueueKeyboardEvent(vitruvianKbd, key - KD_MIN_KEYCODE,
                                   TRUE /* is_up */);
        }
    }
}

static void
vitruvianReconcileKeyStates(BeInputEvent *ev)
{
    /* The 16-byte bitmap is packed over the contiguous code/x/y/buttons
     * int32 fields (bewindow.h BE_INPUT_KEY_STATES).  Bit layout follows
     * Haiku's key_info convention: states[code >> 3] & (1 << (7 - (code &
     * 7))), KeyboardInputDevice.cpp:707-712. */
    const unsigned char *states = (const unsigned char *) &ev->code;
    DeviceIntPtr dev;
    int evcode;

    if (!vitruvianKbd ||
        !((VitruvianKbdPrivate *) vitruvianKbd->driverPrivate)->enabled)
        return;
    dev = vitruvianKbd->dixdev;
    if (!dev || !dev->key)
        return;

    /* The bitmap only tracks evdev codes 0..127; higher codes (media keys)
     * are left to the FOCUS_OUT path. */
    for (evcode = 0; evcode < 128; evcode++) {
        Bool phys_down = (states[evcode >> 3] & (1 << (7 - (evcode & 7))))
            != 0;

        if (!phys_down &&
            key_is_down(dev, evcode + KD_MIN_KEYCODE, KEY_POSTED)) {
            if (vitruvianKeyDebug())
                ErrorF("xvit: reconcile release evdev=%d\n", evcode);
            KdEnqueueKeyboardEvent(vitruvianKbd, evcode, TRUE /* is_up */);
        }
    }
}

static void
vitruvianProcessMotion(BeInputEvent *ev)
{
    ScreenPtr pScreen;

    if (!vitruvianMouse ||
        !((VitruvianPointerPrivate *) vitruvianMouse->driverPrivate)->enabled)
        return;

    mouseX = ev->x;
    mouseY = ev->y;

    if (vitruvianRootless) {
        /* Rootless motion is already desktop-absolute (bewindow.h) and the
         * X root coordinate system is mapped 1:1 onto the desktop, so no
         * per-screen offset applies.  ev->screen is NOT a screen index
         * here -- it carries the emitting window's win_id/XID -- so it
         * must not be used to index screenInfo.screens.  Clamp to screen
         * 0: BeOS windows may hang partially off the desktop. */
        pScreen = screenInfo.screens[0];
        if (mouseX < 0)
            mouseX = 0;
        else if (mouseX > pScreen->width - 1)
            mouseX = pScreen->width - 1;
        if (mouseY < 0)
            mouseY = 0;
        else if (mouseY > pScreen->height - 1)
            mouseY = pScreen->height - 1;
    }
    /* Rootful: convert window coordinates into desktop-wide coordinates,
     * as ephyr does; fill_pointer_events converts back per-screen where
     * needed. */
    else if (ev->screen >= 0 && ev->screen < screenInfo.numScreens) {
        pScreen = screenInfo.screens[ev->screen];
        mouseX += pScreen->x;
        mouseY += pScreen->y;
    }

    KdEnqueuePointerEvent(vitruvianMouse, mouseState | KD_POINTER_DESKTOP,
                          mouseX, mouseY, 0);
}

static void
vitruvianProcessButton(BeInputEvent *ev)
{
    if (!vitruvianMouse ||
        !((VitruvianPointerPrivate *) vitruvianMouse->driverPrivate)->enabled)
        return;

    /* ev->buttons is the full current BeOS button mask, not a delta, so the
     * translation simply replaces the tracked state; KdEnqueuePointerEvent
     * synthesizes press/release from the state difference. */
    mouseState = vitruvianTranslateButtons(ev->buttons);

    KdEnqueuePointerEvent(vitruvianMouse, mouseState | KD_POINTER_DESKTOP,
                          mouseX, mouseY, 0);
}

static void
vitruvianProcessWheel(BeInputEvent *ev)
{
    int button = 0;

    if (!vitruvianMouse ||
        !((VitruvianPointerPrivate *) vitruvianMouse->driverPrivate)->enabled)
        return;

    /* Wheel steps become X button 4/5 (vertical) or 6/7 (horizontal)
     * press+release pairs, the convention every X toolkit expects. */
    if (ev->y < 0)
        button = KD_BUTTON_4;   /* scroll up    */
    else if (ev->y > 0)
        button = KD_BUTTON_5;   /* scroll down  */
    else if (ev->x < 0)
        button = VITRUVIAN_BUTTON_6;    /* scroll left  */
    else if (ev->x > 0)
        button = VITRUVIAN_BUTTON_7;    /* scroll right */

    if (!button)
        return;

    KdEnqueuePointerEvent(vitruvianMouse,
                          (mouseState | button) | KD_POINTER_DESKTOP,
                          mouseX, mouseY, 0);
    KdEnqueuePointerEvent(vitruvianMouse, mouseState | KD_POINTER_DESKTOP,
                          mouseX, mouseY, 0);
}

static void
vitruvianProcessWindowEvent(BeInputEvent *ev)
{
    /*
     * FOCUS_OUT releases held keys in BOTH modes (see the stuck-key
     * comment above vitruvianReleaseHeldKeys).  In rootless this fires on
     * every inter-window focus change too; that matches nested-server
     * practice (Xephyr-class servers cannot see releases delivered
     * elsewhere) and only costs a modifier held across a click-to-focus,
     * which the next key event's reconcile pass cannot restore either --
     * we never synthesize presses.
     */
    if (ev->code == BE_WINDOW_FOCUS_OUT)
        vitruvianReleaseHeldKeys();

    /*
     * Rootless: per-window events (close/move/resize/focus, with
     * ev->screen = the X window's XID) drive the WM replacement logic in
     * vitruvian_rootless.c.
     */
    if (vitruvianRootless) {
        vitruvianRootlessHandleWindowEvent(ev);
        return;
    }

    /*
     * Rootful BE_WINDOW_CLOSE: the user clicked the BWindow close button.
     * Request a clean server shutdown exactly like a SIGTERM would:
     * GiveUp() sets dispatchException |= DE_TERMINATE and isItTimeToYield,
     * letting Dispatch() unwind normally (kdrive's ddxGiveUp disables the
     * screens).
     *
     * BE_WINDOW_RESIZED/MOVED/FOCUS_IN are ignored here (the rootful
     * window is B_NOT_RESIZABLE and X focus tracks the single window).
     */
    if (ev->code == BE_WINDOW_CLOSE)
        GiveUp(0);
}

static void
vitruvianNotifyFd(int fd, int ready, void *data)
{
    BeInputEvent ev;

    while (vitruvianShim && beshim_next_event(vitruvianShim, &ev)) {
        switch (ev.type) {
        case BE_INPUT_KEY:
            vitruvianProcessKey(&ev);
            break;

        case BE_INPUT_MOTION:
            vitruvianProcessMotion(&ev);
            break;

        case BE_INPUT_BUTTON:
            vitruvianProcessButton(&ev);
            break;

        case BE_INPUT_WHEEL:
            vitruvianProcessWheel(&ev);
            break;

        case BE_INPUT_WINDOW:
            vitruvianProcessWindowEvent(&ev);
            break;

        case BE_INPUT_KEY_STATES:
            vitruvianReconcileKeyStates(&ev);
            break;

        default:
            break;
        }
    }
}

/* Mouse calls */

static Status
MouseInit(KdPointerInfo * pi)
{
    pi->driverPrivate = (VitruvianPointerPrivate *)
        calloc(1, sizeof(VitruvianPointerPrivate));
    if (!pi->driverPrivate)
        return BadAlloc;
    pi->nAxes = 3;
    pi->nButtons = 8;
    free(pi->name);
    pi->name = strdup("Xvitruvian virtual mouse");

    /*
     * The BWindow always reports unrotated window coordinates, so pointer
     * coords must be transformed when the screen is rotated via RandR --
     * same situation as Xephyr under its host server.
     */
    pi->transformCoordinates = TRUE;

    vitruvianMouse = pi;
    return Success;
}

static Status
MouseEnable(KdPointerInfo * pi)
{
    ((VitruvianPointerPrivate *) pi->driverPrivate)->enabled = TRUE;
    if (vitruvianShim)
        SetNotifyFd(beshim_input_fd(vitruvianShim), vitruvianNotifyFd,
                    X_NOTIFY_READ, NULL);
    return Success;
}

static void
MouseDisable(KdPointerInfo * pi)
{
    ((VitruvianPointerPrivate *) pi->driverPrivate)->enabled = FALSE;
    if (vitruvianShim)
        RemoveNotifyFd(beshim_input_fd(vitruvianShim));
}

static void
MouseFini(KdPointerInfo * pi)
{
    free(pi->driverPrivate);
    pi->driverPrivate = NULL;
    vitruvianMouse = NULL;
}

KdPointerDriver VitruvianMouseDriver = {
    "vitruvian",
    MouseInit,
    MouseEnable,
    MouseDisable,
    MouseFini,
    NULL,
};

/* Keyboard */

/* kinput.c compares the Init/Enable results against Success (0), so these
 * return Status values even though the struct fields are typed Bool. */
static Status
VitruvianKeyboardInit(KdKeyboardInfo * ki)
{
    ki->driverPrivate = (VitruvianKbdPrivate *)
        calloc(1, sizeof(VitruvianKbdPrivate));
    if (!ki->driverPrivate)
        return BadAlloc;

    /*
     * Raw evdev scancodes 0..247; kinput.c shifts them by KD_MIN_KEYCODE -
     * minScanCode = 8 into the X keycode range the "evdev" xkb rules
     * expect.  See the comment in vitruvianProcessKey().
     */
    ki->minScanCode = 0;
    ki->maxScanCode = KD_MAX_KEYCODE - KD_MIN_KEYCODE;

    free(ki->name);
    ki->name = strdup("Xvitruvian virtual keyboard");

    vitruvianKbd = ki;
    return Success;
}

static Status
VitruvianKeyboardEnable(KdKeyboardInfo * ki)
{
    ((VitruvianKbdPrivate *) ki->driverPrivate)->enabled = TRUE;
    return Success;
}

static void
VitruvianKeyboardDisable(KdKeyboardInfo * ki)
{
    ((VitruvianKbdPrivate *) ki->driverPrivate)->enabled = FALSE;
}

static void
VitruvianKeyboardFini(KdKeyboardInfo * ki)
{
    free(ki->driverPrivate);
    ki->driverPrivate = NULL;
    vitruvianKbd = NULL;
}

static void
VitruvianKeyboardLeds(KdKeyboardInfo * ki, int leds)
{
}

static void
VitruvianKeyboardBell(KdKeyboardInfo * ki, int volume, int frequency,
                      int duration)
{
}

KdKeyboardDriver VitruvianKeyboardDriver = {
    "vitruvian",
    VitruvianKeyboardInit,
    VitruvianKeyboardEnable,
    VitruvianKeyboardLeds,
    VitruvianKeyboardBell,
    VitruvianKeyboardDisable,
    VitruvianKeyboardFini,
    NULL,
};
