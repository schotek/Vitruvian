# Default locale for the session. Nothing else in the image sets LANG, so
# setlocale(LC_ALL, "") resolves to plain "C" (not UTF-8) and every
# locale-aware app either warns (foot) or falls back to ASCII rendering.
# C.UTF-8 is built into glibc (no locale-gen needed) and gives correct
# UTF-8 text handling; a real locale like cs_CZ.UTF-8 would additionally
# need generating in the chroot (etc/locale.gen + locale-gen at image
# build). Guarded so a user-configured locale always wins.
if [ -z "$LANG" ]; then
	export LANG=C.UTF-8
fi
