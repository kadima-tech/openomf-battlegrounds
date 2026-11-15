#include "de/mods.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <system_error>

#include "de/contents.h"
#include "de/objects_file.h"
#include "de/platform.h"

namespace de {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
	for (char &c : s) c = char(tolower(uint8_t(c)));
	return s;
}

std::string same_file_key(const fs::path &p) {
	std::error_code ec;
	fs::path c = fs::weakly_canonical(p, ec);
	return (ec ? p.lexically_normal() : c).string();
}

std::vector<uint8_t> read_bytes(const fs::path &p) {
	std::ifstream f(p, std::ios::binary);
	if (!f) throw std::runtime_error("cannot read " + p.string());
	return {std::istreambuf_iterator<char>(f), {}};
}

}  // namespace

fs::path default_mods_file() { return user_data_dir() / "mods.txt"; }

std::vector<Mod> read_mods(const fs::path &file) {
	std::vector<Mod> out;
	std::set<std::string> seen;
	const fs::path base = file.parent_path();
	if (std::ifstream in(file); in) {
		for (std::string line; std::getline(in, line);) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			bool on = line.rfind("on ", 0) == 0, off = line.rfind("off ", 0) == 0;
			if (!on && !off) continue;
			fs::path p(line.substr(on ? 3 : 4));
			if (p.empty()) continue;
			if (p.is_relative()) p = base / p;
			if (seen.insert(same_file_key(p)).second) out.push_back({p, on});
		}
	}
	std::vector<fs::path> found;
	std::error_code ec;
	for (fs::directory_iterator it(base / "mods", ec), end; !ec && it != end; it.increment(ec)) {
		if (lower(it->path().extension().string()) == ".deobjects" && it->is_regular_file(ec)) found.push_back(it->path());
	}
	std::sort(found.begin(), found.end(), [](const fs::path &a, const fs::path &b) {
		return lower(a.filename().string()) < lower(b.filename().string());
	});
	for (const auto &p : found) {
		if (seen.insert(same_file_key(p)).second) out.push_back({p, true});
	}
	return out;
}

std::vector<fs::path> mods_to_load(const std::vector<Mod> &mods, const std::function<void(const std::string &)> &warn) {
	std::vector<fs::path> out;
	std::set<std::string> names;
	for (const auto &m : mods) {
		if (!m.enabled) continue;
		std::error_code ec;
		if (!fs::is_regular_file(m.path, ec)) {
			warn("mod not found, skipped: " + m.path.string());
			continue;
		}
		if (!names.insert(lower(m.path.filename().string())).second) {
			warn("another mod has this file name, skipped: " + m.path.string());
			continue;
		}
		try {
			ObjectsFile probe(read_bytes(m.path));
		} catch (const std::exception &ex) {
			warn("not an add-on, skipped: " + m.path.string() + " (" + ex.what() + ")");
			continue;
		}
		out.push_back(m.path);
	}
	return out;
}

std::vector<std::pair<std::string, std::string>> addon_contents(const fs::path &file) {
	static const std::pair<const char *, const char *> kinds[] = {
	    {"Description\\Pilot", "pilot"},
	    {"Description\\Creature\\Fighter", "robot"},
	    {"Description\\FighterModelDescription", "model"},
	    {"Description\\Environment", "arena"},
	    {"Description\\SessionMaster", "tournament"},
	};
	ObjectsFile f(read_bytes(file));
	std::vector<std::pair<std::string, std::string>> out;
	for (const auto &info : f.objects()) {
		for (auto [path, kind] : kinds) {
			if (info.class_path != path) continue;
			std::string name = info.name;
			try {
				Contents c(f.object_data(info).contents);
				begin_chunk(c);
				std::string shown = c.string();
				if (!shown.empty()) name = shown;
			} catch (const std::exception &) {
			}
			out.emplace_back(kind, name);
		}
	}
	return out;
}

}  // namespace de
