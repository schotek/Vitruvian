/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 */

#include "VitrineWindow.h"

#include "VitrineSettings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <Deskbar.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Invoker.h>
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
static const uint32 kMsgSeparateToggled = 'sepw';
static const uint32 kMsgRestartAnswer = 'rsta';


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

	/* ČÁST 3: the per-window helper mode. Each guest application then
	 * appears as its own entry among the Deskbar applications (and the
	 * Vitrine row disappears — the helper teams own all user-visible
	 * windows). Read by the compositor at start; toggling offers a
	 * restart. */
	fSeparateBox = new BCheckBox("separate",
		B_TRANSLATE("Show windows of applications separately"),
		new BMessage(kMsgSeparateToggled));

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
		.Add(fSeparateBox)
		.Add(fTrayBox)
		.Add(fStatus)
		.AddGlue()
		.AddGroup(B_HORIZONTAL)
			.Add(fStartButton)
			.AddGlue()
			.Add(aboutButton)
		.End();

	fAutostartBox->SetValue(_ReadBool("autostart", true)
		? B_CONTROL_ON : B_CONTROL_OFF);
	fSeparateBox->SetValue(_ReadBool("separate_windows", false)
		? B_CONTROL_ON : B_CONTROL_OFF);
	fTrayBox->SetValue(BDeskbar().HasItem(VITRINE_TRAY_ITEM_NAME)
		? B_CONTROL_ON : B_CONTROL_OFF);
	if (!fInstalled) {
		fAutostartBox->SetEnabled(false);
		fSeparateBox->SetEnabled(false);
	}
	if (access(VITRINE_WINHOST_PATH, X_OK) != 0)
		fSeparateBox->SetEnabled(false);
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
			_WriteSettings();
			_UpdateStatus();
			break;

		case kMsgSeparateToggled:
		{
			_WriteSettings();
			_UpdateStatus();
			if (!be_roster->IsRunning(VITRINE_SIGNATURE))
				break;
			/* The compositor reads the setting at start — offer the
			 * restart. Asynchronous by contract (a synchronous Go()
			 * from a window thread deadlocks the app_server link);
			 * the answer comes back as kMsgRestartAnswer. */
			BAlert* alert = new BAlert(
				B_TRANSLATE("Restart Vitrine"),
				B_TRANSLATE("The change takes effect the next time "
					"Vitrine starts. Restart it now?\n\nWindows of "
					"running Wayland and X11 applications will "
					"close."),
				B_TRANSLATE("Later"), B_TRANSLATE("Restart now"),
				NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			alert->Go(new BInvoker(new BMessage(kMsgRestartAnswer),
				this));
			break;
		}

		case kMsgRestartAnswer:
		{
			int32 which;
			if (message->FindInt32("which", &which) != B_OK
					|| which != 1)
				break;
			/* BE_QUIT path: Vitrine's ShimApp forwards the external
			 * B_QUIT_REQUESTED into its event loop and exits cleanly.
			 * Relaunch after a grace period — an immediate Launch
			 * would hit B_ALREADY_RUNNING against the dying team. */
			BMessenger(VITRINE_SIGNATURE).SendMessage(B_QUIT_REQUESTED);
			BMessageRunner::StartSending(BMessenger(this),
				new BMessage(kMsgStartVitrine), 2500000, 1);
			break;
		}

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


/*!	Reads one boolean key. Anything unparsable — including a missing
	file — means the caller's default (autostart defaults on: an image
	that ships Vitrine starts it unless the user opted out;
	separate_windows defaults off: the in-process mode is the baseline).
*/
bool
VitrineWindow::_ReadBool(const char* key, bool defaultValue) const
{
	BPath path;
	if (!settings_path(path))
		return defaultValue;

	FILE* file = fopen(path.Path(), "r");
	if (file == NULL)
		return defaultValue;

	size_t keyLength = strlen(key);
	bool enabled = defaultValue;
	char line[256];
	while (fgets(line, sizeof(line), file) != NULL) {
		char* p = line;
		while (isspace(*p))
			p++;
		if (*p == '#' || strncmp(p, key, keyLength) != 0)
			continue;
		p += keyLength;
		while (isspace(*p))
			p++;
		if (*p != '=')
			continue;
		p++;
		while (isspace(*p))
			p++;
		enabled = strncasecmp(p, "true", 4) == 0
			|| strncasecmp(p, "yes", 3) == 0
			|| strncasecmp(p, "on", 2) == 0
			|| *p == '1';
	}
	fclose(file);
	return enabled;
}


/*!	Rewrites the whole settings file from the checkbox states — it is
	small and fully owned by this panel, so a rewrite beats merging.
*/
void
VitrineWindow::_WriteSettings() const
{
	BPath path;
	if (!settings_path(path))
		return;

	FILE* file = fopen(path.Path(), "w");
	if (file == NULL)
		return;
	fprintf(file,
		"# Vitrine — nested Wayland compositor.\n"
		"# Written by the Vitrine preferences panel; autostart is read by\n"
		"# janus and vos-session-boot at login, separate_windows by the\n"
		"# compositor at start. Delete this file to restore defaults.\n"
		"autostart = %s\n"
		"separate_windows = %s\n",
		fAutostartBox->Value() == B_CONTROL_ON ? "true" : "false",
		fSeparateBox->Value() == B_CONTROL_ON ? "true" : "false");
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
