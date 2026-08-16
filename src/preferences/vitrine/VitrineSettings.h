/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Shared description of the Vitrine settings file. The file is per-user
 * plain "key = value" text — the same shape the input server already uses
 * for ~/config/settings/input/xkb_layout — so janus (C), vos-session-boot
 * (shell) and this preferences panel (C++) can all read it without a
 * parser library. A missing file means "defaults", i.e. autostart on.
 */
#ifndef VITRINE_SETTINGS_H
#define VITRINE_SETTINGS_H


/* Relative to B_USER_SETTINGS_DIRECTORY. */
#define VITRINE_SETTINGS_FILE	"vitrine"

/* The compositor binary; the panel greys itself out when it is absent
 * (images built without --enable-wayland do not ship it). */
#define VITRINE_BINARY_PATH	"/system/servers/Vitrine"

#define VITRINE_SIGNATURE	"application/x-vnd.vos-Vitrine"

/* The per-window helper binary; "Show windows separately" needs it. */
#define VITRINE_WINHOST_PATH	"/system/servers/vitrine_window_host"

/* The Deskbar tray applet (src/apps/vitrinetray) this panel can toggle.
 * The item name is the tray view's Name() — BDeskbar looks items up by
 * it, not by signature. */
#define VITRINE_TRAY_PATH	"/system/apps/VitrineTray"
#define VITRINE_TRAY_SIGNATURE	"application/x-vnd.vos-VitrineTray"
#define VITRINE_TRAY_ITEM_NAME	"VitrineTray"


#endif	/* VITRINE_SETTINGS_H */
