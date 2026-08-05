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
	export GTK_THEME="${GTK_THEME:-BeOS}"
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
