// Per-user folders for the engine's own state (registry, recordings, snapshots).
#pragma once

#include <cstdlib>
#include <string>

namespace de {

inline std::string home_dir() {
	const char *home = getenv("HOME");
	return home && *home ? home : ".";
}

// macOS: ~/Library/Application Support/openbg. Elsewhere the XDG data folder:
// $XDG_DATA_HOME/openbg, or ~/.local/share/openbg when it is unset or relative.
inline std::string user_data_dir() {
#if defined(__APPLE__)
	return home_dir() + "/Library/Application Support/openbg";
#else
	const char *xdg = getenv("XDG_DATA_HOME");
	if (xdg && xdg[0] == '/') return std::string(xdg) + "/openbg";
	return home_dir() + "/.local/share/openbg";
#endif
}

// The folder snapshots are written to: ~/Pictures on every platform.
inline std::string pictures_dir() { return home_dir() + "/Pictures"; }

}  // namespace de
