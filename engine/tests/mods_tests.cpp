// The player's mod list (de/mods.h) as the launchers write it, without game data.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "de/contents.h"
#include "de/mods.h"
#include "de/objects_file.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool ok, const char *what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++failures;
}

void write_text(const fs::path &p, const std::string &text) {
	std::ofstream(p, std::ios::binary) << text;
}

void write_addon(const fs::path &p, const std::string &pilot) {
	de::ContentsWriter w;
	size_t at = w.begin_chunk(3);
	w.string(pilot);
	w.u8(0);
	w.end_chunk(at);
	de::NewObject o{pilot, "Description\\Pilot", de::new_guid(), w.take(), {}, {}};
	auto bytes = de::write_objects_file(p.stem().string(), de::new_guid(), {o});
	std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
}

std::vector<std::string> names(const std::vector<fs::path> &paths) {
	std::vector<std::string> out;
	for (const auto &p : paths) out.push_back(p.filename().string());
	return out;
}

}  // namespace

int main() {
	fs::path root = fs::temp_directory_path() / ("openbg-mods-tests-" + de::new_guid().str());
	fs::create_directories(root / "mods");
	fs::create_directories(root / "elsewhere");
	write_addon(root / "elsewhere" / "Zed.DEObjects", "Zed Pilot");
	write_addon(root / "elsewhere" / "Off.DEObjects", "Off Pilot");
	write_addon(root / "mods" / "Alpha.DEObjects", "Alpha Pilot");
	write_addon(root / "mods" / "beta.deobjects", "Beta Pilot");
	write_text(root / "mods" / "Broken.DEObjects", "not an objects file");
	write_text(root / "mods" / "notes.txt", "ignored");
	fs::path list = root / "mods.txt";

	check(de::read_mods(root / "missing" / "mods.txt").empty(), "no list and no mods folder: nothing");

	write_text(list, "# OpenBG mods\r\n"
	                 "on " + (root / "elsewhere" / "Zed.DEObjects").string() + "\r\n"
	                 "off elsewhere/Off.DEObjects\n"
	                 "on mods/beta.deobjects\n"
	                 "on " + (root / "elsewhere" / "Zed.DEObjects").string() + "\n"
	                 "maybe elsewhere/Off.DEObjects\n"
	                 "on elsewhere/Missing.DEObjects\n");
	auto mods = de::read_mods(list);
	std::vector<std::string> order;
	for (const auto &m : mods) order.push_back(m.path.filename().string() + (m.enabled ? "+" : "-"));
	check(order == std::vector<std::string>({"Zed.DEObjects+", "Off.DEObjects-", "beta.deobjects+", "Missing.DEObjects+",
	                                          "Alpha.DEObjects+", "Broken.DEObjects+"}),
	      "list order, CRLF, relative paths, duplicates once, unlisted mods folder files after, enabled");

	std::vector<std::string> warnings;
	auto load = de::mods_to_load(mods, [&](const std::string &w) { warnings.push_back(w); });
	check(names(load) == std::vector<std::string>({"Zed.DEObjects", "beta.deobjects", "Alpha.DEObjects"}),
	      "loads the enabled, existing, valid add-ons in order");
	check(warnings.size() == 2, "warns about the missing and the broken file");

	fs::create_directories(root / "copy");
	fs::copy_file(root / "mods" / "Alpha.DEObjects", root / "copy" / "Alpha.DEObjects");
	write_text(list, "on copy/Alpha.DEObjects\n");
	warnings.clear();
	load = de::mods_to_load(de::read_mods(list), [&](const std::string &w) { warnings.push_back(w); });
	check(load.size() == 2 && load[0] == root / "copy" / "Alpha.DEObjects" && warnings.size() == 2,
	      "two mods with one file name: the first loads");

	write_text(list, "off mods/Alpha.DEObjects\noff mods/beta.deobjects\noff mods/Broken.DEObjects\n");
	check(de::mods_to_load(de::read_mods(list), [](const std::string &) {}).empty(), "a disabled mods folder file stays off");

	auto contents = de::addon_contents(root / "mods" / "Alpha.DEObjects");
	check(contents.size() == 1 && contents[0].first == "pilot" && contents[0].second == "Alpha Pilot", "addon_contents names the pilot");
	bool threw = false;
	try {
		de::addon_contents(root / "mods" / "Broken.DEObjects");
	} catch (const std::exception &) {
		threw = true;
	}
	check(threw, "addon_contents rejects a file that is not an add-on");

	fs::remove_all(root);
	printf("%s\n", failures ? "FAILED" : "all mods tests passed");
	return failures ? 1 : 0;
}
