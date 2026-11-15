// The player's mod list, shared with the launchers (launcher/README.md, "Mods"): mods.txt in the
// settings folder, one add-on per line in load order, "on <path>" or "off <path>" (a path relative
// to the file's folder is read from there). The "mods" folder next to it contributes every
// *.DEObjects the list does not name, enabled, after the list in name order.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace de {

struct Mod {
	std::filesystem::path path;
	bool enabled = true;
};

// user_data_dir() / "mods.txt".
std::filesystem::path default_mods_file();
// The list's entries, then the mods folder's unlisted files; nothing when neither exists.
std::vector<Mod> read_mods(const std::filesystem::path &file);
// The enabled mods that exist and parse, one per file name (the first); `warn` hears the others.
std::vector<std::filesystem::path> mods_to_load(const std::vector<Mod> &mods, const std::function<void(const std::string &)> &warn);

// What an add-on adds: {kind, name} for each pilot, robot, model, arena and tournament description,
// in file order. Throws when the file is not an objects file.
std::vector<std::pair<std::string, std::string>> addon_contents(const std::filesystem::path &file);

}  // namespace de
