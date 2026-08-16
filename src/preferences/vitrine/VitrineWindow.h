/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 */
#ifndef VITRINE_WINDOW_H
#define VITRINE_WINDOW_H


#include <String.h>
#include <Window.h>

class BButton;
class BCheckBox;
class BMenuField;
class BPopUpMenu;
class BStringView;


class VitrineWindow : public BWindow {
public:
							VitrineWindow();
	virtual					~VitrineWindow();

	virtual void			MessageReceived(BMessage* message);
	virtual bool			QuitRequested();

private:
			bool			_ReadString(const char* key,
								BString& value) const;
			bool			_ReadBool(const char* key,
								bool defaultValue) const;
			void			_WriteSettings() const;
			void			_BuildThemeMenu();
			void			_UpdateStatus();

			BCheckBox*		fAutostartBox;
			BCheckBox*		fSeparateBox;
			BCheckBox*		fTrayBox;
			BStringView*	fStatus;
			BButton*		fStartButton;
			BMenuField*		fThemeField;
			BPopUpMenu*		fThemeMenu;
			BString			fGtkTheme;
			bool			fInstalled;
};


#endif	/* VITRINE_WINDOW_H */
