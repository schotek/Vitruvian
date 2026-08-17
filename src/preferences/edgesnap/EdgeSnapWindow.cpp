/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 */

#include "EdgeSnapWindow.h"

#include <Application.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <SeparatorView.h>
#include <StringView.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "EdgeSnap window"


static const uint32 kMsgEnabledToggled = 'entg';
static const uint32 kMsgModifierSelected = 'mdsl';
static const uint32 kMsgSensitivitySelected = 'sssl';
static const uint32 kMsgDefaults = 'dflt';
static const uint32 kMsgRevert = 'rvrt';

static const bool kDefaultEnabled = true;
static const edge_snap_modifier kDefaultModifier = B_EDGE_SNAP_MODIFIER_NONE;
static const edge_snap_sensitivity kDefaultSensitivity
	= B_EDGE_SNAP_SENSITIVITY_MEDIUM;


EdgeSnapWindow::EdgeSnapWindow()
	:
	BWindow(BRect(0, 0, 400, 140),
		B_TRANSLATE_SYSTEM_NAME("EdgeSnap"), B_TITLED_WINDOW,
		B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS),
	fInitialEnabled(edge_snap_enabled()),
	fInitialModifier(get_edge_snap_modifier()),
	fInitialSensitivity(get_edge_snap_sensitivity())
{
	BStringView* header = new BStringView("header",
		B_TRANSLATE("Window snapping"));
	header->SetFont(be_bold_font);

	BStringView* blurb = new BStringView("blurb",
		B_TRANSLATE("Drag a window to the top screen edge to maximize it, "
			"to the left or right edge to fill that half."));

	fEnabledBox = new BCheckBox("enabled",
		B_TRANSLATE("Snap windows dragged to a screen edge"),
		new BMessage(kMsgEnabledToggled));

	// Shift is deliberately not offered: dragging a tab with Shift held
	// already slides the tab along the window border.
	fModifierMenu = new BPopUpMenu("modifier");
	fModifierMenu->AddItem(new BMenuItem(B_TRANSLATE("No key required"),
		new BMessage(kMsgModifierSelected)));
	fModifierMenu->AddItem(new BMenuItem(B_TRANSLATE("Control key"),
		new BMessage(kMsgModifierSelected)));
	fModifierMenu->AddItem(new BMenuItem(B_TRANSLATE("Alt key"),
		new BMessage(kMsgModifierSelected)));

	fModifierField = new BMenuField("modifierfield",
		B_TRANSLATE("Hold modifier key:"), fModifierMenu);

	// How eagerly the zones arm: the cursor-at-edge band and the
	// push-through depth on the sides.
	fSensitivityMenu = new BPopUpMenu("sensitivity");
	fSensitivityMenu->AddItem(new BMenuItem(B_TRANSLATE("Low"),
		new BMessage(kMsgSensitivitySelected)));
	fSensitivityMenu->AddItem(new BMenuItem(B_TRANSLATE("Medium"),
		new BMessage(kMsgSensitivitySelected)));
	fSensitivityMenu->AddItem(new BMenuItem(B_TRANSLATE("High"),
		new BMessage(kMsgSensitivitySelected)));

	fSensitivityField = new BMenuField("sensitivityfield",
		B_TRANSLATE("Snap sensitivity:"), fSensitivityMenu);

	fDefaultsButton = new BButton("defaults", B_TRANSLATE("Defaults"),
		new BMessage(kMsgDefaults));
	fRevertButton = new BButton("revert", B_TRANSLATE("Revert"),
		new BMessage(kMsgRevert));

	BLayoutBuilder::Group<>(this, B_VERTICAL)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(header)
		.Add(blurb)
		.AddStrut(B_USE_DEFAULT_SPACING)
		.Add(fEnabledBox)
		.AddGroup(B_HORIZONTAL)
			.Add(fModifierField)
			.AddGlue()
			.End()
		.AddGroup(B_HORIZONTAL)
			.Add(fSensitivityField)
			.AddGlue()
			.End()
		.AddStrut(B_USE_DEFAULT_SPACING)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.AddGroup(B_HORIZONTAL)
			.Add(fDefaultsButton)
			.Add(fRevertButton)
			.AddGlue()
			.End();

	_ApplyToUI(fInitialEnabled, fInitialModifier, fInitialSensitivity);
	_UpdateButtons();

	CenterOnScreen();
	Show();
}


void
EdgeSnapWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgEnabledToggled:
		{
			bool enabled = fEnabledBox->Value() == B_CONTROL_ON;
			set_edge_snap_enabled(enabled);
			fModifierField->SetEnabled(enabled);
			fSensitivityField->SetEnabled(enabled);
			_UpdateButtons();
			break;
		}

		case kMsgModifierSelected:
			set_edge_snap_modifier(_SelectedModifier());
			_UpdateButtons();
			break;

		case kMsgSensitivitySelected:
			set_edge_snap_sensitivity(_SelectedSensitivity());
			_UpdateButtons();
			break;

		case kMsgDefaults:
			set_edge_snap_enabled(kDefaultEnabled);
			set_edge_snap_modifier(kDefaultModifier);
			set_edge_snap_sensitivity(kDefaultSensitivity);
			_ApplyToUI(kDefaultEnabled, kDefaultModifier,
				kDefaultSensitivity);
			_UpdateButtons();
			break;

		case kMsgRevert:
			set_edge_snap_enabled(fInitialEnabled);
			set_edge_snap_modifier(fInitialModifier);
			set_edge_snap_sensitivity(fInitialSensitivity);
			_ApplyToUI(fInitialEnabled, fInitialModifier,
				fInitialSensitivity);
			_UpdateButtons();
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
EdgeSnapWindow::_ApplyToUI(bool enabled, edge_snap_modifier modifier,
	edge_snap_sensitivity sensitivity)
{
	fEnabledBox->SetValue(enabled ? B_CONTROL_ON : B_CONTROL_OFF);
	fModifierField->SetEnabled(enabled);
	fSensitivityField->SetEnabled(enabled);

	int32 index;
	switch (modifier) {
		case B_EDGE_SNAP_MODIFIER_CONTROL:
			index = 1;
			break;
		case B_EDGE_SNAP_MODIFIER_ALT:
			index = 2;
			break;
		default:
			// Shift has no UI representation; show it as "no key".
			index = 0;
			break;
	}
	if (BMenuItem* item = fModifierMenu->ItemAt(index))
		item->SetMarked(true);

	index = sensitivity;
	if (index < B_EDGE_SNAP_SENSITIVITY_LOW
		|| index > B_EDGE_SNAP_SENSITIVITY_HIGH) {
		index = B_EDGE_SNAP_SENSITIVITY_MEDIUM;
	}
	if (BMenuItem* item = fSensitivityMenu->ItemAt(index))
		item->SetMarked(true);
}


void
EdgeSnapWindow::_UpdateButtons()
{
	bool enabled = fEnabledBox->Value() == B_CONTROL_ON;
	edge_snap_modifier modifier = _SelectedModifier();
	edge_snap_sensitivity sensitivity = _SelectedSensitivity();

	fDefaultsButton->SetEnabled(enabled != kDefaultEnabled
		|| modifier != kDefaultModifier
		|| sensitivity != kDefaultSensitivity);
	fRevertButton->SetEnabled(enabled != fInitialEnabled
		|| modifier != fInitialModifier
		|| sensitivity != fInitialSensitivity);
}


edge_snap_modifier
EdgeSnapWindow::_SelectedModifier() const
{
	switch (fModifierMenu->IndexOf(fModifierMenu->FindMarked())) {
		case 1:
			return B_EDGE_SNAP_MODIFIER_CONTROL;
		case 2:
			return B_EDGE_SNAP_MODIFIER_ALT;
		default:
			return B_EDGE_SNAP_MODIFIER_NONE;
	}
}


edge_snap_sensitivity
EdgeSnapWindow::_SelectedSensitivity() const
{
	switch (fSensitivityMenu->IndexOf(fSensitivityMenu->FindMarked())) {
		case B_EDGE_SNAP_SENSITIVITY_LOW:
			return B_EDGE_SNAP_SENSITIVITY_LOW;
		case B_EDGE_SNAP_SENSITIVITY_HIGH:
			return B_EDGE_SNAP_SENSITIVITY_HIGH;
		default:
			return B_EDGE_SNAP_SENSITIVITY_MEDIUM;
	}
}
