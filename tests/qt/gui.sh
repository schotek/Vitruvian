#!/bin/bash
#
# Author: Vláďa Janeček <vlada@janecek.cloud>
#
# Run a GUI program inside the running desktop session.
#
# Installed into the VM as /tmp/gui.sh and used as:
#     runuser -u vos-live -- /tmp/gui.sh <program> [args]
#
# A process started over ssh inherits root's environment, which has none of
# what a BeOS application needs to reach app_server. Rather than guessing the
# variables, take them from app_server itself — whatever janus gave the
# session is by definition the right set.
pid=$(pgrep -u vos-live -x app_server | head -1)
[ -n "$pid" ] || { echo "app_server not running" >&2; exit 1; }
while IFS= read -r -d "" v; do export "$v"; done < /proc/$pid/environ
exec "$@"
