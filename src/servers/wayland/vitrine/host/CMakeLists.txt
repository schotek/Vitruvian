# Author: Vláďa Janeček <vlada@janecek.cloud>
#
# vitrine_window_host — per-window helper (phase H1). Pure libbe: shares
# the window classes with the compositor by compiling bewindow_win.cpp a
# second time, but links no wlroots/wayland at all.

add_executable(vitrine_window_host
	window_host.cpp
	"${CMAKE_CURRENT_SOURCE_DIR}/../bewindow_win.cpp"
)

target_include_directories(vitrine_window_host PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/.."
)
target_link_libraries(vitrine_window_host PRIVATE be root)

set_source_files_properties(
	window_host.cpp
	"${CMAKE_CURRENT_SOURCE_DIR}/../bewindow_win.cpp"
	PROPERTIES COMPILE_FLAGS
	"-std=gnu++11 -include LinuxBuildCompatibility.h -Wno-multichar -fexceptions"
)

install(TARGETS vitrine_window_host
	COMPONENT /system/servers
	RUNTIME DESTINATION /system/servers
	DESTINATION /system/servers
	PERMISSIONS
		OWNER_READ OWNER_WRITE OWNER_EXECUTE
		GROUP_READ GROUP_EXECUTE
		WORLD_READ WORLD_EXECUTE
)
