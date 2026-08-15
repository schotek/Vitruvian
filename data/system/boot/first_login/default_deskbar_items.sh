#!/bin/sh

# install ProcessController, NetworkStatus & volume control in the Deskbar
#/system/apps/ProcessController -deskbar
#/system/apps/NetworkStatus --deskbar
#/bin/desklink --add-volume

# Vitrine tray control (next to the clock). The applet itself refuses to
# install when /system/servers/Vitrine is absent, waits for the Deskbar,
# and is idempotent — Deskbar then keeps the icon across restarts via its
# replicants settings file.
[ -x /system/apps/VitrineTray ] && /system/apps/VitrineTray --deskbar &
