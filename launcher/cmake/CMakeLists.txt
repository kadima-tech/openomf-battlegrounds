cmake_minimum_required(VERSION 3.20)
project(openbg_launcher_runtime LANGUAGES C CXX)
include(FetchContent)

# Both libraries are linked statically. End users need no Homebrew packages.
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
FetchContent_MakeAvailable(unshield)

set(OPENINDEO_GIT_TAG "5452df0c8900f0892e701253481bf5c311a9a9d7" CACHE STRING "" FORCE)
set(OPENINDEO_DIR "${CMAKE_CURRENT_BINARY_DIR}/no-local-openindeo" CACHE PATH "" FORCE)
# Never bake disc-derived DirectX data into a redistributable app.
set(OPENBG_GAME_ISO "${CMAKE_CURRENT_BINARY_DIR}/no-bundled-game-data.iso" CACHE FILEPATH "" FORCE)
add_subdirectory(../../engine engine)
