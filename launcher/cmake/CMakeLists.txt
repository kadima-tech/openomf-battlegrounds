cmake_minimum_required(VERSION 3.20)
project(openbg_launcher_runtime LANGUAGES C CXX)
include(FetchContent)
if(WIN32)
    enable_language(RC)
endif()

# Both libraries are linked statically. End users need no Homebrew packages (macOS), distro
# packages (Linux) or DLLs beyond those every Windows 10/11 installation has.

# macOS takes zlib from its SDK. Windows has none, so a pinned zlib is built and linked statically;
# ZLIB_* are preset so find_package(ZLIB) in the engine and unshield resolve to it.
if(WIN32)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)
    FetchContent_MakeAvailable(zlib)
    target_include_directories(zlibstatic INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
    set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE STRING "" FORCE)
    set(ZLIB_LIBRARY zlibstatic CACHE STRING "" FORCE)
endif()

# Linux players need no particular C++ runtime: link it statically. SDL2 still
# loads X11/Wayland, audio servers and libGL from the desktop at runtime.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	add_link_options(-static-libstdc++ -static-libgcc)
	# GCC 16 reports libgcc_s as an implicit C library, so CMake would name it on
	# C++ links that contain C objects (SDL2, openindeo) and undo -static-libgcc.
	list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES gcc_s gcc_s_asneeded)
endif()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(sdl2
    URL https://codeload.github.com/libsdl-org/SDL/tar.gz/5d249570393f7a37e037abf22cd6012a4cc56a71
    URL_HASH SHA256=10f1194f8d2e4a73ca1c7c553b3189d0b68ab9ef3544e8d2a268e4429456b373)
FetchContent_MakeAvailable(sdl2)

set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(USE_OUR_OWN_MD5 ON CACHE BOOL "" FORCE)
FetchContent_Declare(unshield
    URL https://codeload.github.com/twogood/unshield/tar.gz/refs/tags/1.5.1
    URL_HASH SHA256=34cd97ff1e6f764436d71676e3d6842dc7bd8e2dd5014068da5c560fe4661f60)
# unshield's bundled MD5 code uses K&R declarations, which C23 (GCC 15+'s default, as in MSYS2)
# rejects. Build it as C17, what Apple clang defaults to (Fedora 42+ also has GCC 15+).
set(CMAKE_C_STANDARD 17)
if(WIN32)
    # iconv only serves unshield's -e (filename encoding) option, which setup does not use.
    # MSYS2 has a libiconv that would make unshield.exe need libiconv-2.dll; skip the probe.
    set(HAVE_ICONV "" CACHE INTERNAL "unshield: iconv disabled on Windows")
endif()
FetchContent_MakeAvailable(unshield)
unset(CMAKE_C_STANDARD)

set(OPENINDEO_GIT_TAG "5452df0c8900f0892e701253481bf5c311a9a9d7" CACHE STRING "" FORCE)
set(OPENINDEO_DIR "${CMAKE_CURRENT_BINARY_DIR}/no-local-openindeo" CACHE PATH "" FORCE)
# Never bake disc-derived DirectX data into a redistributable app.
set(OPENBG_GAME_ISO "${CMAKE_CURRENT_BINARY_DIR}/no-bundled-game-data.iso" CACHE FILEPATH "" FORCE)
add_subdirectory(../../engine engine)

# The Windows 10/11 launcher (Battlegrounds.exe) and its native tests. macOS builds the SwiftUI
# launcher in build-app.sh instead.
if(WIN32)
    set(OPENBG_VERSION "0.1.0" CACHE STRING "Launcher version, major.minor.patch")
    set(OPENBG_BUILD_NUMBER "1" CACHE STRING "Launcher build number")
    string(REPLACE "." "," OPENBG_VERSION_COMMA "${OPENBG_VERSION}")
    string(REGEX MATCH "^[0-9]+" build "${OPENBG_BUILD_NUMBER}")
    math(EXPR build "${build} % 65536")
    set(OPENBG_VERSION_COMMA "${OPENBG_VERSION_COMMA},${build}")
    set(windows ${CMAKE_CURRENT_SOURCE_DIR}/../windows)
    configure_file(${windows}/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/launcher_version.h @ONLY)

    add_library(launcher-core STATIC ${windows}/core.cpp)
    target_compile_features(launcher-core PUBLIC cxx_std_20)
    target_compile_definitions(launcher-core PUBLIC UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_link_libraries(launcher-core PUBLIC bcrypt shell32 ole32 uuid)
    target_compile_options(launcher-core PRIVATE -Wall -Wextra)

    add_executable(Battlegrounds WIN32 ${windows}/Launcher.cpp ${windows}/launcher.rc)
    target_include_directories(Battlegrounds PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${windows})
    target_link_libraries(Battlegrounds PRIVATE launcher-core gdiplus comctl32 dwmapi)
    add_executable(test-readiness ${windows}/test-readiness.cpp)
    target_link_libraries(test-readiness PRIVATE launcher-core)
    add_executable(test-launcher ${windows}/test-launcher.cpp)
    target_link_libraries(test-launcher PRIVATE launcher-core)
    foreach(target Battlegrounds test-readiness test-launcher)
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        if(MINGW)
            # wWinMain/wmain entry points; no compiler runtime DLLs next to the executables.
            target_link_options(${target} PRIVATE -municode -static -static-libgcc -static-libstdc++)
        endif()
    endforeach()
endif()
