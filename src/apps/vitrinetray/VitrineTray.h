/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 */
#ifndef VITRINE_TRAY_H
#define VITRINE_TRAY_H


/* The tray applet itself. */
static const char* kSignature = "application/x-vnd.vos-VitrineTray";

/* The view's Name() — BDeskbar item lookup/removal works by this name. */
static const char* kDeskbarItemName = "VitrineTray";

/* Deskbar's signature: a view unarchived inside the tray runs in this team. */
static const char* kDeskbarSignature = "application/x-vnd.Be-TSKB";

/* Kept in sync with src/preferences/vitrine/VitrineSettings.h and
 * src/servers/wayland/vitrine/vitrine.rdef — the compositor this applet
 * launches, quits and watches. */
static const char* kVitrineSignature = "application/x-vnd.vos-Vitrine";
static const char* kVitrineBinaryPath = "/system/servers/Vitrine";
static const char* kVitrineSettingsSignature =
	"application/x-vnd.Vitruvian-VitrinePrefs";


#endif	/* VITRINE_TRAY_H */
