# Author: Vláďa Janeček <vlada@janecek.cloud>
#
# Deskbar tray applet for the Vitrine compositor. RunnableAddOn: the view
# gets dlopen'd INTO Deskbar (instantiate_deskbar_item), while the same
# binary is execve'd to install itself — the dual-nature .so with PT_INTERP
# every working tray applet uses (NetworkStatus, PowerStatus, ...).
# Only exists on --enable-wayland builds; the matching apps_attrs entry in
# build/profiles/base.cmake is guarded the same way.

RunnableAddOn(VitrineTray
	SOURCES
	VitrineTray.cpp
	VitrineTrayView.cpp
	LIBS be localestub
	RDEF VitrineTray.rdef
)

DoCatalogs("x-vnd.vos-VitrineTray" apps/vitrinetray)

install(TARGETS VitrineTray
	COMPONENT /system/apps
	RUNTIME DESTINATION /system/apps
	LIBRARY DESTINATION /system/apps
	DESTINATION /system/apps
	PERMISSIONS
		OWNER_READ OWNER_WRITE OWNER_EXECUTE
		GROUP_READ GROUP_EXECUTE
		WORLD_READ WORLD_EXECUTE
)
