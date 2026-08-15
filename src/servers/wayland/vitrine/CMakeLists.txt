# Author: Vláďa Janeček <vlada@janecek.cloud>
#
# Vitrine — nested Wayland compositor (wlroots 0.18 + bewindow shim).
#
# Deliberately NOT the Server() macro: it stamps
# "-include LinuxBuildCompatibility.h" on every source, which poisons the
# plain-C compositor core compiled against wlroots/wayland headers. Only the
# BeAPI shim (bewindow.cpp) gets the compatibility header and C++ flags; the
# global BeOS include directories (build/headers.cmake) apply either way.

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET wlroots-0.18)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(PIXMAN REQUIRED IMPORTED_TARGET pixman-1)
# Direct libxcb use: xwayland.c re-grants root X access (xcb_change_hosts)
# on every Xwayland ready. Transitive via wlroots is not a link guarantee.
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)

# wlroots' public wlr_xdg_shell.h includes the generated
# xdg-shell-protocol.h; generate it with wayland-scanner from the sysroot's
# protocol XML. The HOST scanner must win (NO_CMAKE_FIND_ROOT_PATH — the
# chroot toolchain would otherwise resolve the chroot's binary, which then
# fails to run against the host's shared libs); the chroot binary is only a
# fallback for hosts without libwayland-bin.
find_program(WAYLAND_SCANNER wayland-scanner NO_CMAKE_FIND_ROOT_PATH)
if(NOT WAYLAND_SCANNER)
	find_program(WAYLAND_SCANNER wayland-scanner
		HINTS "${VITRUVIAN_CHROOT_PATH}/usr/bin")
endif()
if(NOT WAYLAND_SCANNER)
	message(FATAL_ERROR "wayland-scanner not found (install libwayland-bin)")
endif()

set(_xdg_shell_xml
	"${VITRUVIAN_CHROOT_PATH}/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml")
add_custom_command(
	OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h"
	COMMAND "${WAYLAND_SCANNER}" server-header "${_xdg_shell_xml}"
		"${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h"
	DEPENDS "${_xdg_shell_xml}"
	VERBATIM
)

add_executable(vitrine
	main.c
	backend.c
	output.c
	input.c
	shell.c
	rootless.c
	popup.c
	xwayland.c
	clipboard.c
	bewindow.cpp
	"${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h"
)

target_include_directories(vitrine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# BeOS resources: signature, version and the Deskbar icon. xres embeds them
# into the binary itself, which is enough for Tracker/Deskbar because
# BAppFileInfo defaults to B_USE_BOTH_LOCATIONS — no STAGING flag and no
# SYSTEM_SERVERS membership needed (that list is unconditional, while this
# target only exists in --enable-wayland builds).
CompileRdef(vitrine vitrine.rdef)
LinkRdefs(vitrine)

# Installed as "Vitrine": the Deskbar labels a running application with its
# executable's FILE NAME (TBarApp::AddTeam, src/apps/deskbar/BarApp.cpp:931),
# not with the signature or a resource. The localized-name path reads a
# SYS:NAME *attribute*, which does not survive installation (that is what the
# apps_attrs tarball exists for), so the file name is the only reliable
# handle. The CMake target keeps its lowercase name.
set_target_properties(vitrine PROPERTIES C_STANDARD 11 OUTPUT_NAME Vitrine)

# wlroots 0.18 exposes the backend/interface headers only behind this guard.
target_compile_definitions(vitrine PRIVATE WLR_USE_UNSTABLE)

set_source_files_properties(bewindow.cpp PROPERTIES
	COMPILE_FLAGS "-std=gnu++11 -include LinuxBuildCompatibility.h -Wno-multichar -fexceptions"
)

target_link_libraries(vitrine PRIVATE
	PkgConfig::WLROOTS
	PkgConfig::WAYLAND_SERVER
	PkgConfig::XKBCOMMON
	PkgConfig::PIXMAN
	PkgConfig::XCB
	be
	root
)

# Same semantics as ImageInclude("/system/servers" vitrine), inlined so the
# target stays out of SYSTEM_SERVERS: that list also drives the apps_attrs
# staging loop (build/profiles/base.cmake), which needs an rdef we don't
# have yet. Full membership is planned once Vitrine stabilizes.
install(TARGETS vitrine
	COMPONENT /system/servers
	RUNTIME DESTINATION /system/servers
	DESTINATION /system/servers
	PERMISSIONS
		OWNER_READ OWNER_WRITE OWNER_EXECUTE
		GROUP_READ GROUP_EXECUTE
		WORLD_READ WORLD_EXECUTE
)
