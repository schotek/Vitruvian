/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 */

#include "VitrineWindow.h"

#include "VitrineSettings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <Deskbar.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>
#include <StringView.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Vitrine window"


static const uint32 kMsgAutostartToggled = 'atgl';
static const uint32 kMsgStartVitrine = 'strt';
static const uint32 kMsgRefreshStatus = 'rfsh';
static const uint32 kMsgTrayToggled = 'tray';


static bool
settings_path(BPath& path)
{
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return false;
	return path.Append(VITRINE_SETTINGS_FILE) == B_OK;
}


VitrineWindow::VitrineWindow()
	:
	BWindow(BRect(0, 0, 460, 200),
		B_TRANSLATE_SYSTEM_NAME("VitrineSettings"), B_TITLED_WINDOW,
		B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS),
	fInstalled(access(VITRINE_BINARY_PATH, X_OK) == 0)
{
	BStringView* header = new BStringView("header",
		B_TRANSLATE("Vitrine — Wayland and X11 application support"));
	header->SetFont(be_bold_font);

	BStringView* blurb = new BStringView("blurb",
		B_TRANSLATE("Hosts Wayland and X11 applications as native windows "
			"on the desktop."));

	fAutostartBox = new BCheckBox("autostart",
		B_TRANSLATE("Start Vitrine when logging in"),
		new BMessage(kMsgAutostartToggled));

	/* The Deskbar tray icon (src/apps/vitrinetray) — the persistent handle
	 * for a nested server that deliberately has no entry among the
	 * applications. */
	fTrayBox = new BCheckBox("tray",
		B_TRANSLATE("Show a control icon in the Deskbar tray"),
		new BMessage(kMsgTrayToggled));

	fStatus = new BStringView("status", "");

	/* Manual launch for the autostart-off case: the roster resolves the
	 * signature (it scans /system/servers too) and B_EXCLUSIVE_LAUNCH in
	 * the rdef makes a second start a no-op, so the button is safe to
	 * press at any time. */
	fStartButton = new BButton("start", B_TRANSLATE("Start now"),
		new BMessage(kMsgStartVitrine));

	/* B_ABOUT_REQUESTED goes to the application, which owns the panel (see
	 * VitrineApp::AboutRequested) — the same handler the Deskbar's About
	 * item reaches. */
	BButton* aboutButton = new BButton("about",
		B_TRANSLATE("About" B_UTF8_ELLIPSIS),
		new BMessage(B_ABOUT_REQUESTED));
	aboutButton->SetTarget(be_app);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_ITEM_SPACING)
		.SetInsets(B_USE_WINDOW_INSETS)
		.Add(header)
		.Add(blurb)
		.Add(fAutostartBox)
		.Add(fTrayBox)
		.Add(fStatus)
		.AddGlue()
		.AddGroup(B_HORIZONTAL)
			.Add(fStartButton)
			.AddGlue()
			.Add(aboutButton)
		.End();

	fAutostartBox->SetValue(_ReadAutostart() ? B_CONTROL_ON : B_CONTROL_OFF);
	fTrayBox->SetValue(BDeskbar().HasItem(VITRINE_TRAY_ITEM_NAME)
		? B_CONTROL_ON : B_CONTROL_OFF);
	if (!fInstalled)
		fAutostartBox->SetEnabled(false);
	if (access(VITRINE_TRAY_PATH, X_OK) != 0)
		fTrayBox->SetEnabled(false);
	_UpdateStatus();

	CenterOnScreen();
	Show();
}


VitrineWindow::~VitrineWindow()
{
}


void
VitrineWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgAutostartToggled:
			_WriteAutostart(fAutostartBox->Value() == B_CONTROL_ON);
			_UpdateStatus();
			break;

		case kMsgStartVitrine:
		{
			status_t status = be_roster->Launch(VITRINE_SIGNATURE);
			if (status != B_OK && status != B_ALREADY_RUNNING) {
				BString text(B_TRANSLATE("Could not start Vitrine: %error%"));
				text.ReplaceFirst("%error%", strerror(status));
				fStatus->SetText(text);
				break;
			}
			_UpdateStatus();
			/* The roster registration lags the launch by a moment, so
			 * IsRunning() can still say "not running" — refresh once more
			 * shortly. Fire-and-forget one-shot; no cleanup to track. */
			BMessageRunner::StartSending(BMessenger(this),
				new BMessage(kMsgRefreshStatus), 2000000, 1);
			break;
		}

		case kMsgRefreshStatus:
			_UpdateStatus();
			break;

		case kMsgTrayToggled:
		{
			/* Toggling from OUR team is safe inline — the deadlock trap is
			 * only RemoveItem() called from inside Deskbar itself (see the
			 * tray view). AddItem must use the entry_ref variant: it is the
			 * only one Deskbar persists across restarts. */
			BDeskbar deskbar;
			status_t status;
			if (fTrayBox->Value() == B_CONTROL_ON) {
				entry_ref ref;
				status = be_roster->FindApp(VITRINE_TRAY_SIGNATURE, &ref);
				if (status == B_OK)
					status = deskbar.AddItem(&ref);
			} else {
				status = deskbar.RemoveItem(VITRINE_TRAY_ITEM_NAME);
			}
			if (status != B_OK) {
				fTrayBox->SetValue(
					deskbar.HasItem(VITRINE_TRAY_ITEM_NAME)
						? B_CONTROL_ON : B_CONTROL_OFF);
				BString text(
					B_TRANSLATE("Changing the tray icon failed: %error%"));
				text.ReplaceFirst("%error%", strerror(status));
				fStatus->SetText(text);
			}
			break;
		}

		default:
			BWindow::MessageReceived(message);
	}
}


bool
VitrineWindow::QuitRequested()
{
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}


/*!	Reads the autostart flag. Anything unparsable — including a missing
	file — means the default, which is on: an image that ships Vitrine
	starts it unless the user opted out.
*/
bool
VitrineWindow::_ReadAutostart() const
{
	BPath path;
	if (!settings_path(path))
		return true;

	FILE* file = fopen(path.Path(), "r");
	if (file == NULL)
		return true;

	bool enabled = true;
	char line[256];
	while (fgets(line, sizeof(line), file) != NULL) {
		char* p = line;
		while (isspace(*p))
			p++;
		if (*p == '#' || strncmp(p, "autostart", 9) != 0)
			continue;
		p += 9;
		while (isspace(*p))
			p++;
		if (*p != '=')
			continue;
		p++;
		while (isspace(*p))
			p++;
		enabled = !(strncasecmp(p, "false", 5) == 0
			|| strncasecmp(p, "no", 2) == 0
			|| strncasecmp(p, "off", 3) == 0
			|| *p == '0');
	}
	fclose(file);
	return enabled;
}


void
VitrineWindow::_WriteAutostart(bool enabled) const
{
	BPath path;
	if (!settings_path(path))
		return;

	FILE* file = fopen(path.Path(), "w");
	if (file == NULL)
		return;
	fprintf(file,
		"# Vitrine — nested Wayland compositor.\n"
		"# Written by the Vitrine preferences panel; read by janus and\n"
		"# vos-session-boot at login. Delete this file to restore defaults.\n"
		"autostart = %s\n", enabled ? "true" : "false");
	fclose(file);
}


void
VitrineWindow::_UpdateStatus()
{
	if (!fInstalled) {
		fStatus->SetText(
			B_TRANSLATE("Vitrine is not installed in this system image."));
		fStartButton->SetEnabled(false);
		return;
	}

	bool running = be_roster->IsRunning(VITRINE_SIGNATURE);
	fStartButton->SetEnabled(!running);
	if (running) {
		fStatus->SetText(fAutostartBox->Value() == B_CONTROL_ON
			? B_TRANSLATE("Currently running.")
			: B_TRANSLATE("Currently running; it will not start at the next "
				"login."));
	} else {
		fStatus->SetText(fAutostartBox->Value() == B_CONTROL_ON
			? B_TRANSLATE("Not running; it will start at the next login.")
			: B_TRANSLATE("Not running."));
	}
}
