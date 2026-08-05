/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Xvitruvian - a kdrive X server that runs inside a VitruvianOS
 *              app_server window (BWindow).
 *
 * Rootless mode: no root window is displayed.  Every mapped top-level X
 * window (direct InputOutput child of the root) gets its own BWindow via the
 * C shim (bewindow.h); the BeOS decorations replace the X window manager:
 *
 *   close button        -> WM_DELETE_WINDOW ClientMessage (or client kill)
 *   BeOS activation     -> X input focus + raise
 *   user move/resize    -> ConfigureWindow()
 *   X-side configure    -> beshim_move_window()/beshim_resize_window()
 *
 * Rendering uses manual Composite redirection: each tracked window renders
 * into its own backing pixmap (compRedirectWindow with
 * CompositeRedirectManual, composite/compalloc.c:136), and a per-window
 * DamageRec drives copies of the dirty boxes from that pixmap into the
 * window's BBitmap from the screen BlockHandler -- the same
 * damage-accumulate/blit-on-block scheme the rootful path uses for the
 * whole screen (vitruvianInternalDamageRedisplay).
 *
 * Manually redirected windows do not occlude their siblings and are never
 * clipped by them (mi/mivaltree.c:171 "#define TreatAsTransparent(w)
 * ((w)->redirectDraw == RedirectDrawManual)"), so overlapping top-levels
 * each keep fully rendered pixmaps -- exactly what per-window BWindows need.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "vitruvian.h"

#include "inputstr.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "propertyst.h"
#include "property.h"
#include "privates.h"
/* compint.h is composite's private header; it is reachable because the
 * top-level meson.build puts 'composite' into the shared include list `inc`
 * (xorg-server/meson.build:562) -- the same way dix/window.c and Xext/xres.c
 * include it from outside composite/. */
#include "compint.h"

/* Set by ddxProcessArgument("-rootless") in vitruvianinit.c. */
Bool vitruvianRootless = TRUE;   /* rootless is the default; see ddxProcessArgument */

/* ClientMessage events sent on behalf of a WM must carry the send-event flag
 * like XSendEvent-generated ones do; dix keeps this define private to
 * dix/events.c:5535, so mirror it here. */
#define VITRUVIAN_SEND_EVENT_BIT 0x80

/*
 * Per-window state.  Kept both in a global singly-linked list (for the
 * BlockHandler flush walk) and in a window dev-private (for O(1) lookup in
 * the screen hooks and the property callback).
 */
typedef struct _VitruvianWindow {
    WindowPtr win;
    BeWindow *bewin;
    DamagePtr damage;
    int x, y;                   /* last position pushed to/known by BeOS  */
    int width, height;          /* current BBitmap dimensions             */
    Bool full_repaint;          /* repaint everything on the next flush   */
    Bool pending_configure;     /* X must be moved to x/y on next flush
                                 * (WM placement chose a position; see
                                 * vitruvianRootlessTrack)                */
    struct _VitruvianWindow *next;
} VitruvianWindow;

static VitruvianWindow *vitruvianWindows;

static DevPrivateKeyRec vitruvianWindowKeyRec;

#define vitruvianWindowKey (&vitruvianWindowKeyRec)

/* Atoms are wiped and re-interned each server generation (InitAtoms,
 * dix/main.c:184), so these are refreshed in vitruvianRootlessInit. */
static Atom vitruvianAtomWmProtocols;
static Atom vitruvianAtomWmDelete;
static Atom vitruvianAtomWmName;
static Atom vitruvianAtomNetWmName;
static Atom vitruvianAtomUtf8String;
static Atom vitruvianAtomWmNormalHints;

/* InitCallbackManager (dix/main.c:189) deletes every callback list at the
 * start of each generation (dix/dixutils.c:892), so the PropertyStateCallback
 * registration must be redone per generation -- and only once per
 * generation, hence this guard. */
static unsigned long vitruvianCallbackGeneration;

static VitruvianWindow *
vitruvianWindowGet(WindowPtr pWin)
{
    return dixLookupPrivate(&pWin->devPrivates, vitruvianWindowKey);
}

/*
 * A window deserves its own BWindow iff it is an InputOutput direct child
 * of the root.  (InputOnly windows have no pixels; deeper children render
 * into their top-level ancestor's pixmap.)
 */
static Bool
vitruvianRootlessCandidate(WindowPtr pWin)
{
    return pWin->parent && !pWin->parent->parent &&
        pWin->drawable.class == InputOutput;
}

/*
 * Read the window title: _NET_WM_NAME (UTF8_STRING, format 8) wins over
 * WM_NAME (STRING/COMPOUND_TEXT, passed through as-is).  `excluded` skips a
 * property that is being deleted right now: DeleteProperty runs the
 * PropertyStateCallback while the doomed PropertyRec may still be reachable,
 * so the fallback must not resurrect it.  Property data is NOT
 * NUL-terminated (dix/property.c stores raw client bytes); returns a
 * malloc'ed NUL-terminated copy or NULL.
 */
static char *
vitruvianRootlessGetTitle(WindowPtr pWin, Atom excluded)
{
    Atom prefs[2];
    int i;

    prefs[0] = vitruvianAtomNetWmName;
    prefs[1] = vitruvianAtomWmName;

    for (i = 0; i < 2; i++) {
        PropertyPtr pProp;
        char *title;
        int rc;

        if (prefs[i] == None || prefs[i] == excluded)
            continue;

        rc = dixLookupProperty(&pProp, pWin, prefs[i], serverClient,
                               DixReadAccess);
        if (rc != Success || pProp->format != 8 || !pProp->data ||
            !pProp->size)
            continue;

        /* EWMH requires _NET_WM_NAME to be typed UTF8_STRING; anything
         * else is a broken client -- fall through to WM_NAME. */
        if (prefs[i] == vitruvianAtomNetWmName &&
            pProp->type != vitruvianAtomUtf8String)
            continue;

        /* format 8 => PropertyRec.size counts bytes (dix/property.c
         * stores size in format/8 units). */
        title = malloc(pProp->size + 1);
        if (!title)
            return NULL;
        memcpy(title, pProp->data, pProp->size);
        title[pProp->size] = '\0';
        return title;
    }
    return NULL;
}

/*
 * WM placement policy.  Without a window manager, X clients map wherever
 * they asked -- almost always (0,0), which parks the window in the desktop
 * corner with its BeOS tab (drawn ABOVE the frame) clipped off-screen.
 *
 * ICCCM: the client's position preference is the window's own geometry; the
 * obsolete x/y fields inside WM_SIZE_HINTS are ignored, only the flags
 * matter.  USPosition (1<<0) = the *user* asked (e.g. `xterm -geometry
 * +100+100`): always honor.  PPosition (1<<2) = the *program* picked a spot:
 * honor unless it is the meaningless (0,0) that Xt toolkits emit for "no
 * preference" -- the same pragmatism classic WMs apply.  Everything else
 * gets centered with a small cascade so stacked launches stay distinguishable.
 */
static Bool
vitruvianRootlessWantsPosition(WindowPtr pWin)
{
    PropertyPtr pProp;
    uint32_t flags;
    int rc;

    rc = dixLookupProperty(&pProp, pWin, vitruvianAtomWmNormalHints,
                           serverClient, DixReadAccess);
    if (rc != Success || pProp->format != 32 || !pProp->data ||
        pProp->size < 1)
        return FALSE;

    /* First CARD32 of WM_SIZE_HINTS is the flags field (ICCCM 4.1.2.3). */
    flags = *(uint32_t *) pProp->data;
    if (flags & (1u << 0))      /* USPosition */
        return TRUE;
    if ((flags & (1u << 2)) &&  /* PPosition, but not the Xt (0,0) default */
        (pWin->drawable.x != 0 || pWin->drawable.y != 0))
        return TRUE;
    return FALSE;
}

static void
vitruvianRootlessPlace(WindowPtr pWin, int *px, int *py)
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    static int cascade;
    int off = (cascade++ % 8) * 24;
    int x = (pScreen->width - (int) pWin->drawable.width) / 2 + off;
    int y = (pScreen->height - (int) pWin->drawable.height) / 2 + off;

    /* Keep the BeOS tab reachable: it sits above the frame (~26 px), so
     * never place the content edge above y = 28; keep at least a corner of
     * oversized windows on-screen. */
    if (x > pScreen->width - 64)
        x = pScreen->width - 64;
    if (y > pScreen->height - 64)
        y = pScreen->height - 64;
    if (x < 4)
        x = 4;
    if (y < 28)
        y = 28;

    *px = x;
    *py = y;
}

/*
 * Detach a window from the rootless machinery.
 *
 * Deliberately does NOT call compUnredirectWindow: the redirection lives
 * for the whole lifetime of the window, not just its realized span.
 * Unredirecting here would run compFreeClientWindow ->
 * compHandleMarkedWindows -> miValidateTree synchronously, and this
 * function is reached from inside UnrealizeTree/CrushTree (client
 * teardown), where the window tree is mid-surgery -- that exact chain
 * segfaulted in miComputeClips (verified backtrace: CrushTree ->
 * UnmapWindow -> UnrealizeTree -> our unrealize wrap -> untrack ->
 * compUnredirectWindow -> FreeResource -> compFreeClientWindow ->
 * compHandleMarkedWindows -> miValidateTree -> miComputeClips, SIGSEGV).
 * Composite tears the redirection down itself when the window dies
 * (compDestroyWindow walks cw->clients and FreeResource()s every entry,
 * compwindow.c:609-612), at a point where the tree is consistent.  On a
 * plain unmap the redirection simply stays in place; compCheckRedirect
 * frees the backing pixmap while unrealized and re-allocates it on the
 * next realize.
 */
static void
vitruvianRootlessUntrack(WindowPtr pWin)
{
    VitruvianWindow *vw = vitruvianWindowGet(pWin);
    VitruvianWindow **prev;

    if (!vw)
        return;

    if (vw->damage) {
        /* DamageDestroy unregisters first when still registered
         * (miext/damage/damage.c:1837); it only touches the damage list,
         * no tree validation -- safe inside UnrealizeTree. */
        DamageDestroy(vw->damage);
        vw->damage = NULL;
    }

    if (vw->bewin) {
        beshim_destroy_window(vw->bewin);
        vw->bewin = NULL;
    }

    for (prev = &vitruvianWindows; *prev; prev = &(*prev)->next) {
        if (*prev == vw) {
            *prev = vw->next;
            break;
        }
    }

    dixSetPrivate(&pWin->devPrivates, vitruvianWindowKey, NULL);
    free(vw);
}

/*
 * Attach a freshly realized top-level: redirect it manually, hang an
 * accumulate-only DamageRec off it and open the BWindow.
 */
static void
vitruvianRootlessTrack(WindowPtr pWin)
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    VitruvianWindow *vw;
    BeWindowSpec spec;
    char *title;
    int rc;

    /* RealizeTree sets pWin->realized before calling the RealizeWindow
     * hook (dix/window.c:2604), so compRedirectWindow's compCheckRedirect
     * (compwindow.c:156) sees a realized window and allocates the backing
     * pixmap immediately.  (Redirecting from inside the Realize hook is
     * sanctioned practice -- Xwayland does the same.)
     *
     * BadAccess means a Manual redirect is already installed
     * (compalloc.c:155-158): ours, surviving from a previous
     * unmap/remap cycle, because untrack intentionally never unredirects
     * (see vitruvianRootlessUntrack).  Composite's realize pass -- which
     * ran before us in the wrap chain -- has already re-allocated the
     * backing pixmap in that case. */
    rc = compRedirectWindow(serverClient, pWin, CompositeRedirectManual);
    if (rc != Success && rc != BadAccess) {
        ErrorF("Xvitruvian: cannot redirect window 0x%lx (rc %d)\n",
               (unsigned long) pWin->drawable.id, rc);
        return;
    }

    vw = calloc(1, sizeof(*vw));
    if (!vw)
        return;                 /* stays redirected; see bail_free comment */

    /* Accumulate-only damage, same setup as the rootful screen damage in
     * vitruvianSetInternalDamage: DamageReportNone still accumulates into
     * DamageRegion (miext/damage/damage.c:1970), which the BlockHandler
     * flush drains.  Registered on the *window* drawable: because the
     * window is redirected, the record lands on the backing pixmap's
     * damage list (getDrawableDamageRef resolves windows through
     * GetWindowPixmap, damage.c:86-110), so rendering into the pixmap is
     * what feeds it; damageSetWindowPixmap re-homes it if the pixmap is
     * ever swapped (damage.c:1548). */
    vw->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE,
                              pScreen, NULL);
    if (!vw->damage)
        goto bail_free;
    DamageRegister(&pWin->drawable, vw->damage);

    title = vitruvianRootlessGetTitle(pWin, None);

    /* WM placement (see vitruvianRootlessWantsPosition): menus place
     * themselves, position-hinted windows are honored, the rest is
     * centered/cascaded.  The BWindow opens at the placed position right
     * away; the X window is moved to match from the next BlockHandler
     * flush (pending_configure) rather than here -- RealizeWindow runs in
     * the middle of MapWindow (dix/window.c: RealizeTree happens before
     * MapWindow's own ValidateTree/expose pass), and re-entering
     * ConfigureWindow would run a nested ValidateTree over the half-updated
     * tree.  The first flush executes before anything is blitted, so the
     * window never visibly jumps. */
    spec.x = pWin->drawable.x;
    spec.y = pWin->drawable.y;
    if (!pWin->overrideRedirect && !vitruvianRootlessWantsPosition(pWin))
        vitruvianRootlessPlace(pWin, &spec.x, &spec.y);

    /* BWindow frames denote the content area (decorations lie outside,
     * bewindow.h), so X drawable geometry maps 1:1 onto desktop coords. */
    spec.w = pWin->drawable.width;
    spec.h = pWin->drawable.height;
    spec.title = title;
    spec.override_redirect = pWin->overrideRedirect;
    spec.resizable = !pWin->overrideRedirect;
    spec.win_id = (int) pWin->drawable.id;

    vw->bewin = beshim_create_xwindow(vitruvianShim, &spec);
    free(title);
    if (!vw->bewin) {
        ErrorF("Xvitruvian: cannot create BWindow for 0x%lx\n",
               (unsigned long) pWin->drawable.id);
        DamageDestroy(vw->damage);
        goto bail_free;
    }

    vw->win = pWin;
    vw->x = spec.x;
    vw->y = spec.y;
    vw->pending_configure = (spec.x != pWin->drawable.x ||
                             spec.y != pWin->drawable.y);
    vw->width = pWin->drawable.width;
    vw->height = pWin->drawable.height;
    /* The backing pixmap was primed with a copy of the parent's pixels
     * (compNewPixmap, compalloc.c:531) which never saw the client's
     * drawing; paint the whole window once so the BBitmap starts sane. */
    vw->full_repaint = TRUE;

    vw->next = vitruvianWindows;
    vitruvianWindows = vw;
    dixSetPrivate(&pWin->devPrivates, vitruvianWindowKey, vw);
    return;

 bail_free:
    free(vw);
    /* The window stays redirected on these (OOM-class) failure paths:
     * unredirecting here would re-enter miValidateTree from inside
     * MapWindow (see vitruvianRootlessUntrack), and composite reclaims
     * everything at window destruction anyway. */
}

/*
 * Screen hook wraps.  Wrap ordering: composite installs its hooks at
 * extension-init time (compScreenInit, compinit.c:376-395, run from
 * InitExtensions at dix/main.c:194), while ours are installed from
 * CreateScreenResources (dix/main.c:200-211) -- later, so whoever wrapped
 * LAST runs FIRST: our hook runs, chains into composite's, and does its own
 * work before/after the inner call using the standard save/restore pattern.
 */

static Bool
vitruvianRootlessRealizeWindow(WindowPtr pWin)
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    KdScreenPriv(pScreen);
    VitruvianScrPriv *scrpriv = pScreenPriv->screen->driver;
    Bool ret;

    pScreen->RealizeWindow = scrpriv->RealizeWindow;
    ret = (*pScreen->RealizeWindow) (pWin);
    scrpriv->RealizeWindow = pScreen->RealizeWindow;
    pScreen->RealizeWindow = vitruvianRootlessRealizeWindow;

    if (ret && vitruvianRootlessCandidate(pWin) && !vitruvianWindowGet(pWin))
        vitruvianRootlessTrack(pWin);

    return ret;
}

static Bool
vitruvianRootlessUnrealizeWindow(WindowPtr pWin)
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    KdScreenPriv(pScreen);
    VitruvianScrPriv *scrpriv = pScreenPriv->screen->driver;
    Bool ret;

    pScreen->UnrealizeWindow = scrpriv->UnrealizeWindow;
    ret = (*pScreen->UnrealizeWindow) (pWin);
    scrpriv->UnrealizeWindow = pScreen->UnrealizeWindow;
    pScreen->UnrealizeWindow = vitruvianRootlessUnrealizeWindow;

    /* The inner chain includes compUnrealizeWindow (compwindow.c:284),
     * whose compCheckRedirect already freed the backing pixmap (realized
     * is FALSE by now); our damage record survived that because
     * damageSetWindowPixmap migrated it back to the screen pixmap's list
     * (damage.c:1548).  Now drop everything, including the redirection
     * bookkeeping. */
    vitruvianRootlessUntrack(pWin);

    return ret;
}

static Bool
vitruvianRootlessDestroyWindow(WindowPtr pWin)
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    KdScreenPriv(pScreen);
    VitruvianScrPriv *scrpriv = pScreenPriv->screen->driver;
    Bool ret;

    /* Clean up BEFORE chaining down: damage's own DestroyWindow wrap
     * destroys every DamageRec still attached to the window
     * (damageDestroyWindow, miext/damage/damage.c), which would leave
     * vw->damage dangling if it ran first.  Since we wrapped last, we run
     * first -- exploit that.  Composite's wrap (further down the chain)
     * owns the redirection teardown, hence unredirect = FALSE. */
    vitruvianRootlessUntrack(pWin);

    pScreen->DestroyWindow = scrpriv->DestroyWindow;
    ret = (*pScreen->DestroyWindow) (pWin);
    scrpriv->DestroyWindow = pScreen->DestroyWindow;
    pScreen->DestroyWindow = vitruvianRootlessDestroyWindow;

    return ret;
}

static void
vitruvianRootlessMoveWindow(WindowPtr pWin, int x, int y, WindowPtr pSib,
                            VTKind kind)
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    KdScreenPriv(pScreen);
    VitruvianScrPriv *scrpriv = pScreenPriv->screen->driver;
    VitruvianWindow *vw;

    pScreen->MoveWindow = scrpriv->MoveWindow;
    (*pScreen->MoveWindow) (pWin, x, y, pSib, kind);
    scrpriv->MoveWindow = pScreen->MoveWindow;
    pScreen->MoveWindow = vitruvianRootlessMoveWindow;

    /* After the inner call pWin->drawable.x/y hold the new screen-space
     * position (miMoveWindow updates the drawable).  The pixmap bits do
     * not change on a move -- composite only updates screen_x/screen_y
     * (compwindow.c:249-251) -- so no repaint, just move the BWindow.
     * beshim_move_window suppresses the echo FrameMoved (bewindow.h). */
    vw = vitruvianWindowGet(pWin);
    if (vw && vw->bewin &&
        (vw->x != pWin->drawable.x || vw->y != pWin->drawable.y)) {
        vw->x = pWin->drawable.x;
        vw->y = pWin->drawable.y;
        beshim_move_window(vw->bewin, vw->x, vw->y);
    }
}

static void
vitruvianRootlessResizeWindow(WindowPtr pWin, int x, int y,
                              unsigned int w, unsigned int h, WindowPtr pSib)
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    KdScreenPriv(pScreen);
    VitruvianScrPriv *scrpriv = pScreenPriv->screen->driver;
    VitruvianWindow *vw;

    pScreen->ResizeWindow = scrpriv->ResizeWindow;
    (*pScreen->ResizeWindow) (pWin, x, y, w, h, pSib);
    scrpriv->ResizeWindow = pScreen->ResizeWindow;
    pScreen->ResizeWindow = vitruvianRootlessResizeWindow;

    vw = vitruvianWindowGet(pWin);
    if (!vw || !vw->bewin)
        return;

    if (vw->width != pWin->drawable.width ||
        vw->height != pWin->drawable.height) {
        vw->width = pWin->drawable.width;
        vw->height = pWin->drawable.height;
        /* Swaps in a fresh BBitmap; must not echo BE_WINDOW_RESIZED
         * (bewindow.h).  The new bitmap is uninitialized, so schedule a
         * full copy on the next flush (bits are re-fetched there). */
        beshim_resize_window(vw->bewin, vw->width, vw->height);
        vw->full_repaint = TRUE;
    }

    /* A resize may reposition the window too (e.g. gravity). */
    if (vw->x != pWin->drawable.x || vw->y != pWin->drawable.y) {
        vw->x = pWin->drawable.x;
        vw->y = pWin->drawable.y;
        beshim_move_window(vw->bewin, vw->x, vw->y);
    }
}

/*
 * PropertyStateCallback: track WM_NAME/_NET_WM_NAME changes.  Called from
 * deliverPropertyNotifyEvent (dix/property.c:128) on every property change
 * or deletion, before the PropertyNotify goes out.
 */
static void
vitruvianRootlessPropertyCallback(CallbackListPtr *pcbl, void *closure,
                                  void *calldata)
{
    PropertyStateRec *rec = calldata;
    VitruvianWindow *vw;
    Atom name;
    char *title;

    if (!rec || !rec->win || !rec->prop)
        return;

    vw = vitruvianWindowGet(rec->win);
    if (!vw || !vw->bewin)
        return;

    name = rec->prop->propertyName;
    if (name != vitruvianAtomWmName && name != vitruvianAtomNetWmName)
        return;

    /* Recompute with the preference order instead of using rec->prop
     * directly: a WM_NAME change must not clobber an existing
     * _NET_WM_NAME.  On deletion, exclude the dying property. */
    title = vitruvianRootlessGetTitle(rec->win,
                                     rec->state == PropertyDelete
                                     ? name : None);
    beshim_set_title(vw->bewin, title ? title : "");
    free(title);
}

/*
 * BlockHandler flush: copy each tracked window's dirty boxes from its
 * composite backing pixmap into its BBitmap and blit them.
 *
 * Coordinate spaces, verified against miext/damage/damage.c:
 *  - Rendering into the backing pixmap enters damageRegionAppend in
 *    pixmap-relative coords and is translated to screen coords via
 *    screen_x/screen_y (damage.c:159-165).
 *  - It is then translated into the *target drawable's* space: for our
 *    window-registered damage, RegionTranslate(-draw_x, -draw_y) with
 *    draw_x/draw_y = pDrawable->x/y (damage.c:202-203, 253-256 "Move
 *    region to target coordinate space").
 *  => DamageRegion() boxes are WINDOW-LOCAL (origin = drawable.x/y), which
 *     is also the BBitmap's space.
 *
 * Source position in the pixmap: the backing pixmap sits at screen
 * (drawable.x - bw, drawable.y - bw) and spans the border
 * (compAllocPixmap, composite/compalloc.c:600-607; screen_x/y set in
 * compNewPixmap, compalloc.c:544-545), so a window-local box (x1,y1) reads
 * from pixmap-local (x1 + drawable.x - screen_x, y1 + drawable.y -
 * screen_y).  The X border itself is never shown -- BeOS decorations
 * replace it.
 */
void
vitruvianRootlessFlush(ScreenPtr pScreen)
{
    VitruvianWindow *vw;

    for (vw = vitruvianWindows; vw; vw = vw->next) {
        WindowPtr pWin = vw->win;
        PixmapPtr pPixmap;
        RegionPtr pRegion;
        RegionRec fullRegion;
        Bool full = vw->full_repaint;
        CARD8 *src_bits, *dst_bits;
        int src_stride, dst_stride;
        int win_x, win_y;
        int nbox;
        BoxPtr pbox;

        if (!vw->bewin || pWin->drawable.pScreen != pScreen)
            continue;

        /* Deferred WM placement (vitruvianRootlessTrack): the BWindow
         * already sits at vw->x/y, teach X the same position now that we
         * are at top level (BlockHandler), outside MapWindow's tree
         * update.  Re-enters our MoveWindow wrap, which finds vw->x/y
         * already equal to the new drawable position and stays quiet. */
        if (vw->pending_configure) {
            XID pvals[2];
            int bw = (int) pWin->borderWidth;

            vw->pending_configure = FALSE;
            pvals[0] = (XID) (vw->x - bw);
            pvals[1] = (XID) (vw->y - bw);
            (void) ConfigureWindow(pWin, CWX | CWY, pvals, serverClient);
        }

        if (pWin->redirectDraw != RedirectDrawManual)
            continue;
        pPixmap = (*pScreen->GetWindowPixmap) (pWin);
        if (!pPixmap || !pPixmap->devPrivate.ptr ||
            pPixmap->drawable.bitsPerPixel != 32)
            continue;

        if (full) {
            BoxRec bounds;

            bounds.x1 = 0;
            bounds.y1 = 0;
            bounds.x2 = pWin->drawable.width;
            bounds.y2 = pWin->drawable.height;
            RegionInit(&fullRegion, &bounds, 1);
            pRegion = &fullRegion;
        }
        else {
            pRegion = DamageRegion(vw->damage);
            if (!RegionNotEmpty(pRegion))
                continue;
        }

        /* Re-fetch every time: beshim_resize_window swaps the BBitmap. */
        dst_bits = beshim_window_bits(vw->bewin, &dst_stride);
        if (!dst_bits) {
            /* Keep full_repaint and the damage region: retry next flush. */
            if (full)
                RegionUninit(&fullRegion);
            continue;
        }

        src_bits = pPixmap->devPrivate.ptr;
        src_stride = pPixmap->devKind;
        win_x = pWin->drawable.x;
        win_y = pWin->drawable.y;

        nbox = RegionNumRects(pRegion);
        pbox = RegionRects(pRegion);

        while (nbox--) {
            int x1 = pbox->x1;
            int y1 = pbox->y1;
            int x2 = pbox->x2;
            int y2 = pbox->y2;
            int sx, sy, row;

            pbox++;

            /* Clip to the window content AND the current BBitmap size:
             * vw->width/height track the bitmap, drawable.* the window;
             * they only diverge inside a not-yet-flushed resize. */
            if (x1 < 0)
                x1 = 0;
            if (y1 < 0)
                y1 = 0;
            if (x2 > pWin->drawable.width)
                x2 = pWin->drawable.width;
            if (x2 > vw->width)
                x2 = vw->width;
            if (y2 > pWin->drawable.height)
                y2 = pWin->drawable.height;
            if (y2 > vw->height)
                y2 = vw->height;
            if (x1 >= x2 || y1 >= y2)
                continue;

            /* Window-local -> pixmap-local (see block comment). */
            sx = x1 + win_x - pPixmap->screen_x;
            sy = y1 + win_y - pPixmap->screen_y;
            if (sx < 0 || sy < 0 ||
                sx + (x2 - x1) > pPixmap->drawable.width ||
                sy + (y2 - y1) > pPixmap->drawable.height)
                continue;

            for (row = 0; row < y2 - y1; row++)
                memcpy(dst_bits + (size_t) (y1 + row) * dst_stride +
                       (size_t) x1 * 4,
                       src_bits + (size_t) (sy + row) * src_stride +
                       (size_t) sx * 4, (size_t) (x2 - x1) * 4);

            beshim_blit(vw->bewin, x1, y1, x2 - x1, y2 - y1);
        }

        if (full) {
            RegionUninit(&fullRegion);
            vw->full_repaint = FALSE;
        }
        DamageEmpty(vw->damage);
    }
}

/*
 * BE_INPUT_WINDOW events from the shim (BeInputEvent.screen carries the X
 * window's XID, see BeWindowSpec.win_id).  Called from vitruvianNotifyFd in
 * vitruvian_input.c when rootless.
 */
void
vitruvianRootlessHandleWindowEvent(BeInputEvent *ev)
{
    WindowPtr pWin;
    VitruvianWindow *vw;
    XID vals[2];
    int rc;

    /* DixGetAttrAccess is the weakest mode that lets us at the window; the
     * heavier operations below (ConfigureWindow, SetInputFocus) run as
     * serverClient anyway.  Lookup failure means the window died while the
     * event sat in the pipe -- drop it silently. */
    rc = dixLookupWindow(&pWin, (Window) ev->screen, serverClient,
                         DixGetAttrAccess);
    if (rc != Success)
        return;
    vw = vitruvianWindowGet(pWin);
    if (!vw)
        return;

    switch (ev->code) {
    case BE_WINDOW_CLOSE:{
        /* WM behavior: send WM_DELETE_WINDOW if the client opted in via
         * WM_PROTOCOLS, else kill the client (like xkill / a WM would). */
        PropertyPtr pProp;
        Bool wantsDelete = FALSE;

        rc = dixLookupProperty(&pProp, pWin, vitruvianAtomWmProtocols,
                               serverClient, DixReadAccess);
        if (rc == Success && pProp->format == 32 && pProp->data) {
            uint32_t *protos = pProp->data;
            uint32_t i;

            /* format 32 => size counts CARD32 entries (dix/property.c
             * stores size in format/8 units). */
            for (i = 0; i < pProp->size; i++) {
                if ((Atom) protos[i] == vitruvianAtomWmDelete) {
                    wantsDelete = TRUE;
                    break;
                }
            }
        }

        if (wantsDelete) {
            xEvent event = { 0 };

            UpdateCurrentTime();
            /* Same shape and delivery a WM's XSendEvent would produce:
             * ProcSendEvent sets SEND_EVENT_BIT and calls
             * DeliverEventsToWindow with the requested (here: empty) event
             * mask (dix/events.c:5622-5643); ClientMessage is
             * CantBeFiltered (dix/events.c:404), so the empty filter still
             * reaches the window's owning client
             * (DeliverToWindowOwner, dix/events.c:2206). */
            event.u.u.type = ClientMessage | VITRUVIAN_SEND_EVENT_BIT;
            event.u.u.detail = 32;      /* format */
            event.u.clientMessage.window = pWin->drawable.id;
            event.u.clientMessage.u.l.type = vitruvianAtomWmProtocols;
            event.u.clientMessage.u.l.longs0 = (INT32) vitruvianAtomWmDelete;
            event.u.clientMessage.u.l.longs1 =
                (INT32) currentTime.milliseconds;
            (void) DeliverEventsToWindow(inputInfo.pointer, pWin, &event, 1,
                                         NoEventMask, NullGrab);
        }
        else if (wClient(pWin) && wClient(pWin) != serverClient) {
            CloseDownClient(wClient(pWin));
        }
        break;
    }

    case BE_WINDOW_MOVED:{
        /* ev->x/y = new content-area origin in desktop coords == desired
         * drawable.x/y.  ConfigureWindow's CWX/CWY values position the
         * BORDER origin relative to the parent (x is recovered as
         * drawable.x - parent.x - bw, dix/window.c:2211-2214), so subtract
         * borderWidth; the root sits at 0,0.  This re-enters our
         * MoveWindow wrap, whose position compare (plus the shim's own
         * echo guard) makes the resulting beshim_move_window a no-op. */
        int bw = (int) pWin->borderWidth;

        if (pWin->drawable.x == ev->x && pWin->drawable.y == ev->y)
            break;
        vals[0] = (XID) (ev->x - bw);
        vals[1] = (XID) (ev->y - bw);
        (void) ConfigureWindow(pWin, CWX | CWY, vals, serverClient);
        break;
    }

    case BE_WINDOW_RESIZED:
        /* ev->x/y = new content width/height.  vals layout per
         * dix/window.c:2228-2232: CWWidth then CWHeight. */
        if (pWin->drawable.width == ev->x && pWin->drawable.height == ev->y)
            break;
        if (ev->x <= 0 || ev->y <= 0)
            break;
        vals[0] = (XID) ev->x;
        vals[1] = (XID) ev->y;
        (void) ConfigureWindow(pWin, CWWidth | CWHeight, vals, serverClient);
        break;

    case BE_WINDOW_FOCUS_IN:
        /* BeOS activated the window: give it X focus and raise it, like a
         * click-to-focus WM.  Override-redirect windows never get this
         * (B_AVOID_FOCUS), but guard anyway.  SetInputFocus signature per
         * dix/events.c:4935/include/dix.h:496; it fails with BadMatch for
         * unviewable windows, which is fine to ignore.  vals[0] for a lone
         * CWStackMode is the stack mode (dix/window.c:2259-2266). */
        if (pWin->overrideRedirect)
            break;
        (void) SetInputFocus(serverClient, inputInfo.keyboard,
                             pWin->drawable.id, RevertToParent, CurrentTime,
                             FALSE);
        vals[0] = Above;
        (void) ConfigureWindow(pWin, CWStackMode, vals, serverClient);
        break;

    case BE_WINDOW_FOCUS_OUT:
        /* X keeps the last focus until some other window takes it. */
        break;

    default:
        break;
    }
}

/*
 * Per-generation setup, called from vitruvianCreateResources (which runs
 * from dix/main.c:200-211, after InitExtensions at :194 -- i.e. after
 * composite wrapped the screen hooks and before any client can create,
 * let alone map, a window).
 */
Bool
vitruvianRootlessInit(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    VitruvianScrPriv *scrpriv = pScreenPriv->screen->driver;

    /* Private keys are reset every generation; re-registering an already
     * registered key is explicitly allowed (include/privates.h:95). */
    if (!dixRegisterPrivateKey(&vitruvianWindowKeyRec, PRIVATE_WINDOW, 0))
        return FALSE;

    /* The atom table is rebuilt each generation (InitAtoms,
     * dix/main.c:184). */
    vitruvianAtomWmProtocols = MakeAtom("WM_PROTOCOLS", 12, TRUE);
    vitruvianAtomWmDelete = MakeAtom("WM_DELETE_WINDOW", 16, TRUE);
    vitruvianAtomWmName = MakeAtom("WM_NAME", 7, TRUE);
    vitruvianAtomNetWmName = MakeAtom("_NET_WM_NAME", 12, TRUE);
    vitruvianAtomUtf8String = MakeAtom("UTF8_STRING", 11, TRUE);
    vitruvianAtomWmNormalHints = MakeAtom("WM_NORMAL_HINTS", 15, TRUE);
    if (vitruvianAtomWmProtocols == BAD_RESOURCE ||
        vitruvianAtomWmDelete == BAD_RESOURCE ||
        vitruvianAtomWmName == BAD_RESOURCE ||
        vitruvianAtomNetWmName == BAD_RESOURCE ||
        vitruvianAtomUtf8String == BAD_RESOURCE ||
        vitruvianAtomWmNormalHints == BAD_RESOURCE)
        return FALSE;

    /* Callback lists are wiped per generation too (InitCallbackManager,
     * dix/main.c:189 -> DeleteCallbackManager, dix/dixutils.c:892);
     * re-register once per generation. */
    if (vitruvianCallbackGeneration != serverGeneration) {
        if (!AddCallback(&PropertyStateCallback,
                         vitruvianRootlessPropertyCallback, NULL))
            return FALSE;
        vitruvianCallbackGeneration = serverGeneration;
    }

    scrpriv->RealizeWindow = pScreen->RealizeWindow;
    pScreen->RealizeWindow = vitruvianRootlessRealizeWindow;

    scrpriv->UnrealizeWindow = pScreen->UnrealizeWindow;
    pScreen->UnrealizeWindow = vitruvianRootlessUnrealizeWindow;

    scrpriv->DestroyWindow = pScreen->DestroyWindow;
    pScreen->DestroyWindow = vitruvianRootlessDestroyWindow;

    scrpriv->MoveWindow = pScreen->MoveWindow;
    pScreen->MoveWindow = vitruvianRootlessMoveWindow;

    scrpriv->ResizeWindow = pScreen->ResizeWindow;
    pScreen->ResizeWindow = vitruvianRootlessResizeWindow;

    return TRUE;
}

/*
 * CloseScreen-time sweep.  By the time kdrive's CloseScreen runs, dix has
 * already destroyed all client windows (their Unrealize/Destroy wraps
 * emptied the list); anything left is defensive cleanup.  The ScreenRec
 * (and our wraps with it) is torn down wholesale afterwards, so no unwrap
 * is needed.
 */
void
vitruvianRootlessFini(ScreenPtr pScreen)
{
    VitruvianWindow *vw = vitruvianWindows;

    while (vw) {
        VitruvianWindow *next = vw->next;

        if (vw->win && vw->win->drawable.pScreen == pScreen)
            vitruvianRootlessUntrack(vw->win);
        vw = next;
    }
}
