cmake_minimum_required(VERSION 3.20)
project(openbg LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall> $<$<COMPILE_LANGUAGE:CXX>:-Wextra> $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>)

# Windows 10 and 11 build with MinGW-w64 (GCC or Clang; MSYS2's UCRT64 environment, or a cross
# compiler). The C++ runtime is linked in, so the programs need no compiler DLLs, and an embedded
# manifest makes UTF-8 the process code page, so narrow-string file APIs take any path.
if(WIN32)
	add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
	if(MINGW)
		add_link_options(-static -static-libgcc -static-libstdc++)
	endif()
	enable_language(RC)
	set(OPENBG_WINDOWS_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/app/windows/openbg.rc)
endif()

# GothicKit dmusic (third_party/dmusic, MIT): DirectMusic style segments and DLS synthesis for the
# songs. Built here as a static library; macOS has no C11 <threads.h>, so it uses its POSIX shim
# (Windows its Win32 one).
file(GLOB DMUSIC_SOURCES third_party/dmusic/src/*.c third_party/dmusic/src/io/*.c)
if(WIN32)
	set(DMUSIC_THREAD third_party/dmusic/src/thread/Thread.Win32.c)
else()
	set(DMUSIC_THREAD third_party/dmusic/src/thread/Thread.Posix.c)
endif()
add_library(dmusic STATIC ${DMUSIC_SOURCES} ${DMUSIC_THREAD})
set_target_properties(dmusic PROPERTIES C_STANDARD 11)
target_include_directories(dmusic PUBLIC third_party/dmusic/include PRIVATE third_party/dmusic/src)
target_compile_definitions(dmusic PUBLIC DM_STATIC=1 PRIVATE DM_BUILD=1)
target_compile_options(dmusic PRIVATE -w)
if(MINGW)
	# MinGW declares gmtime_r (used by dmusic's logger) only for POSIX thread-safe functions.
	target_compile_definitions(dmusic PRIVATE _POSIX_THREAD_SAFE_FUNCTIONS=200112L)
endif()
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
	set(OPENINDEO_GIT_TAG "main" CACHE STRING "openindeo revision")
	FetchContent_Declare(openindeo GIT_REPOSITORY https://github.com/kadima-tech/openindeo.git GIT_TAG ${OPENINDEO_GIT_TAG})
	FetchContent_MakeAvailable(openindeo)
endif()

# Core data layer: DE_CContents stream, block decompression, .DEObjects container.
add_library(de STATIC
	src/de/contents.cpp
	src/de/lzrw3a.cpp
	src/de/md5.cpp
	src/de/objects_file.cpp
	src/de/objects_model.cpp
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
	src/de/platform.cpp
)
target_include_directories(de PUBLIC src)
target_link_libraries(de PRIVATE openindeo)
if(WIN32)
	target_link_libraries(de PRIVATE shell32 ole32 uuid)
endif()

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

# The byte link between the machines of a network session (src/net/link.h).
add_library(net STATIC src/net/link.cpp)
target_include_directories(net PUBLIC src)

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
	src/game/netplay.cpp
)
target_link_libraries(game PUBLIC vm de net)

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
add_executable(vmc2-tests tests/vmc2_tests.cpp)
target_link_libraries(vmc2-tests PRIVATE vm)
add_test(NAME vmc2-writer COMMAND vmc2-tests "${OPENBG_DATA_DIR}")
# tools/deobjects-editor's JSON form, written and read back by the engine's reader.
set(DEOBJECTS_EDITOR ${CMAKE_CURRENT_SOURCE_DIR}/../tools/deobjects-editor/src)
add_library(deobjects-convert STATIC ${DEOBJECTS_EDITOR}/json.cpp ${DEOBJECTS_EDITOR}/schema.cpp
	${DEOBJECTS_EDITOR}/convert.cpp)
target_include_directories(deobjects-convert PUBLIC ${DEOBJECTS_EDITOR})
target_link_libraries(deobjects-convert PUBLIC de)
add_executable(deobjects-data-tests tests/deobjects_tests.cpp)
target_link_libraries(deobjects-data-tests PRIVATE game deobjects-convert)
add_test(NAME deobjects-edit-reload COMMAND deobjects-data-tests "${OPENBG_DATA_DIR}")
# Unpacks and packs every file of the Data folder (~10 s); each must come back byte for byte.
add_test(NAME deobjects-round-trip COMMAND deobjects-data-tests "${OPENBG_DATA_DIR}" --all)
set_tests_properties(deobjects-round-trip PROPERTIES LABELS full)

# Native viewer (SDL2 + OpenGL 4.1 core).
if(NOT TARGET SDL2::SDL2)
	find_package(SDL2 REQUIRED)
endif()
find_package(OpenGL REQUIRED)
# SDL's main() wrapper turns the Windows command line into UTF-8 arguments.
set(OPENBG_SDL_LIBS SDL2::SDL2)
if(WIN32 AND TARGET SDL2::SDL2main)
	set(OPENBG_SDL_LIBS SDL2::SDL2main SDL2::SDL2)
endif()
add_executable(openbg-viewer app/viewer.cpp)
target_link_libraries(openbg-viewer PRIVATE de ${OPENBG_SDL_LIBS} OpenGL::GL ZLIB::ZLIB)
add_executable(openbg-arena app/arena.cpp app/render.cpp app/glow.cpp)
target_link_libraries(openbg-arena PRIVATE de ${OPENBG_SDL_LIBS} OpenGL::GL ZLIB::ZLIB)
add_executable(pcc tools/pcc.cpp)
target_link_libraries(pcc PRIVATE vm)
add_executable(openbg-brain tools/brain.cpp)
target_link_libraries(openbg-brain PRIVATE vm)
add_executable(openbg-fight app/fight.cpp app/game_view.cpp app/scene_lights.cpp
	app/shadows.cpp app/render.cpp app/overlay.cpp app/particles_gl.cpp app/audio.cpp app/music_player.cpp app/glow.cpp)
target_link_libraries(openbg-fight PRIVATE game dmusic ${OPENBG_SDL_LIBS} OpenGL::GL ZLIB::ZLIB)
add_executable(openbg-net app/net_harness.cpp app/game_view.cpp app/scene_lights.cpp
	app/shadows.cpp app/render.cpp app/overlay.cpp app/particles_gl.cpp app/audio.cpp app/music_player.cpp app/glow.cpp)
target_link_libraries(openbg-net PRIVATE game dmusic ${OPENBG_SDL_LIBS} OpenGL::GL ZLIB::ZLIB)
add_executable(openbg app/frontend.cpp app/game_view.cpp app/scene_lights.cpp
	app/shadows.cpp app/render.cpp app/overlay.cpp app/particles_gl.cpp app/audio.cpp app/music_player.cpp app/glow.cpp)
target_link_libraries(openbg PRIVATE game dmusic ${OPENBG_SDL_LIBS} OpenGL::GL ZLIB::ZLIB)
if(WIN32)
	# Every program gets the manifest: UTF-8 arguments and paths, DPI awareness, Windows 10/11.
	foreach(program deobj-dump de-tests vmc2-tests deobjects-data-tests openbg-viewer openbg-arena pcc openbg-brain openbg-fight openbg-net openbg)
		target_sources(${program} PRIVATE ${OPENBG_WINDOWS_RESOURCES})
	endforeach()
endif()
