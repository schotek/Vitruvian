/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 */
#ifndef VITRINE_TRAY_VIEW_H
#define VITRINE_TRAY_VIEW_H


#include <View.h>

class BBitmap;


class VitrineTrayView : public BView {
public:
								VitrineTrayView(BRect frame);
								VitrineTrayView(BMessage* archive);
	virtual						~VitrineTrayView();

	static	VitrineTrayView*	Instantiate(BMessage* archive);
	virtual	status_t			Archive(BMessage* archive,
									bool deep = true) const;

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MessageReceived(BMessage* message);

private:
			void				_Init();
			void				_LoadIcon();
			void				_UpdateRunning(bool invalidate);
			void				_ShowMenu(BPoint where);
	static	int32				_RemoveFromDeskbarThread(void* data);

			BBitmap*			fIcon;
			bool				fRunning;
			bool				fInDeskbar;
};


#endif	/* VITRINE_TRAY_VIEW_H */
