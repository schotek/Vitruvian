/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Preferences panel for window edge snapping: dragging a window to the
 * top screen edge maximizes it, to the left/right edge tiles it to that
 * half. The feature itself lives in app_server (EdgeSnap desktop
 * listener); this panel only flips its two settings.
 */

#include <AboutWindow.h>
#include <Application.h>
#include <Catalog.h>

#include "EdgeSnapWindow.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "EdgeSnap application"


static const char* kSignature = "application/x-vnd.Vitruvian-EdgeSnap";


class EdgeSnapApp : public BApplication {
public:
	EdgeSnapApp()
		:
		BApplication(kSignature)
	{
	}

	virtual void ReadyToRun()
	{
		new EdgeSnapWindow();
	}

	virtual void AboutRequested()
	{
		BAboutWindow* about = new BAboutWindow(
			B_TRANSLATE_SYSTEM_NAME("EdgeSnap"), kSignature);

		const char* authors[] = {
			"Vláďa Janeček <vlada@janecek.cloud>",
			NULL
		};

		about->AddDescription(B_TRANSLATE("Snap windows to screen halves "
			"or maximize them by dragging them to a screen edge."));
		about->AddAuthors(authors);
		about->Show();
	}
};


int
main()
{
	EdgeSnapApp app;
	app.Run();
	return 0;
}
