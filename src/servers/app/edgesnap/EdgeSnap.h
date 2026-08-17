/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 */
#ifndef EDGE_SNAP_H
#define EDGE_SNAP_H


#include <map>

#include <InterfaceDefs.h>
#include <Point.h>
#include <Rect.h>

#include "DesktopListener.h"


class Desktop;
class Window;


/*!	Snaps windows dragged to a screen edge: the top edge maximizes the
	window into the work area, the left/right edges tile it to the
	respective half. While a drag hovers over a trigger zone a translucent
	"ghost" rectangle previews the target area; releasing the mouse commits
	the snap, dragging a snapped window away restores its previous frame.
*/
class EdgeSnap : public DesktopListener {
public:
								EdgeSnap();
	virtual						~EdgeSnap();

	virtual int32				Identifier();

	virtual	void				ListenerRegistered(Desktop* desktop);
	virtual	void				ListenerUnregistered();

	virtual bool				HandleMessage(Window* sender,
									BPrivate::LinkReceiver& link,
									BPrivate::LinkSender& reply);

	virtual void				WindowAdded(Window* window);
	virtual void				WindowRemoved(Window* window);

	virtual bool				KeyPressed(uint32 what, int32 key,
									int32 modifiers);
	virtual void				MouseEvent(BMessage* message);
	virtual void				MouseDown(Window* window, BMessage* message,
									const BPoint& where);
	virtual void				MouseUp(Window* window, BMessage* message,
									const BPoint& where);
	virtual void				MouseMoved(Window* window, BMessage* message,
									const BPoint& where);

	virtual void				WindowMoved(Window* window);
	virtual void				WindowResized(Window* window);
	virtual void				WindowActivated(Window* window);
	virtual void				WindowSentBehind(Window* window,
									Window* behindOf);
	virtual void				WindowWorkspacesChanged(Window* window,
									uint32 workspaces);
	virtual void				WindowHidden(Window* window,
									bool fromMinimize);
	virtual void				WindowMinimized(Window* window,
									bool minimize);

	virtual void				WindowTabLocationChanged(Window* window,
									float location, bool isShifting);
	virtual void				SizeLimitsChanged(Window* window,
									int32 minWidth, int32 maxWidth,
									int32 minHeight, int32 maxHeight);
	virtual void				WindowLookChanged(Window* window,
									window_look look);
	virtual void				WindowFeelChanged(Window* window,
									window_feel feel);

	virtual bool				SetDecoratorSettings(Window* window,
									const BMessage& settings);
	virtual void				GetDecoratorSettings(Window* window,
									BMessage& settings);

private:
			enum snap_zone {
				kZoneNone = 0,
				kZoneLeft,
				kZoneRight,
				kZoneTop
			};

			struct SnapInfo {
				BRect			savedFrame;
				BRect			snappedFrame;
			};

			typedef std::map<Window*, SnapInfo> SnapInfoMap;

			bool				_IsWindowEligible(Window* window);
			bool				_ModifierMatches();
			BRect				_ScreenFrame(Window* window);
			BRect				_WorkArea(Window* window);
			BRect				_ZoneFrame(snap_zone zone, Window* window);
			BRect				_TargetFrame(snap_zone zone, Window* window,
									BRect* _decorated);
			snap_zone			_ZoneAt(BPoint where, Window* window);
			snap_zone			_OverdriveZone(BPoint where, Window* window,
									const BRect& screenFrame);
			int32				_Sensitivity();
			void				_UpdateGrabOffsets(Window* window,
									const BPoint& where);
			void				_Arm(snap_zone zone, Window* window);
			void				_Disarm();
			void				_Commit(Window* window, snap_zone zone);
			void				_RestoreWindow(Window* window,
									const BPoint& where);
			void				_DrawGhost();
			void				_EraseGhost();
			void				_StopTracking();

			Desktop*			fDesktop;
			Window*				fDragWindow;
			BPoint				fDragStartPoint;
			snap_zone			fArmedZone;
			float				fGrabOffsetLeft;
			float				fGrabOffsetRight;
			BRect				fGhostFrame;
			bool				fGhostVisible;
			int32				fCurrentModifiers;
			SnapInfoMap			fSnappedWindows;
};


#endif	// EDGE_SNAP_H
