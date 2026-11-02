# Cross-compiling the Windows x64 build from Linux or macOS with MinGW-w64
# (Debian/Ubuntu: apt install g++-mingw-w64-x86-64-posix; Homebrew: brew install mingw-w64).
# build-windows.sh uses it automatically outside MSYS2.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(triple x86_64-w64-mingw32)
# Debian's -posix variants provide std::thread; Homebrew's default compilers already do.
find_program(CMAKE_C_COMPILER NAMES ${triple}-gcc-posix ${triple}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${triple}-g++-posix ${triple}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${triple}-windres REQUIRED)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
