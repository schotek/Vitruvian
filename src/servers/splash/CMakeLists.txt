find_package(PkgConfig REQUIRED)
pkg_check_modules(SPLASH_DRM REQUIRED libdrm)

# Embed the logo at build time — the splash runs before any filesystem
# with artwork is guaranteed to be readable.
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/logo_rgba.h
	COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/embed_png.py
		${CMAKE_SOURCE_DIR}/data/artwork/vitruvian-logo.png
		${CMAKE_CURRENT_BINARY_DIR}/logo_rgba.h Logo
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/embed_png.py
		${CMAKE_SOURCE_DIR}/data/artwork/vitruvian-logo.png
	COMMENT "Embedding boot splash logo")

# Deliberately not Server(): must not link libbe — it runs long before
# registrar/app_server exist (same rule as janus_launch).
add_executable(vitruvian-splash splash.c
	${CMAKE_CURRENT_BINARY_DIR}/logo_rgba.h)
target_include_directories(vitruvian-splash PRIVATE
	${CMAKE_CURRENT_BINARY_DIR} ${SPLASH_DRM_INCLUDE_DIRS})
target_link_libraries(vitruvian-splash PRIVATE ${SPLASH_DRM_LIBRARIES})
