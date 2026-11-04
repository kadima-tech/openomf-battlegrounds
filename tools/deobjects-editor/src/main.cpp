// deobjects-edit: inspect and edit One Must Fall: Battlegrounds .DEObjects files.
//
//   deobjects-edit list   <file>               the object table
//   deobjects-edit tree   <file>               objects as their parent/child hierarchy
//   deobjects-edit show   <file> <object>      one object as JSON (#index or name)
//   deobjects-edit unpack <file> <dir>         write the file as editable JSON + binary files
//   deobjects-edit pack   <dir> <file>         build a .DEObjects file from an unpacked folder
//   deobjects-edit verify <file>...            check that unpack + pack gives the same bytes
//   deobjects-edit schemas                     the class layouts the editor decodes
//
//   --data <dir>  (show, unpack) name objects in other files that references point at
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "convert.h"
#include "json.h"
#include "model.h"
#include "schema.h"

namespace fs = std::filesystem;
using deo::Bytes;

namespace {

constexpr const char *kIndexName = "deobjects.json";

Bytes read_bytes(const fs::path &p) {
	std::ifstream in(p, std::ios::binary);
	if (!in) throw deo::Error("cannot open " + p.string());
	return Bytes(std::istreambuf_iterator<char>(in), {});
}

void write_bytes(const fs::path &p, const Bytes &b) {
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	if (!out) throw deo::Error("cannot write " + p.string());
	out.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
	if (!out) throw deo::Error("error writing " + p.string());
}

void write_text(const fs::path &p, const std::string &s) {
	write_bytes(p, Bytes(s.begin(), s.end()));
}

json::Value read_json(const fs::path &p) {
	Bytes b = read_bytes(p);
	try {
		return json::Value::parse(std::string(b.begin(), b.end()));
	} catch (const json::Error &e) {
		throw deo::Error(p.string() + ": " + e.what());
	}
}

// The object table of every .DEObjects file in a folder (the game's Data folder), read from
// the start of each file only: GUID text -> "<name> (<file>)".
deo::GuidNames scan_names(const fs::path &dir) {
	deo::GuidNames names;
	for (auto &entry : fs::directory_iterator(dir)) {
		std::string ext = entry.path().extension().string();
		for (char &c : ext) c = char(tolower(c));
		if (ext != ".deobjects") continue;
		std::ifstream in(entry.path(), std::ios::binary);
		auto take = [&](Bytes &b, size_t n) {
			size_t at = b.size();
			b.resize(at + n);
			in.read(reinterpret_cast<char *>(b.data() + at), std::streamsize(n));
			if (size_t(in.gcount()) != n) throw deo::Error("truncated");
		};
		auto u32_at = [](const Bytes &b, size_t p) {
			return uint32_t(b[p]) | uint32_t(b[p + 1]) << 8 | uint32_t(b[p + 2]) << 16 | uint32_t(b[p + 3]) << 24;
		};
		try {
			Bytes head;
			take(head, 4);
			uint32_t sig = u32_at(head, 0);
			if (sig > 256) continue;
			take(head, sig + 1 + 4 + 4);
			take(head, u32_at(head, head.size() - 4));
			deo::File f = deo::File::read(head, true);
			std::string file = entry.path().stem().string();
			for (const deo::Object &o : f.objects) {
				std::string n = o.info.name_text();
				names[o.info.guid.str()] = "\"" + deo::latin1_to_utf8(Bytes(n.begin(), n.end())) + "\" in " + file;
			}
		} catch (const std::exception &) {
			fprintf(stderr, "warning: cannot read the object table of %s\n", entry.path().string().c_str());
		}
	}
	return names;
}

std::string name_of(const deo::Object &o) {
	std::string n = o.info.name_text();
	return deo::latin1_to_utf8(Bytes(n.begin(), n.end()));
}

std::string class_of(const deo::Object &o) {
	std::string n = o.info.class_text();
	return deo::latin1_to_utf8(Bytes(n.begin(), n.end()));
}

// "#12" or an object name (the first match).
size_t find_object(const deo::File &f, const std::string &spec) {
	if (spec.size() > 1 && spec[0] == '#') {
		char *end = nullptr;
		unsigned long i = strtoul(spec.c_str() + 1, &end, 10);
		if (*end == 0 && i < f.objects.size()) return i;
	}
	for (size_t i = 0; i < f.objects.size(); ++i) {
		if (name_of(f.objects[i]) == spec) return i;
	}
	throw deo::Error("no object \"" + spec + "\" (use a name or #index; see the list command)");
}

// Children of each object from the hierarchy blocks (local references only).
std::vector<std::vector<size_t>> children_of(const deo::File &f) {
	std::vector<std::vector<size_t>> out(f.objects.size());
	for (size_t i = 0; i < f.objects.size(); ++i) {
		json::Value h = deo::object_to_json(f, i, nullptr).at("hierarchy");
		if (!h.is_array() || h.size() == 0) continue;
		const json::Value *kids = h.items()[0].find("fields");
		if (!kids || !kids->has("children")) continue;
		for (const json::Value &r : kids->at("children").items()) {
			if (r.is_string() && r.as_string()[0] == '#') {
				size_t k = strtoul(r.as_string().c_str() + 1, nullptr, 10);
				if (k < f.objects.size()) out[i].push_back(k);
			}
		}
	}
	return out;
}

int cmd_list(const fs::path &file) {
	deo::File f = deo::File::read(read_bytes(file));
	printf("%s: %zu objects, %zu GUID pool entries\n", file.filename().string().c_str(), f.objects.size(),
	       f.guid_pool.size());
	printf("%5s  %-40s %-36s %8s %4s  %s\n", "#", "name", "class", "contents", "ex", "flags");
	for (size_t i = 0; i < f.objects.size(); ++i) {
		const deo::Object &o = f.objects[i];
		printf("%5zu  %-40s %-36s %8zu %4zu  %s\n", i, name_of(o).c_str(), class_of(o).c_str(), o.contents.size(),
		       o.ex.size(), o.compressed() ? "compressed" : "");
	}
	return 0;
}

int cmd_tree(const fs::path &file) {
	deo::File f = deo::File::read(read_bytes(file));
	auto kids = children_of(f);
	std::vector<bool> has_parent(f.objects.size());
	for (auto &k : kids) {
		for (size_t c : k) has_parent[c] = true;
	}
	std::vector<bool> seen(f.objects.size());
	std::function<void(size_t, int)> show = [&](size_t i, int depth) {
		printf("%*s#%zu %s  [%s]\n", depth * 2, "", i, name_of(f.objects[i]).c_str(), class_of(f.objects[i]).c_str());
		if (seen[i]) return;
		seen[i] = true;
		for (size_t c : kids[i]) show(c, depth + 1);
	};
	for (size_t i = 0; i < f.objects.size(); ++i) {
		if (!has_parent[i]) show(i, 0);
	}
	return 0;
}

int cmd_show(const fs::path &file, const std::string &spec, const deo::GuidNames *names) {
	deo::File f = deo::File::read(read_bytes(file));
	size_t i = find_object(f, spec);
	printf("%s\n", deo::object_to_json(f, i, nullptr, names).dump().c_str());
	return 0;
}

// Builds the JSON form in memory and packs it again: the result must equal `original`.
bool round_trip(const deo::File &f, const Bytes &original, std::string *why) {
	std::map<std::string, Bytes> blobs;
	auto save = [&](size_t object, const std::string &tag, const Bytes &data) {
		std::string key = std::to_string(object) + "." + tag;
		blobs[key] = data;
		return key;
	};
	auto load = [&](const std::string &key) { return blobs.at(key); };
	std::vector<std::string> paths;
	std::vector<json::Value> objects;
	for (size_t i = 0; i < f.objects.size(); ++i) {
		// Through text, as the files on disk would be.
		objects.push_back(json::Value::parse(deo::object_to_json(f, i, save).dump()));
		paths.push_back(std::to_string(i));
	}
	json::Value index = json::Value::parse(deo::index_to_json(f, paths).dump());
	Bytes again = deo::file_from_json(index, objects, load, nullptr).write();
	if (again == original) return true;
	size_t at = 0;
	while (at < again.size() && at < original.size() && again[at] == original[at]) ++at;
	*why = "first difference at byte " + std::to_string(at) + " (sizes " + std::to_string(original.size()) + " and " +
	       std::to_string(again.size()) + ")";
	return false;
}

int cmd_verify(const std::vector<fs::path> &files) {
	int failed = 0;
	for (const fs::path &p : files) {
		try {
			Bytes data = read_bytes(p);
			deo::File f = deo::File::read(data);
			std::string why;
			if (f.write() != data) {
				printf("DIFFERS  %s: the container does not rebuild identically\n", p.string().c_str());
				for (auto &w : f.warnings) printf("         %s\n", w.c_str());
				++failed;
			} else if (!round_trip(f, data, &why)) {
				printf("DIFFERS  %s: JSON round trip, %s\n", p.string().c_str(), why.c_str());
				++failed;
			} else {
				printf("OK       %s (%zu objects)\n", p.string().c_str(), f.objects.size());
			}
		} catch (const std::exception &e) {
			printf("ERROR    %s: %s\n", p.string().c_str(), e.what());
			++failed;
		}
	}
	return failed ? 1 : 0;
}

int cmd_unpack(const fs::path &file, const fs::path &dir, bool force, const deo::GuidNames *names) {
	Bytes data = read_bytes(file);
	deo::File f = deo::File::read(data);
	if (fs::exists(dir / kIndexName) && !force) {
		throw deo::Error((dir / kIndexName).string() + " already exists (add --force to overwrite)");
	}
	fs::create_directories(dir / "objects");
	fs::create_directories(dir / "blobs");
	auto save = [&](size_t object, const std::string &tag, const Bytes &bytes) {
		char name[64];
		snprintf(name, sizeof name, "blobs/%04zu.%s.bin", object, tag.c_str());
		write_bytes(dir / name, bytes);
		return std::string(name);
	};
	std::vector<std::string> paths;
	for (size_t i = 0; i < f.objects.size(); ++i) {
		std::string rel = "objects/" + deo::object_file_name(f, i);
		write_text(dir / rel, deo::object_to_json(f, i, save, names).dump() + "\n");
		paths.push_back(rel);
	}
	write_text(dir / kIndexName, deo::index_to_json(f, paths).dump() + "\n");
	printf("unpacked %zu objects to %s\n", f.objects.size(), dir.string().c_str());
	std::string why;
	if (f.write() != data || !round_trip(f, data, &why)) {
		printf("warning: packing this folder unchanged will not give identical bytes%s%s\n", why.empty() ? "" : ": ",
		       why.c_str());
		for (auto &w : f.warnings) printf("  %s\n", w.c_str());
	} else {
		printf("verified: packing it unchanged gives back the identical file\n");
	}
	return 0;
}

int cmd_pack(const fs::path &dir, const fs::path &out) {
	json::Value index = read_json(dir / kIndexName);
	std::vector<json::Value> objects;
	for (const json::Value &p : index.at("objects").items()) objects.push_back(read_json(dir / p.as_string()));
	auto load = [&](const std::string &rel) { return read_bytes(dir / rel); };
	std::vector<std::string> warnings;
	deo::File f = deo::file_from_json(index, objects, load, &warnings);
	for (auto &w : warnings) fprintf(stderr, "warning: %s\n", w.c_str());
	Bytes data = f.write();
	// Read it back as a sanity check before replacing anything.
	deo::File::read(data);
	fs::path tmp = out;
	tmp += ".tmp";
	write_bytes(tmp, data);
	// Not every C++ library's rename replaces an existing file on Windows.
	if (fs::exists(out)) fs::remove(out);
	fs::rename(tmp, out);
	printf("packed %zu objects into %s (%zu bytes)\n", f.objects.size(), out.string().c_str(), data.size());
	return 0;
}

int cmd_schemas() {
	const char *blocks[] = {"contents", "references", "hierarchy"};
	for (const deo::ChunkSchema &s : deo::all_schemas()) {
		printf("%s chunk %d of %s (%s)\n", blocks[int(s.block)], s.index,
		       *s.class_prefix ? s.class_prefix : "every class", s.label);
		for (const deo::Field &fd : s.fields) {
			printf("    %-28s %-20s", fd.name, deo::type_name(fd.type));
			if (fd.min_version) printf(" from v%d", fd.min_version);
			printf("\n");
		}
		printf("    source: %s\n\n", s.source);
	}
	return 0;
}

int usage() {
	fprintf(stderr,
	        "deobjects-edit: inspect and edit One Must Fall: Battlegrounds .DEObjects files\n\n"
	        "  deobjects-edit list   <file>                 list the objects\n"
	        "  deobjects-edit tree   <file>                 show the object hierarchy\n"
	        "  deobjects-edit show   <file> <object>        print one object as JSON (name or #index)\n"
	        "  deobjects-edit unpack <file> <dir> [--force] write editable JSON to <dir>\n"
	        "  deobjects-edit pack   <dir> <file>           build a .DEObjects file from <dir>\n"
	        "  deobjects-edit verify <file>...              check files survive unpack + pack unchanged\n"
	        "  deobjects-edit schemas                       list the class layouts it decodes\n\n"
	        "  --data <dir>  with show/unpack: name the objects that references into other files\n"
	        "                point at, from the .DEObjects files in <dir> (the game's Data folder)\n");
	return 2;
}

}  // namespace

int main(int argc, char **argv) {
	std::vector<std::string> args(argv + 1, argv + argc);
	bool force = false;
	std::string data_dir;
	for (auto it = args.begin(); it != args.end();) {
		if (*it == "--force") {
			force = true;
			it = args.erase(it);
		} else if (*it == "--data" && it + 1 != args.end()) {
			data_dir = *(it + 1);
			it = args.erase(it, it + 2);
		} else {
			++it;
		}
	}
	if (args.empty()) return usage();
	const std::string &cmd = args[0];
	try {
		deo::GuidNames names;
		if (!data_dir.empty()) names = scan_names(data_dir);
		const deo::GuidNames *n = data_dir.empty() ? nullptr : &names;
		if (cmd == "list" && args.size() == 2) return cmd_list(args[1]);
		if (cmd == "tree" && args.size() == 2) return cmd_tree(args[1]);
		if (cmd == "show" && args.size() == 3) return cmd_show(args[1], args[2], n);
		if (cmd == "unpack" && args.size() == 3) return cmd_unpack(args[1], args[2], force, n);
		if (cmd == "pack" && args.size() == 3) return cmd_pack(args[1], args[2]);
		if (cmd == "verify" && args.size() >= 2) return cmd_verify({args.begin() + 1, args.end()});
		if (cmd == "schemas" && args.size() == 1) return cmd_schemas();
	} catch (const std::exception &e) {
		fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
	return usage();
}
