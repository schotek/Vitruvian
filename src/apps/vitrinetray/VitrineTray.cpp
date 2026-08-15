/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * The application half of the tray applet: installs the replicant into
 * the Deskbar via the entry_ref variant of BDeskbar::AddItem() — the only
 * variant Deskbar persists (the path lands in ~/config/settings/Deskbar/
 * replicants and is re-loaded on every Deskbar start, with dead paths
 * auto-pruned). Idempotent: an already-present item is left alone.
 * Nothing to show otherwise, so the app quits right after (rdef carries
 * B_BACKGROUND_APP — no Deskbar team entry for this helper).
 */

#include "VitrineTray.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>
#include <Deskbar.h>
#include <Entry.h>
#include <Roster.h>

class VitrineTrayApp : public BApplication {
public:
	VitrineTrayApp(status_t* error)
		:
		BApplication(kSignature, error)
	{
	}

	virtual void
	ReadyToRun()
	{
		/* Without the compositor there is nothing to control — refuse to
		 * install so an image without Vitrine gets no dead icon. */
		if (access(kVitrineBinaryPath, X_OK) != 0) {
			fprintf(stderr, "VitrineTray: %s is not installed, "
				"not adding a tray icon\n", kVitrineBinaryPath);
			Quit();
			return;
		}

		BDeskbar deskbar;
		bool isRunning = deskbar.IsRunning();
		if (deskbar.HasItem(kDeskbarItemName)) {
			Quit();
			return;
		}

		/* At session boot this may run before Deskbar is up — wait for it
		 * (the NetworkStatus VOS pattern). */
		int32 tries = 10;
		while (!isRunning && --tries > 0) {
			snooze(1000000);
			isRunning = BDeskbar().IsRunning();
		}
		if (!isRunning) {
			fprintf(stderr, "VitrineTray: Deskbar is not running, "
				"giving up\n");
			Quit();
			return;
		}

		app_info info;
		if (GetAppInfo(&info) == B_OK) {
			status_t status = BDeskbar().AddItem(&info.ref);
			if (status != B_OK) {
				fprintf(stderr, "VitrineTray: installing the tray icon "
					"failed: %s\n", strerror(status));
			}
		}
		Quit();
	}
};


int
main(int /*argc*/, char** /*argv*/)
{
	/* --deskbar and a plain run do the same idempotent install; the flag
	 * exists so boot scripts read naturally (and for symmetry with the
	 * other applets).
	 *
	 * The first_login seed runs this BEFORE janus has spawned the session's
	 * registrar/app_server (janus.cpp: run_first_login_for_user precedes
	 * the post-auth chain on the autologin path). A plain BApplication
	 * constructor would exit(0) silently on the failed connect
	 * (Application.cpp:546-552), eating the tray icon on every fresh boot.
	 * Construct with the error out-param and retry — the same pattern the
	 * compositor's shim uses. ~20 s covers a slow first boot; the seed
	 * script backgrounds us, so nobody waits on this. */
	const int kMaxAttempts = 80;		/* x 250 ms ≈ 20 s */
	status_t error = B_ERROR;
	VitrineTrayApp* app = NULL;
	for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
		error = B_ERROR;
		app = new VitrineTrayApp(&error);
		if (error == B_OK)
			break;
		/* Destructor resets be_app, making the retry legal. An exclusive-
		 * launch refusal means another instance won the race — done. */
		delete app;
		app = NULL;
		if (error == B_ALREADY_RUNNING)
			return 0;
		if (attempt + 1 < kMaxAttempts)
			snooze(250000);
	}
	if (app == NULL) {
		fprintf(stderr, "VitrineTray: registrar/app_server not reachable "
			"(%s), giving up\n", strerror(error));
		return 1;
	}

	app->Run();
	delete app;
	return 0;
}
