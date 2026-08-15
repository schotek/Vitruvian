/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Deskbar tray replicant controlling the Vitrine compositor: the icon
 * mirrors the running state (dimmed while stopped) and the click menu
 * starts/quits the compositor and opens its preferences. Vitrine is a
 * nested server, not a desktop application — this tray presence next to
 * the clock is its user-facing handle, not an entry among applications.
 *
 * Hosted inside Deskbar's team, so every rule NetworkStatusViewNM learned
 * the hard way applies here too: exactly one popup menu per MouseDown
 * (two menu-tracking threads fight for the mouse grab and the desktop
 * "freezes"), alerts only via the async Go(NULL), and BDeskbar::
 * RemoveItem() never inline from the window thread — it SendMessage()s
 * Deskbar's own team, which this code runs in, so it must run on a
 * detached thread.
 */

#include "VitrineTrayView.h"

#include "VitrineTray.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <Catalog.h>
#include <Deskbar.h>
#include <IconUtils.h>
#include <MenuItem.h>
#include <Messenger.h>
#include <PopUpMenu.h>
#include <Resources.h>
#include <Roster.h>
#include <String.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "VitrineTray"


static const uint32 kMsgStartVitrine = 'strt';
static const uint32 kMsgQuitVitrine = 'quit';
static const uint32 kMsgOpenSettings = 'sett';
static const uint32 kMsgRemoveFromDeskbar = 'remv';

static const float kIconSize = 16.0f;

extern "C" _EXPORT BView* instantiate_deskbar_item(float maxWidth,
	float maxHeight);


VitrineTrayView::VitrineTrayView(BRect frame)
	:
	BView(frame, kDeskbarItemName, B_FOLLOW_LEFT | B_FOLLOW_TOP,
		B_WILL_DRAW)
{
	_Init();
}


VitrineTrayView::VitrineTrayView(BMessage* archive)
	:
	BView(archive)
{
	_Init();
}


VitrineTrayView::~VitrineTrayView()
{
	delete fIcon;
}


void
VitrineTrayView::_Init()
{
	fIcon = NULL;
	fRunning = false;
	fInDeskbar = false;

	app_info info;
	if (be_app != NULL && be_app->GetAppInfo(&info) == B_OK
		&& strcasecmp(info.signature, kDeskbarSignature) == 0)
		fInDeskbar = true;

	_LoadIcon();
}


VitrineTrayView*
VitrineTrayView::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, "VitrineTrayView"))
		return NULL;

	return new VitrineTrayView(archive);
}


status_t
VitrineTrayView::Archive(BMessage* archive, bool deep) const
{
	status_t status = BView::Archive(archive, deep);
	if (status == B_OK)
		status = archive->AddString("add_on", kSignature);
	if (status == B_OK)
		status = archive->AddString("class", "VitrineTrayView");

	return status;
}


void
VitrineTrayView::_LoadIcon()
{
	/* Resources come from this add-on's own image, found by the address of
	 * the exported entry point (the NetworkStatus/PowerStatus idiom —
	 * inside the tray be_app is Deskbar, so app-based lookups would fetch
	 * Deskbar's resources). */
	BResources resources;
	if (resources.SetToImage((void*)&instantiate_deskbar_item) != B_OK)
		return;

	/* By NAME, not id: the rdef `resource vector_icon` shorthand assigns
	 * an id of its own choosing (1001 here) — the stable handle is the
	 * "BEOS:ICON" name, the same one resattr keys the xattr mapping on. */
	size_t size;
	const void* data = resources.LoadResource(B_VECTOR_ICON_TYPE,
		"BEOS:ICON", &size);
	if (data == NULL)
		return;

	BBitmap* icon = new BBitmap(BRect(0, 0, kIconSize - 1, kIconSize - 1),
		B_RGBA32);
	if (icon->InitCheck() != B_OK
		|| BIconUtils::GetVectorIcon((const uint8*)data, size, icon) != B_OK) {
		delete icon;
		return;
	}
	fIcon = icon;
}


void
VitrineTrayView::AttachedToWindow()
{
	BView::AttachedToWindow();
	if (Parent() != NULL)
		SetViewColor(Parent()->ViewColor());
	else
		SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
	SetLowColor(ViewColor());

	/* Live state: the registrar broadcasts app launch/quit; filter on the
	 * compositor's signature in MessageReceived. A one-shot IsRunning()
	 * seeds the state (and MouseDown re-seeds before building the menu, so
	 * a missed notification can never wedge the menu contents). */
	be_roster->StartWatching(BMessenger(this),
		B_REQUEST_LAUNCHED | B_REQUEST_QUIT);
	_UpdateRunning(false);
}


void
VitrineTrayView::DetachedFromWindow()
{
	be_roster->StopWatching(BMessenger(this));
	BView::DetachedFromWindow();
}


void
VitrineTrayView::_UpdateRunning(bool invalidate)
{
	bool running = be_roster->IsRunning(kVitrineSignature);
	if (running == fRunning)
		return;
	fRunning = running;
	if (invalidate)
		Invalidate();
}


void
VitrineTrayView::Draw(BRect updateRect)
{
	if (fIcon == NULL)
		return;

	SetDrawingMode(B_OP_ALPHA);
	if (fRunning) {
		SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	} else {
		/* Stopped: the icon at reduced opacity — the constant-alpha blend
		 * scales the whole bitmap by the high color's alpha. */
		SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
		SetHighColor(0, 0, 0, 110);
	}
	BRect bounds = Bounds();
	BPoint at((bounds.Width() - kIconSize + 1) / 2,
		(bounds.Height() - kIconSize + 1) / 2);
	DrawBitmap(fIcon, at);
	SetDrawingMode(B_OP_COPY);
}


void
VitrineTrayView::MouseDown(BPoint where)
{
	/* One menu per click, whatever the button (see file header). */
	_UpdateRunning(true);
	_ShowMenu(where);
}


void
VitrineTrayView::_ShowMenu(BPoint where)
{
	BPopUpMenu* menu = new BPopUpMenu(B_EMPTY_STRING, false, false);
	menu->SetAsyncAutoDestruct(true);
	menu->SetFont(be_plain_font);

	if (access(kVitrineBinaryPath, X_OK) != 0) {
		BMenuItem* missing = new BMenuItem(
			B_TRANSLATE("Vitrine is not installed"), NULL);
		missing->SetEnabled(false);
		menu->AddItem(missing);
	} else if (fRunning) {
		menu->AddItem(new BMenuItem(B_TRANSLATE("Quit Vitrine"),
			new BMessage(kMsgQuitVitrine)));
	} else {
		menu->AddItem(new BMenuItem(B_TRANSLATE("Start Vitrine"),
			new BMessage(kMsgStartVitrine)));
	}

	menu->AddItem(new BMenuItem(
		B_TRANSLATE("Vitrine settings" B_UTF8_ELLIPSIS),
		new BMessage(kMsgOpenSettings)));

	if (fInDeskbar) {
		menu->AddSeparatorItem();
		menu->AddItem(new BMenuItem(B_TRANSLATE("Remove from Deskbar"),
			new BMessage(kMsgRemoveFromDeskbar)));
	}

	menu->SetTargetForItems(this);
	ConvertToScreen(&where);
	menu->Go(where, true, true, true);
}


void
VitrineTrayView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_SOME_APP_LAUNCHED:
		case B_SOME_APP_QUIT:
		{
			const char* signature;
			if (message->FindString("be:signature", &signature) == B_OK
				&& strcasecmp(signature, kVitrineSignature) == 0)
				_UpdateRunning(true);
			break;
		}

		case kMsgStartVitrine:
		{
			status_t status = be_roster->Launch(kVitrineSignature);
			if (status != B_OK && status != B_ALREADY_RUNNING) {
				BString text(B_TRANSLATE("Could not start Vitrine: %error%"));
				text.ReplaceFirst("%error%", strerror(status));
				/* Async only — a synchronous Go() would block Deskbar's
				 * window thread. */
				BAlert* alert = new BAlert(B_TRANSLATE("Vitrine"), text,
					B_TRANSLATE("OK"));
				alert->Go(NULL);
			}
			break;
		}

		case kMsgQuitVitrine:
		{
			/* The compositor's shim forwards this to the wl event loop
			 * (BE_INPUT_QUIT) and tears the whole process down cleanly. */
			BMessenger messenger(kVitrineSignature);
			if (messenger.IsValid())
				messenger.SendMessage(B_QUIT_REQUESTED);
			break;
		}

		case kMsgOpenSettings:
			be_roster->Launch(kVitrineSettingsSignature);
			break;

		case kMsgRemoveFromDeskbar:
		{
			/* Never inline: RemoveItem() SendMessage()s the very team this
			 * code runs in — a detached thread avoids the self-deadlock
			 * (see file header). */
			thread_id thread = spawn_thread(_RemoveFromDeskbarThread,
				"remove tray icon", B_NORMAL_PRIORITY, NULL);
			if (thread >= B_OK)
				resume_thread(thread);
			break;
		}

		default:
			BView::MessageReceived(message);
	}
}


int32
VitrineTrayView::_RemoveFromDeskbarThread(void* /*data*/)
{
	BDeskbar deskbar;
	deskbar.RemoveItem(kDeskbarItemName);
	return 0;
}


/* ---- Deskbar add-on entry points ---- */

extern "C" _EXPORT BView*
instantiate_deskbar_item(float maxWidth, float maxHeight)
{
	/* Square and height-driven: maxWidth is the whole tray width, and a
	 * wider-than-a-row replicant makes TReplicantTray::
	 * LocationForReplicant() loop forever. */
	return new VitrineTrayView(BRect(0, 0, maxHeight - 1, maxHeight - 1));
}


extern "C" _EXPORT status_t
instantiate_deskbar_item_for_width(float* width, float* height)
{
	*width = kIconSize;
	*height = kIconSize;
	return B_OK;
}
