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


#endif	/* VITRINE_SETTINGS_H */
