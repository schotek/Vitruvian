/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 */
#ifndef EDGE_SNAP_WINDOW_H
#define EDGE_SNAP_WINDOW_H


#include <InterfaceDefs.h>
#include <Window.h>


class BButton;
class BCheckBox;
class BMenuField;
class BPopUpMenu;


class EdgeSnapWindow : public BWindow {
public:
								EdgeSnapWindow();

	virtual	void				MessageReceived(BMessage* message);

private:
			void				_ApplyToUI(bool enabled,
									edge_snap_modifier modifier,
									edge_snap_sensitivity sensitivity);
			void				_UpdateButtons();
			edge_snap_modifier	_SelectedModifier() const;
			edge_snap_sensitivity	_SelectedSensitivity() const;

			BCheckBox*			fEnabledBox;
			BMenuField*			fModifierField;
			BPopUpMenu*			fModifierMenu;
			BMenuField*			fSensitivityField;
			BPopUpMenu*			fSensitivityMenu;
			BButton*			fDefaultsButton;
			BButton*			fRevertButton;

			bool				fInitialEnabled;
			edge_snap_modifier	fInitialModifier;
			edge_snap_sensitivity	fInitialSensitivity;
};


#endif	// EDGE_SNAP_WINDOW_H
