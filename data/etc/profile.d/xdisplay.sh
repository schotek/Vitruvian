# Author: Vláďa Janeček <vlada@janecek.cloud>
#
# Wayland/X11 session environment for the Vitrine nested compositor.
# Static names are safe: vitrine binds WAYLAND_DISPLAY=wayland-0 itself and
# sweeps a stale X :0 before its lazy Xwayland starts, so the values hold
# across compositor crash-restarts. Shell sessions get them here; Tracker-
# launched applications get the same set from janus (which never sources
# profile.d). Every export yields to a value already present.
if [ -x /system/servers/Vitrine ]; then
	export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
	export DISPLAY="${DISPLAY:-:0}"
	export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland}"
	export GDK_BACKEND="${GDK_BACKEND:-wayland,x11}"
	# The user's theme pick from the Vitrine preferences panel; missing
	# key/file = the vendored BeOS look (same reader janus applies for
	# Tracker-launched applications).
	if [ -z "$GTK_THEME" ]; then
		vitrine_theme="$(sed -n \
			's/^[[:space:]]*gtk_theme[[:space:]]*=[[:space:]]*//p' \
			"${HOME:-/root}/config/settings/vitrine" 2>/dev/null \
			| tail -1 | sed 's/[[:space:]]*$//')"
		GTK_THEME="${vitrine_theme:-BeOS}"
		unset vitrine_theme
	fi
	export GTK_THEME
	export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"

	# pam_systemd/logind is the primary provider; the /run/vos/user tree
	# is janus's fallback for PAM stacks without pam_systemd.
	if [ -z "$XDG_RUNTIME_DIR" ]; then
		for _rundir in "/run/user/$(id -u)" "/run/vos/user/$(id -u)"; do
			if [ -d "$_rundir" ]; then
				export XDG_RUNTIME_DIR="$_rundir"
				break
			fi
		done
		unset _rundir
	fi
fi
