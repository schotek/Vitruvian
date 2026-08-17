/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 */


#include "EdgeSnap.h"

#include <string.h>

#include <Message.h>
#include <WindowPrivate.h>

#include "Decorator.h"
#include "Desktop.h"
#include "DesktopSettings.h"
#include "SATGroup.h"
#include "SATWindow.h"
#include "Screen.h"
#include "StackAndTile.h"
#include "Window.h"
#include "drawing/DrawingEngine.h"


// Cursor distance from a screen edge that arms a snap zone, indexed by
// edge_snap_sensitivity (low, medium, high).
static const float kSnapTriggerDistance[3] = { 4.0f, 8.0f, 16.0f };
// Decorated-frame distance from a side edge that counts as contact.
static const float kEdgeContactSlack = 1.0f;
// Extra cursor travel towards a side edge, past the point where the
// window frame reached it, that arms the zone ("pushing through") —
// less sensitivity means a deeper push. Indexed like above.
static const float kSideOverdrive[3] = { 30.0f, 15.0f, 8.0f };
// Cursor travel from the drag start before a snapped window restores.
static const float kRestoreMouseThreshold = 8.0f;
// Gap kept around the Deskbar, matching BWindow::Zoom().
static const float kDeskbarGap = 2.0f;

// Windows that cannot be moved or fully resized are not snappable.
static const uint32 kNonSnappableFlags = B_NOT_MOVABLE | B_NOT_RESIZABLE
	| B_NOT_H_RESIZABLE | B_NOT_V_RESIZABLE;


static uint32
modifier_mask_for(edge_snap_modifier modifier)
{
	switch (modifier) {
		case B_EDGE_SNAP_MODIFIER_SHIFT:
			return B_SHIFT_KEY;
		case B_EDGE_SNAP_MODIFIER_CONTROL:
			return B_CONTROL_KEY;
		case B_EDGE_SNAP_MODIFIER_ALT:
			return B_COMMAND_KEY;
		case B_EDGE_SNAP_MODIFIER_NONE:
		default:
			return 0;
	}
}


EdgeSnap::EdgeSnap()
	:
	fDesktop(NULL),
	fDragWindow(NULL),
	fDragStartPoint(B_ORIGIN),
	fArmedZone(kZoneNone),
	fGrabOffsetLeft(0),
	fGrabOffsetRight(0),
	fGhostFrame(),
	fGhostVisible(false),
	fCurrentModifiers(0)
{
}


EdgeSnap::~EdgeSnap()
{
}


int32
EdgeSnap::Identifier()
{
	return 'edsn';
}


void
EdgeSnap::ListenerRegistered(Desktop* desktop)
{
	fDesktop = desktop;
}


void
EdgeSnap::ListenerUnregistered()
{
	fSnappedWindows.clear();
	_StopTracking();
	fDesktop = NULL;
}


bool
EdgeSnap::HandleMessage(Window* sender, BPrivate::LinkReceiver& link,
	BPrivate::LinkSender& reply)
{
	return false;
}


void
EdgeSnap::WindowAdded(Window* window)
{
}


void
EdgeSnap::WindowRemoved(Window* window)
{
	fSnappedWindows.erase(window);
	if (window == fDragWindow)
		_StopTracking();
}


bool
EdgeSnap::KeyPressed(uint32 what, int32 key, int32 modifiers)
{
	if (what == B_MODIFIERS_CHANGED) {
		fCurrentModifiers = modifiers;
		if (fArmedZone != kZoneNone && (!_ModifierMatches()
				|| fDesktop->GetStackAndTile()->SATKeyPressed())) {
			// The modifier rule broke mid-drag, or Stack & Tile took over.
			_Disarm();
		}
	}
	return false;
}


void
EdgeSnap::MouseEvent(BMessage* message)
{
}


void
EdgeSnap::MouseDown(Window* window, BMessage* message, const BPoint& where)
{
	_StopTracking();

	int32 buttons = message->FindInt32("buttons");
	if ((buttons & B_PRIMARY_MOUSE_BUTTON) == 0)
		return;

	// Only track actual window drags (the behaviour's drag state was
	// already entered when this notification fires).
	if (!window->IsDragging())
		return;

	if (!_IsWindowEligible(window))
		return;

	int32 modifiers;
	if (message->FindInt32("modifiers", &modifiers) == B_OK)
		fCurrentModifiers = modifiers;

	fDragWindow = window;
	fDragStartPoint = where;
	_UpdateGrabOffsets(window, where);
}


void
EdgeSnap::MouseUp(Window* window, BMessage* message, const BPoint& where)
{
	if (fDragWindow == NULL || window != fDragWindow)
		return;

	// Only a released primary button ends the drag; note the behaviour's
	// drag state is already gone when this notification fires.
	int32 buttons = message->FindInt32("buttons");
	if ((buttons & B_PRIMARY_MOUSE_BUTTON) != 0)
		return;

	if (fArmedZone != kZoneNone) {
		snap_zone zone = fArmedZone;
		_Disarm();
		_Commit(window, zone);
	} else {
		// The window was dragged without snapping; if it was snapped
		// before and has left its snapped frame, it is free again.
		SnapInfoMap::iterator it = fSnappedWindows.find(window);
		if (it != fSnappedWindows.end()
			&& window->Frame() != it->second.snappedFrame) {
			fSnappedWindows.erase(it);
		}
	}

	_StopTracking();
}


void
EdgeSnap::MouseMoved(Window* window, BMessage* message, const BPoint& where)
{
	if (fDragWindow == NULL || window != fDragWindow)
		return;

	if (!window->IsDragging()) {
		// The drag ended some other way (state change); stop tracking.
		_StopTracking();
		return;
	}

	int32 modifiers;
	if (message->FindInt32("modifiers", &modifiers) == B_OK)
		fCurrentModifiers = modifiers;

	// Dragging a snapped window away restores its remembered frame.
	SnapInfoMap::iterator it = fSnappedWindows.find(window);
	if (it != fSnappedWindows.end()) {
		BPoint travel = where - fDragStartPoint;
		if (travel.x * travel.x + travel.y * travel.y
				> kRestoreMouseThreshold * kRestoreMouseThreshold) {
			_RestoreWindow(window, where);
		}
	}

	snap_zone zone = _ZoneAt(where, window);
	if (zone == fArmedZone)
		return;

	_Disarm();
	if (zone != kZoneNone)
		_Arm(zone, window);
}


void
EdgeSnap::WindowMoved(Window* window)
{
	if (window == fDragWindow) {
		// Each move step repaints the area under the ghost; draw it again.
		if (fGhostVisible)
			_DrawGhost();
		return;
	}

	// A programmatic move of a snapped window frees it.
	SnapInfoMap::iterator it = fSnappedWindows.find(window);
	if (it != fSnappedWindows.end()
		&& window->Frame() != it->second.snappedFrame) {
		fSnappedWindows.erase(it);
	}
}


void
EdgeSnap::WindowResized(Window* window)
{
	// Our own commits run under the notification guard and do not get
	// here; any other resize frees the window.
	SnapInfoMap::iterator it = fSnappedWindows.find(window);
	if (it != fSnappedWindows.end()
		&& window->Frame() != it->second.snappedFrame) {
		fSnappedWindows.erase(it);
	}
}


void
EdgeSnap::WindowActivated(Window* window)
{
}


void
EdgeSnap::WindowSentBehind(Window* window, Window* behindOf)
{
}


void
EdgeSnap::WindowWorkspacesChanged(Window* window, uint32 workspaces)
{
}


void
EdgeSnap::WindowHidden(Window* window, bool fromMinimize)
{
	if (window == fDragWindow)
		_StopTracking();
}


void
EdgeSnap::WindowMinimized(Window* window, bool minimize)
{
	if (minimize && window == fDragWindow)
		_StopTracking();
}


void
EdgeSnap::WindowTabLocationChanged(Window* window, float location,
	bool isShifting)
{
}


void
EdgeSnap::SizeLimitsChanged(Window* window, int32 minWidth, int32 maxWidth,
	int32 minHeight, int32 maxHeight)
{
}


void
EdgeSnap::WindowLookChanged(Window* window, window_look look)
{
}


void
EdgeSnap::WindowFeelChanged(Window* window, window_feel feel)
{
	if (feel != B_NORMAL_WINDOW_FEEL) {
		fSnappedWindows.erase(window);
		if (window == fDragWindow)
			_StopTracking();
	}
}


bool
EdgeSnap::SetDecoratorSettings(Window* window, const BMessage& settings)
{
	return false;
}


void
EdgeSnap::GetDecoratorSettings(Window* window, BMessage& settings)
{
}


// #pragma mark - private


bool
EdgeSnap::_IsWindowEligible(Window* window)
{
	DesktopSettings settings(fDesktop);
	if (!settings.EdgeSnapEnabled())
		return false;

	if (window->Feel() != B_NORMAL_WINDOW_FEEL)
		return false;

	if ((window->Flags() & kNonSnappableFlags) != 0)
		return false;

	// Skip the desktop furniture (Stack & Tile precedent).
	if (strcmp(window->Title(), "Deskbar") == 0
		|| strcmp(window->Title(), "Desktop") == 0) {
		return false;
	}

	// Windows in a Stack & Tile group are managed by their group.
	SATWindow* satWindow
		= fDesktop->GetStackAndTile()->GetSATWindow(window);
	if (satWindow != NULL && satWindow->GetGroup() != NULL
		&& satWindow->GetGroup()->CountItems() > 1) {
		return false;
	}

	return true;
}


bool
EdgeSnap::_ModifierMatches()
{
	DesktopSettings settings(fDesktop);
	uint32 mask = modifier_mask_for(settings.EdgeSnapModifier());
	if (mask == 0)
		return true;
	return (fCurrentModifiers & mask) != 0;
}


BRect
EdgeSnap::_ScreenFrame(Window* window)
{
	const ::Screen* screen = window->Screen();
	if (screen != NULL)
		return screen->Frame();
	return fDesktop->VirtualScreen().Frame();
}


/*!	Returns the screen frame minus the Deskbar strip, mirroring what
	BWindow::Zoom() does on the client side. The Deskbar has no special
	feel or look, so it is identified by its (untranslated) title and its
	location is derived from its geometry.
*/
BRect
EdgeSnap::_WorkArea(Window* window)
{
	BRect screenFrame = _ScreenFrame(window);
	BRect workArea = screenFrame;

	Window* deskbar = NULL;
	for (Window* other = fDesktop->CurrentWindows().FirstWindow();
			other != NULL; other = other->NextWindow(fDesktop->CurrentWorkspace())) {
		if (!other->IsHidden() && strcmp(other->Title(), "Deskbar") == 0) {
			deskbar = other;
			break;
		}
	}

	if (deskbar == NULL)
		return workArea;

	BRect frame = deskbar->Frame();
	if (Decorator* decorator = deskbar->Decorator())
		frame = decorator->GetFootprint().Frame();

	if (frame.Width() < 16 || frame.Height() < 16) {
		// Probably auto-hidden; do not reserve space for it.
		return workArea;
	}

	if (frame.Width() >= 0.8f * screenFrame.Width()) {
		// Horizontal bar: reserve the top or bottom strip.
		if (frame.top - screenFrame.top < screenFrame.bottom - frame.bottom)
			workArea.top = frame.bottom + kDeskbarGap;
		else
			workArea.bottom = frame.top - kDeskbarGap;
	} else {
		// Vertical bar: reserve the left or right strip.
		if (frame.left - screenFrame.left
				< screenFrame.right - frame.right) {
			workArea.left = frame.right + kDeskbarGap;
		} else
			workArea.right = frame.left - kDeskbarGap;
	}

	return workArea;
}


BRect
EdgeSnap::_ZoneFrame(snap_zone zone, Window* window)
{
	BRect workArea = _WorkArea(window);

	switch (zone) {
		case kZoneLeft:
		{
			BRect frame = workArea;
			frame.right = floorf((workArea.left + workArea.right) / 2);
			return frame;
		}
		case kZoneRight:
		{
			BRect frame = workArea;
			frame.left = floorf((workArea.left + workArea.right) / 2) + 1;
			return frame;
		}
		case kZoneTop:
		default:
			return workArea;
	}
}


EdgeSnap::snap_zone
EdgeSnap::_ZoneAt(BPoint where, Window* window)
{
	if (!_ModifierMatches())
		return kZoneNone;

	// Never compete with Stack & Tile.
	if (fDesktop->GetStackAndTile()->SATKeyPressed())
		return kZoneNone;

	// The feature may have been switched off mid-drag.
	if (!_IsWindowEligible(window))
		return kZoneNone;

	BRect screenFrame = _ScreenFrame(window);
	float triggerDistance = kSnapTriggerDistance[_Sensitivity()];
	snap_zone zone = kZoneNone;
	if (where.x <= screenFrame.left + triggerDistance)
		zone = kZoneLeft;
	else if (where.x >= screenFrame.right - triggerDistance)
		zone = kZoneRight;
	else if (where.y <= screenFrame.top + triggerDistance)
		zone = kZoneTop;

	if (zone == kZoneNone)
		zone = _OverdriveZone(where, window, screenFrame);

	return zone;
}


/*!	Side zones are also armed by "pushing through": while the dragged
	window's decorated frame touches a side edge (which is where
	MagneticBorder parks it), moving the cursor another kSideOverdrive
	towards that edge arms the zone. The cursor itself rarely reaches a
	side edge: it grips the tab at an arbitrary horizontal offset, so the
	window arrives (and magnetically sticks) long before the cursor would
	— unlike the top edge, where the tab keeps the cursor within a few
	pixels of the window's top border.

	The push depth is derived from the grab offset captured at mouse-down
	(cursor at "screen edge + grab offset" is exactly where the window
	frame reaches the edge), so it is independent of how coarsely the
	mouse events arrive.
*/
EdgeSnap::snap_zone
EdgeSnap::_OverdriveZone(BPoint where, Window* window,
	const BRect& screenFrame)
{
	BRect footprint = window->Frame();
	if (Decorator* decorator = window->Decorator())
		footprint = decorator->GetFootprint().Frame();

	float overdrive = kSideOverdrive[_Sensitivity()];
	if (footprint.left <= screenFrame.left + kEdgeContactSlack) {
		if ((screenFrame.left + fGrabOffsetLeft) - where.x >= overdrive)
			return kZoneLeft;
	} else if (footprint.right >= screenFrame.right - kEdgeContactSlack) {
		if (where.x - (screenFrame.right - fGrabOffsetRight) >= overdrive)
			return kZoneRight;
	}

	return kZoneNone;
}


/*!	Returns the configured sensitivity clamped to the constant tables'
	bounds (settings loaded from disk are range-checked already, this
	guards the array access).
*/
int32
EdgeSnap::_Sensitivity()
{
	DesktopSettings settings(fDesktop);
	int32 sensitivity = settings.EdgeSnapSensitivity();
	if (sensitivity < B_EDGE_SNAP_SENSITIVITY_LOW
		|| sensitivity > B_EDGE_SNAP_SENSITIVITY_HIGH) {
		return B_EDGE_SNAP_SENSITIVITY_MEDIUM;
	}
	return sensitivity;
}


void
EdgeSnap::_UpdateGrabOffsets(Window* window, const BPoint& where)
{
	BRect footprint = window->Frame();
	if (Decorator* decorator = window->Decorator())
		footprint = decorator->GetFootprint().Frame();

	fGrabOffsetLeft = where.x - footprint.left;
	fGrabOffsetRight = footprint.right - where.x;
}


/*!	Computes the client frame the window would get in the given zone:
	the decorated footprint fills the zone frame, clamped to the window's
	size limits. Horizontally the clamped frame stays anchored to the
	zone's screen edge (right zone anchors right). \a _decorated, if not
	NULL, receives the decorated footprint of the result — this is what
	the ghost previews, so the preview stays honest for windows whose
	limits keep them from filling the whole zone.
*/
BRect
EdgeSnap::_TargetFrame(snap_zone zone, Window* window, BRect* _decorated)
{
	BRect zoneFrame = _ZoneFrame(zone, window);
	BRect frame = window->Frame();
	BRect footprint = frame;
	if (Decorator* decorator = window->Decorator())
		footprint = decorator->GetFootprint().Frame();

	// insets of the client frame within the decorated footprint
	float left = frame.left - footprint.left;
	float top = frame.top - footprint.top;
	float right = footprint.right - frame.right;
	float bottom = footprint.bottom - frame.bottom;

	BRect target(zoneFrame.left + left, zoneFrame.top + top,
		zoneFrame.right - right, zoneFrame.bottom - bottom);

	int32 minWidth, maxWidth, minHeight, maxHeight;
	window->GetSizeLimits(&minWidth, &maxWidth, &minHeight, &maxHeight);

	float width = target.Width();
	if (width < minWidth)
		width = minWidth;
	if (width > maxWidth)
		width = maxWidth;
	if (zone == kZoneRight)
		target.left = target.right - width;
	else
		target.right = target.left + width;

	float height = target.Height();
	if (height < minHeight)
		height = minHeight;
	if (height > maxHeight)
		height = maxHeight;
	target.bottom = target.top + height;

	if (_decorated != NULL) {
		*_decorated = target;
		_decorated->left -= left;
		_decorated->top -= top;
		_decorated->right += right;
		_decorated->bottom += bottom;
	}

	return target;
}


void
EdgeSnap::_Arm(snap_zone zone, Window* window)
{
	fArmedZone = zone;
	_TargetFrame(zone, window, &fGhostFrame);
	_DrawGhost();
}


void
EdgeSnap::_Disarm()
{
	fArmedZone = kZoneNone;
	_EraseGhost();
}


/*!	Applies the zone: positions the window so that its decorated
	footprint fills the zone frame, within the window's size limits.
*/
void
EdgeSnap::_Commit(Window* window, snap_zone zone)
{
	BRect target = _TargetFrame(zone, window, NULL);
	BRect frame = window->Frame();

	BRect saved = frame;
	fDesktop->MoveWindowBy(window, target.left - frame.left,
		target.top - frame.top);
	frame = window->Frame();
	fDesktop->ResizeWindowBy(window, target.Width() - frame.Width(),
		target.Height() - frame.Height());

	SnapInfo info;
	info.savedFrame = saved;
	SnapInfoMap::iterator it = fSnappedWindows.find(window);
	if (it != fSnappedWindows.end() && saved == it->second.snappedFrame) {
		// Re-snapping a window that is still in its snapped frame: keep
		// the original pre-snap frame as the restore target.
		info.savedFrame = it->second.savedFrame;
	}
	info.snappedFrame = window->Frame();
	fSnappedWindows[window] = info;
}


/*!	Restores a snapped window to its remembered size mid-drag, keeping
	the cursor at the same relative position on the tab.
*/
void
EdgeSnap::_RestoreWindow(Window* window, const BPoint& where)
{
	SnapInfoMap::iterator it = fSnappedWindows.find(window);
	if (it == fSnappedWindows.end())
		return;

	BRect frame = window->Frame();
	BRect saved = it->second.savedFrame;
	fSnappedWindows.erase(it);

	if (frame.Width() <= 0 || saved.Width() <= 0)
		return;

	float relative = (where.x - frame.left) / frame.Width();
	if (relative < 0)
		relative = 0;
	if (relative > 1)
		relative = 1;

	fDesktop->ResizeWindowBy(window, saved.Width() - frame.Width(),
		saved.Height() - frame.Height());
	fDesktop->MoveWindowBy(window,
		(where.x - relative * saved.Width()) - frame.left, 0);

	// The window changed size and position under the cursor.
	_UpdateGrabOffsets(window, where);
}


void
EdgeSnap::_DrawGhost()
{
	if (fDragWindow == NULL || !fGhostFrame.IsValid())
		return;

	DrawingEngine* engine = fDesktop->GetDrawingEngine();
	if (engine == NULL || !engine->LockParallelAccess())
		return;

	// Leave the dragged window visible through the ghost.
	BRegion clipping(fGhostFrame);
	clipping.Exclude(&fDragWindow->VisibleRegion());
	engine->ConstrainClippingRegion(&clipping);

	drawing_mode oldMode;
	engine->SetDrawingMode(B_OP_ALPHA, oldMode);
	engine->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	engine->SetPenSize(1.0f);

	// Match Tracker's transparent selection rectangle.
	DesktopSettings settings(fDesktop);
	rgb_color stroke = settings.UIColor(B_NAVIGATION_BASE_COLOR);
	stroke.alpha = 128;
	engine->SetHighColor(stroke);
	engine->StrokeRect(fGhostFrame);

	BRect interior = fGhostFrame;
	interior.InsetBy(1, 1);
	if (interior.IsValid()) {
		rgb_color fill = settings.UIColor(B_CONTROL_HIGHLIGHT_COLOR);
		fill.alpha = 90;
		engine->SetHighColor(fill);
		engine->FillRect(interior);
	}

	engine->SetDrawingMode(oldMode);

	// Painter cannot take a NULL clipping region; restore "no clipping"
	// as a constraint to the whole screen instead.
	BRegion screenRegion(_ScreenFrame(fDragWindow));
	engine->ConstrainClippingRegion(&screenRegion);
	engine->UnlockParallelAccess();

	fGhostVisible = true;
}


void
EdgeSnap::_EraseGhost()
{
	if (!fGhostVisible)
		return;

	fGhostVisible = false;

	BRegion dirty(fGhostFrame);
	fDesktop->MarkDirty(dirty);

	// MarkDirty() only notifies windows; without a desktop window (no
	// Tracker) the background must be refilled explicitly.
	Window* bottom = fDesktop->CurrentWindows().FirstWindow();
	if (bottom == NULL || bottom->Feel() != kDesktopWindowFeel)
		fDesktop->RedrawBackground();
}


void
EdgeSnap::_StopTracking()
{
	if (fArmedZone != kZoneNone)
		_Disarm();
	else if (fGhostVisible)
		_EraseGhost();
	fDragWindow = NULL;
	fDragStartPoint = B_ORIGIN;
	fGrabOffsetLeft = 0;
	fGrabOffsetRight = 0;
}
