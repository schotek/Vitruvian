/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 */
#ifndef VITRINE_WINDOW_H
#define VITRINE_WINDOW_H


#include <Window.h>

class BButton;
class BCheckBox;
class BStringView;


class VitrineWindow : public BWindow {
public:
							VitrineWindow();
	virtual					~VitrineWindow();

	virtual void			MessageReceived(BMessage* message);
	virtual bool			QuitRequested();

private:
			bool			_ReadBool(const char* key,
								bool defaultValue) const;
			void			_WriteSettings() const;
			void			_UpdateStatus();

			BCheckBox*		fAutostartBox;
			BCheckBox*		fSeparateBox;
			BCheckBox*		fTrayBox;
			BStringView*	fStatus;
			BButton*		fStartButton;
			bool			fInstalled;
};


#endif	/* VITRINE_WINDOW_H */
