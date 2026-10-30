// deobj-diff: compare two versions of a .DEObjects file (the official patches, PATCHES.md).
//
//   deobj-diff <old> <new> [detail-dir]
//
// Objects are matched by GUID. Prints added / removed / renamed objects and, per changed object,
// which blocks differ; Behavior sources by element and compiled Class methods by signature.
// With a detail dir, writes old/new text of each changed object there (script source, method
// disassembly, contents chunks) for a local `diff`; that output is game data, keep it out of git.
//
// Build: re/scripts/build_deobj_diff.sh <build dir>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include "de/contents.h"
#include "de/objects_file.h"
#include "de/pcode.h"
#include "vm/disasm.h"
#include "vm/vmcode.h"

namespace fs = std::filesystem;

static std::vector<uint8_t> read_file(const char *path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) throw std::runtime_error(std::string("cannot open ") + path);
	return {std::istreambuf_iterator<char>(f), {}};
}

static std::string key_of(const de::ObjectsFile::ObjectData::EX &e) {
	std::string s;
	for (uint8_t b : e.header) {
		char t[4];
		snprintf(t, sizeof t, b >= 32 && b < 127 ? "%c" : "%02x", b);
		s += t;
	}
	return s;
}

static std::string safe(std::string s) {
	for (auto &c : s)
		if (c == '/' || c == '\\' || c == ':' || c == ' ') c = '_';
	return s;
}

struct Loaded {
	de::ObjectsFile file;
	std::map<std::string, size_t> by_guid;
	explicit Loaded(const char *p) : file(read_file(p)) {
		for (size_t i = 0; i < file.objects().size(); ++i) by_guid[file.objects()[i].guid.str()] = i;
	}
};

static std::map<std::string, std::string> behavior_elements(const de::ObjectsFile::ObjectData &d) {
	std::map<std::string, std::string> out;
	de::Contents c(d.contents);
	auto p = de::PCode::parse(c);
	out["(import)"] = p.import.source;
	out["(global)"] = p.global.source;
	out["(members)"] = p.members.source;
	for (size_t i = 0; i < p.elements.size(); ++i) {
		auto &e = p.elements[i];
		std::string k = p.qualified_name(i);
		std::string v = e.source + "|m" + std::to_string(e.method_id) + (e.virtual_ ? "v" : "") + (e.excluded ? "x" : "");
		while (out.count(k)) k += "~";
		out[k] = v;
	}
	return out;
}

static std::map<std::string, std::vector<uint8_t>> class_methods(const de::ObjectsFile &f, const de::ObjectsFile::ObjectData &d,
                                                                 std::string *disasm) {
	std::map<std::string, std::vector<uint8_t>> out;
	auto code = f.ex_payload(d, "VMC2");
	if (!code) return out;
	auto v = vm::VMClass::parse(code->data, vm::class_behavior_refs(d.references).size());
	out["(change info)"] = v.change_info;
	std::string names;
	for (auto &k : v.classes) names += k.name + ";";
	out["(classes)"].assign(names.begin(), names.end());
	names.clear();
	for (auto &i : v.intrinsics) names += std::to_string(i.class_index) + i.signature + ";";
	out["(intrinsics)"].assign(names.begin(), names.end());
	names.clear();
	for (auto &g : v.global_refs) names += std::to_string(g.kind) + g.name + ";";
	out["(global refs)"].assign(names.begin(), names.end());
	auto &fin = out["(finals)"];
	for (auto &x : v.finals) fin.insert(fin.end(), x.data.begin(), x.data.end());
	fin.insert(fin.end(), v.tail.begin(), v.tail.end());
	for (auto &b : v.behaviors)
		for (auto &m : b.methods) {
			if (!m) continue;
			std::string k = m->signature;
			while (out.count(k)) k += "~";
			out[k] = m->code;
			if (disasm) {
				*disasm += "\n" + m->signature + "\n";
				try {
					for (auto &ins : vm::decode(m->code)) *disasm += "  " + vm::format(ins) + "\n";
				} catch (const std::exception &ex) {
					*disasm += std::string("  <decode error: ") + ex.what() + ">\n";
				}
			}
		}
	if (disasm) {
		std::string head = "classes:";
		for (auto &k : v.classes) head += " " + k.name;
		head += "\nglobal refs:";
		for (auto &g : v.global_refs) head += " " + g.name;
		head += "\nintrinsics:\n";
		for (auto &i : v.intrinsics) head += "  " + v.classes[i.class_index].name + "::" + i.signature + "\n";
		*disasm = head + *disasm;
	}
	return out;
}

static std::string chunk_text(const std::vector<uint8_t> &data) {
	std::string s;
	de::Contents c(data);
	char buf[64];
	while (c.remaining() > 1) {
		size_t start = c.pos();
		de::Chunk ch;
		try {
			ch = de::begin_chunk(c);
		} catch (...) {
			break;
		}
		size_t body = c.pos();
		if (ch.end > c.size() || ch.end < body) {
			snprintf(buf, sizeof buf, "@%zu not a chunk\n", start);
			s += buf;
			break;
		}
		snprintf(buf, sizeof buf, "chunk @%zu v%u %zu bytes\n", start, ch.version, ch.end - body);
		s += buf;
		for (size_t k = body; k + 4 <= ch.end; k += 4) {
			float fv;
			int32_t iv;
			memcpy(&fv, data.data() + k, 4);
			memcpy(&iv, data.data() + k, 4);
			snprintf(buf, sizeof buf, "  +%zu %08x i=%d f=%g\n", k - body, uint32_t(iv), iv, fv);
			s += buf;
		}
		c.seek(ch.end);
	}
	return s;
}

static void write(const fs::path &p, const std::string &s) { std::ofstream(p, std::ios::binary) << s; }

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <old.DEObjects> <new.DEObjects> [detail-dir]\n", argv[0]);
		return 2;
	}
	Loaded a(argv[1]), b(argv[2]);
	const char *detail = argc > 3 ? argv[3] : nullptr;
	if (detail) fs::create_directories(detail);
	auto &ao = a.file.objects();
	auto &bo = b.file.objects();
	printf("objects %zu -> %zu\n", ao.size(), bo.size());
	for (auto &[g, i] : a.by_guid)
		if (!b.by_guid.count(g)) printf("removed  %s [%s] %s\n", ao[i].name.c_str(), ao[i].class_path.c_str(), g.c_str());
	for (auto &[g, i] : b.by_guid)
		if (!a.by_guid.count(g)) printf("added    %s [%s] %s\n", bo[i].name.c_str(), bo[i].class_path.c_str(), g.c_str());
	for (auto &[g, j] : b.by_guid) {
		auto it = a.by_guid.find(g);
		if (it == a.by_guid.end()) continue;
		auto &oa = ao[it->second];
		auto &ob = bo[j];
		std::string what;
		if (oa.name != ob.name) what += " renamed(" + oa.name + ")";
		if (oa.class_path != ob.class_path) what += " class(" + oa.class_path + ")";
		auto da = a.file.object_data(oa), db = b.file.object_data(ob);
		if (da.contents != db.contents) what += " contents(" + std::to_string(da.contents.size()) + "->" + std::to_string(db.contents.size()) + ")";
		if (da.references != db.references) what += " references";
		if (da.hierarchy != db.hierarchy) what += " hierarchy";
		std::map<std::string, std::vector<uint8_t>> exa, exb;
		for (auto &e : da.ex) exa[key_of(e)] = a.file.ex_payload(e).data;
		for (auto &e : db.ex) exb[key_of(e)] = b.file.ex_payload(e).data;
		for (auto &[k, v] : exa)
			if (!exb.count(k)) what += " ex-removed(" + k + ")";
			else if (exb[k] != v) what += " ex(" + k + ")";
		for (auto &[k, v] : exb)
			if (!exa.count(k)) what += " ex-added(" + k + ")";
		if (what.empty()) continue;
		printf("changed  %s [%s]%s\n", ob.name.c_str(), ob.class_path.c_str(), what.c_str());
		std::string base = detail ? (fs::path(detail) / safe(ob.name + "." + ob.class_path + "." + g.substr(0, 8))).string() : "";
		try {
			if (ob.class_path == "Behavior" && da.contents != db.contents) {
				auto ea = behavior_elements(da), eb = behavior_elements(db);
				for (auto &[k, v] : ea)
					if (!eb.count(k)) printf("    element removed %s\n", k.c_str());
					else if (eb[k] != v) printf("    element changed %s\n", k.c_str());
				for (auto &[k, v] : eb)
					if (!ea.count(k)) printf("    element added %s\n", k.c_str());
				if (detail) {
					de::Contents ca(da.contents), cb(db.contents);
					write(base + ".old.dec", de::PCode::parse(ca).text());
					write(base + ".new.dec", de::PCode::parse(cb).text());
				}
			} else if (ob.class_path == "Class") {
				std::string sa, sb;
				auto ma = class_methods(a.file, da, detail ? &sa : nullptr), mb = class_methods(b.file, db, detail ? &sb : nullptr);
				int real = 0;
				for (auto &[k, v] : ma)
					if (!mb.count(k)) printf("    method removed %s\n", k.c_str()), ++real;
					else if (mb[k] != v && k[0] == '(') printf("    %s changed\n", k.c_str()), real += k != "(change info)";
					else if (mb[k] != v) printf("    method changed %s (%zu -> %zu bytes)\n", k.c_str(), v.size(), mb[k].size()), ++real;
				for (auto &[k, v] : mb)
					if (!ma.count(k)) printf("    method added %s\n", k.c_str()), ++real;
				if (!real && da.contents == db.contents && da.references == db.references) printf("    (recompiled only)\n");
				if (detail) {
					write(base + ".old.dis", sa);
					write(base + ".new.dis", sb);
				}
			} else if (detail && da.contents != db.contents) {
				write(base + ".old.chunks", chunk_text(da.contents));
				write(base + ".new.chunks", chunk_text(db.contents));
			}
			if (detail && da.references != db.references) {
				write(base + ".old.refs", chunk_text(da.references));
				write(base + ".new.refs", chunk_text(db.references));
			}
		} catch (const std::exception &ex) {
			printf("    (decode error: %s)\n", ex.what());
		}
	}
	auto &ga = a.file.guid_pool(), &gb = b.file.guid_pool();
	if (ga.size() != gb.size()) printf("external guid pool %zu -> %zu\n", ga.size(), gb.size());
	return 0;
}
