// The few operating-system services the engine needs beyond standard C++: large lazily committed
// address ranges for the script arena and object pool, and the per-user folder that holds the
// game's settings and recordings. macOS and other POSIX systems use mmap; Windows uses
// VirtualAlloc.
#pragma once

#include <cstddef>
#include <filesystem>

namespace de {

// Call first in main(). On Windows, makes the C runtime treat narrow strings as UTF-8, the
// encoding of SDL's arguments and of the manifest's process code page, so std::filesystem's
// conversions between paths and strings keep every character of folders such as a
// user profile with an accented name. Needs the Universal C Runtime (MSYS2 UCRT64); elsewhere it does nothing.
void init_platform();

// Reserves `size` bytes of address space without committing memory. Returns nullptr on failure.
void *reserve_pages(size_t size);
// Makes [p, p + size) of a reserved range readable and writable (zero filled on first touch).
bool commit_pages(void *p, size_t size);
void release_pages(void *p, size_t size);

// The folder for the player's settings and recordings (created on demand by callers):
//   macOS:   ~/Library/Application Support/openbg
//   Windows: %APPDATA%\openbg
//   other:   $XDG_DATA_HOME/openbg, or ~/.local/share/openbg
std::filesystem::path user_data_dir();

// Where the replay controls' Snapshot saves pictures: the user's Pictures folder.
std::filesystem::path pictures_dir();

}  // namespace de
