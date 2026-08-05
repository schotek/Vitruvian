/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 */
#ifndef VITRINE_WINDOW_H
#define VITRINE_WINDOW_H


#include <Window.h>

class BCheckBox;
class BStringView;


class VitrineWindow : public BWindow {
public:
							VitrineWindow();
	virtual					~VitrineWindow();

	virtual void			MessageReceived(BMessage* message);
	virtual bool			QuitRequested();

private:
			bool			_ReadAutostart() const;
			void			_WriteAutostart(bool enabled) const;
			void			_UpdateStatus();

			BCheckBox*		fAutostartBox;
			BStringView*	fStatus;
			bool			fInstalled;
};


#endif	/* VITRINE_WINDOW_H */
