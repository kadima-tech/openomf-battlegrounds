cmake_minimum_required(VERSION 3.20)
project(openbg LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall> $<$<COMPILE_LANGUAGE:CXX>:-Wextra> $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>)

# GothicKit dmusic (third_party/dmusic, MIT): DirectMusic style segments and DLS synthesis for the
# songs. Built here as a static library; macOS has no C11 <threads.h>, so it uses its POSIX shim.
file(GLOB DMUSIC_SOURCES third_party/dmusic/src/*.c third_party/dmusic/src/io/*.c)
add_library(dmusic STATIC ${DMUSIC_SOURCES}
	third_party/dmusic/src/thread/Thread.Posix.c)
set_target_properties(dmusic PROPERTIES C_STANDARD 11)
target_include_directories(dmusic PUBLIC third_party/dmusic/include PRIVATE third_party/dmusic/src)
target_compile_definitions(dmusic PUBLIC DM_STATIC=1 PRIVATE DM_BUILD=1)
target_compile_options(dmusic PRIVATE -w)
# dmusic tables that are DirectX's own data (dmsynth.dll's filter coefficients, dmime.dll's curve
# shapes) are read from the game disc at build time; without the disc they fall back.
set(OPENBG_GAME_ISO "${CMAKE_CURRENT_SOURCE_DIR}/../source/Battlegrounds.iso" CACHE FILEPATH "the game disc image")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(DIRECTX_TABLES ${CMAKE_CURRENT_BINARY_DIR}/generated/_MsFilterTables.h ${CMAKE_CURRENT_BINARY_DIR}/generated/_MsCurveTables.h)
add_custom_command(OUTPUT ${DIRECTX_TABLES}
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../re/scripts/directx_tables.py ${CMAKE_CURRENT_BINARY_DIR}/generated --iso ${OPENBG_GAME_ISO}
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../re/scripts/directx_tables.py
	COMMENT "Reading DirectX's tables from the game disc")
target_sources(dmusic PRIVATE ${DIRECTX_TABLES})
target_include_directories(dmusic PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# openindeo (github.com/kadima-tech/openindeo, MIT): Indeo 4/5 video, the Image\AVI textures
# Cinepak does not cover. A checkout next to this repository is used when there is one
# (OPENINDEO_DIR); otherwise it is fetched.
set(OPENINDEO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../openindeo" CACHE PATH "openindeo source")
set(OPENINDEO_TOOLS OFF CACHE BOOL "" FORCE)
if(EXISTS "${OPENINDEO_DIR}/CMakeLists.txt")
	add_subdirectory(${OPENINDEO_DIR} openindeo)
else()
	include(FetchContent)
	FetchContent_Declare(openindeo GIT_REPOSITORY https://github.com/kadima-tech/openindeo.git GIT_TAG main)
	FetchContent_MakeAvailable(openindeo)
endif()

# Core data layer: DE_CContents stream, block decompression, .DEObjects container.
add_library(de STATIC
	src/de/contents.cpp
	src/de/lzrw3a.cpp
	src/de/md5.cpp
	src/de/objects_file.cpp
	src/de/pcode.cpp
	src/de/image.cpp
	src/de/avi.cpp
	src/de/mesh.cpp
	src/de/font.cpp
	src/de/sound.cpp
	src/de/value.cpp
	src/de/particles.cpp
	src/de/texture.cpp
	src/de/scene.cpp
	src/de/anim.cpp
	src/de/lighting.cpp
)
target_include_directories(de PUBLIC src)
target_link_libraries(de PRIVATE openindeo)

# Script VM: compiled VMCode (VMC2) reader, bytecode decoder.
add_library(vm STATIC
	src/vm/vmcode.cpp
	src/vm/disasm.cpp
	src/vm/memory.cpp
	src/vm/script.cpp
	src/vm/executor.cpp
	src/vm/pcode_parser.cpp
	src/vm/compiler.cpp
)
target_link_libraries(vm PUBLIC de)

# Game runtime: native objects and intrinsics for compiled scripts.
add_library(game STATIC
	src/game/runtime.cpp
	src/game/natives_core.cpp
	src/game/natives_ai.cpp
	src/game/natives_combat.cpp
	src/game/natives_script.cpp
	src/game/natives_transform.cpp
	src/game/particles.cpp
	src/game/forces.cpp
	src/game/grapple.cpp
	src/game/creatures.cpp
	src/game/needs.cpp
	src/game/music.cpp
	src/game/simple_physics.cpp
	src/game/observer.cpp
	src/game/impulses.cpp
	src/game/navigation.cpp
	src/game/session.cpp
	src/game/replay.cpp
	src/game/ui.cpp
	src/game/controls.cpp
	src/game/descriptions.cpp
)
target_link_libraries(game PUBLIC vm de)

add_executable(deobj-dump tools/deobj_dump.cpp)
find_package(ZLIB REQUIRED)
target_link_libraries(deobj-dump PRIVATE vm ZLIB::ZLIB)

enable_testing()
add_executable(de-tests tests/de_tests.cpp)
target_link_libraries(de-tests PRIVATE vm)
# The port follows the last official patch, 2.123 (PATCHES.md); the disc's 2.090 data still loads.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../source/patched/v2123/Data")
	set(OPENBG_DEFAULT_DATA "${CMAKE_CURRENT_SOURCE_DIR}/../source/patched/v2123/Data")
else()
	set(OPENBG_DEFAULT_DATA "${CMAKE_CURRENT_SOURCE_DIR}/../source/extracted/App_Executables/Data")
endif()
set(OPENBG_DATA_DIR "${OPENBG_DEFAULT_DATA}" CACHE PATH "Original game Data directory")
add_test(NAME de-tests COMMAND de-tests "${OPENBG_DATA_DIR}")
# Walks all 728 MB of shipped data (~20 s); exclude with: ctest -LE full
add_test(NAME de-all-files COMMAND de-tests "${OPENBG_DATA_DIR}" --all)
set_tests_properties(de-all-files PROPERTIES LABELS full)

# Native viewer (SDL2 + OpenGL 4.1 core).
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
add_executable(openbg-viewer app/viewer.cpp)
target_link_libraries(openbg-viewer PRIVATE de SDL2::SDL2 OpenGL::GL ZLIB::ZLIB)
add_executable(openbg-arena app/arena.cpp app/render.cpp app/glow.cpp)
target_link_libraries(openbg-arena PRIVATE de SDL2::SDL2 OpenGL::GL ZLIB::ZLIB)
add_executable(pcc tools/pcc.cpp)
target_link_libraries(pcc PRIVATE vm)
add_executable(openbg-brain tools/brain.cpp)
target_link_libraries(openbg-brain PRIVATE vm)
add_executable(openbg-fight app/fight.cpp app/game_view.cpp app/scene_lights.cpp
	app/shadows.cpp app/render.cpp app/overlay.cpp app/particles_gl.cpp app/audio.cpp app/music_player.cpp app/glow.cpp)
target_link_libraries(openbg-fight PRIVATE game dmusic SDL2::SDL2 OpenGL::GL ZLIB::ZLIB)
add_executable(openbg app/frontend.cpp app/game_view.cpp app/scene_lights.cpp
	app/shadows.cpp app/render.cpp app/overlay.cpp app/particles_gl.cpp app/audio.cpp app/music_player.cpp app/glow.cpp)
target_link_libraries(openbg PRIVATE game dmusic SDL2::SDL2 OpenGL::GL ZLIB::ZLIB)
