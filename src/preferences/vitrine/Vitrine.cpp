/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Preferences panel for the Vitrine compositor. Deliberately a separate
 * application from the compositor itself: it must be usable when Vitrine
 * is not running (that is when you want to switch it back on), and it
 * carries no Wayland dependency, so it ships even in images built without
 * --enable-wayland — where it reports that Vitrine is not installed.
 */

#include <AboutWindow.h>
#include <Application.h>
#include <Catalog.h>

#include "VitrineWindow.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Vitrine application"


static const char* kSignature = "application/x-vnd.Vitruvian-VitrinePrefs";


class VitrineApp : public BApplication {
public:
	VitrineApp()
		:
		BApplication(kSignature)
	{
	}

	virtual void ReadyToRun()
	{
		new VitrineWindow();
	}

	/*!	Reached from the window's "About" button (which posts
		B_ABOUT_REQUESTED) as well as from the Deskbar's own About item, so
		both entry points show the same standard panel.
	*/
	virtual void AboutRequested()
	{
		BAboutWindow* about = new BAboutWindow(
			B_TRANSLATE_SYSTEM_NAME("VitrineSettings"), kSignature);

		const char* authors[] = {
			"Vláďa Janeček <vlada@janecek.cloud>",
			NULL
		};

		about->AddDescription(B_TRANSLATE("Settings for Vitrine, the nested "
			"Wayland compositor that hosts Wayland and X11 applications as "
			"native windows on the VitruvianOS desktop."));
		/* Authorship only — no copyright or licence line, by decision. */
		about->AddAuthors(authors);
		about->Show();
	}
};


int
main()
{
	VitrineApp app;
	app.Run();
	return 0;
}
