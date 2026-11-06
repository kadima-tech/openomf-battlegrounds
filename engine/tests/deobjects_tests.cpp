// Writing .DEObjects files through tools/deobjects-editor's JSON form, checked with the engine's
// reader. Usage: deobjects-data-tests <Data dir> [--all]
//   default: edit a pilot's stat, a description's name and a compressed object in
//            Pilots.DEObjects, pack, reload with de::ObjectsFile and compare every object.
//   --all:   also unpack and pack every .DEObjects file in the folder; each must come back
//            byte for byte.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include "convert.h"
#include "de/objects_file.h"
#include "de/objects_model.h"
#include "de/platform.h"
#include "de/value.h"
#include "game/runtime.h"
#include "json.h"

namespace fs = std::filesystem;

static int failures = 0;

#define CHECK(cond)                                                       \
	do {                                                                  \
		if (!(cond)) {                                                    \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                   \
		}                                                                 \
	} while (0)

static std::vector<uint8_t> read_file(const fs::path &p) {
	std::ifstream f(p, std::ios::binary);
	return {std::istreambuf_iterator<char>(f), {}};
}

struct Unpacked {
	json::Value index;
	std::vector<json::Value> objects;
	std::map<std::string, deo::Bytes> blobs;
};

static Unpacked unpack(const deo::File &f) {
	Unpacked u;
	auto save = [&](size_t object, const std::string &tag, const deo::Bytes &data) {
		std::string key = std::to_string(object) + "." + tag;
		u.blobs[key] = data;
		return key;
	};
	std::vector<std::string> paths;
	for (size_t i = 0; i < f.objects.size(); ++i) {
		u.objects.push_back(json::Value::parse(deo::object_to_json(f, i, save).dump()));
		paths.push_back(std::to_string(i));
	}
	u.index = json::Value::parse(deo::index_to_json(f, paths).dump());
	return u;
}

static deo::Bytes pack(const Unpacked &u) {
	std::vector<std::string> warnings;
	deo::File f = deo::file_from_json(u.index, u.objects, [&](const std::string &k) { return u.blobs.at(k); }, &warnings);
	for (auto &w : warnings) fprintf(stderr, "pack: %s\n", w.c_str());
	return f.write();
}

// The stored contents block exactly as it is in the file (still compressed when flagged).
static std::vector<uint8_t> stored_block(const std::vector<uint8_t> &file, const de::ObjectInfo &o) {
	de::Contents c(file);
	c.seek(o.data_offset);
	return c.block();
}

static size_t find_object(const de::ObjectsFile &f, const std::string &class_path, const std::string &name = "") {
	for (size_t i = 0; i < f.objects().size(); ++i) {
		const de::ObjectInfo &o = f.objects()[i];
		if (o.class_path == class_path && (name.empty() || o.name == name)) return i;
	}
	return SIZE_MAX;
}

static void test_edit_and_reload(const fs::path &dir) {
	const std::vector<uint8_t> original = read_file(dir / "Pilots.DEObjects");
	const de::ObjectsFile before(original);
	deo::File model = deo::File::read(original);
	CHECK(model.write() == original);
	Unpacked u = unpack(model);

	// A pilot's power stat: the last contents chunk (DE_CPilot, v4) after three
	// {embedded colour, f32} pairs, as read_pilot reads it.
	const size_t pilot = find_object(before, "Actor\\Creature\\Pilot", "Elizabeth");
	CHECK(pilot != SIZE_MAX);
	if (pilot == SIZE_MAX) return;
	const auto old_pilot = game::read_pilot(before, before.objects()[pilot], 0);
	CHECK(old_pilot && old_pilot->stats.power != 77.5f);
	json::Value &chunks = u.objects[pilot]["contents"];
	json::Value &last = chunks.items().back();
	CHECK(last.at("version").as_int() == 4);
	std::string hex;
	for (const json::Value &line : last.at("hex").items()) hex += line.as_string();
	de::Contents body(deo::from_hex(hex));
	for (int k = 0; k < 3; ++k) {
		de::read_embedded_color(body);
		body.f32();
	}
	const float power = 77.5f;
	uint32_t bits;
	memcpy(&bits, &power, 4);
	deo::Bytes le = {uint8_t(bits), uint8_t(bits >> 8), uint8_t(bits >> 16), uint8_t(bits >> 24)};
	hex.replace(body.pos() * 2, 8, deo::to_hex(le));
	last["hex"] = json::Value(hex);

	// The pilot's description: its decoded name field.
	const size_t desc = find_object(before, "Description\\Pilot", "Elizabeth");
	CHECK(desc != SIZE_MAX);
	if (desc == SIZE_MAX) return;
	const std::string new_name = "Elizabeth (edited, longer)";
	json::Value &desc_chunk = u.objects[desc]["contents"].items()[0];
	CHECK(desc_chunk.at("fields").at("name").as_string() == "Elizabeth");
	desc_chunk["fields"]["name"] = json::Value(new_name);

	// A compressed object whose original stream is dropped: re-stored with method 0.
	size_t packed = SIZE_MAX;
	for (size_t i = 0; i < before.objects().size() && packed == SIZE_MAX; ++i) {
		if (before.objects()[i].flags & de::ObjectsFile::kFlagCompressed) packed = i;
	}
	CHECK(packed != SIZE_MAX);
	if (packed == SIZE_MAX) return;
	auto &members = u.objects[packed].members();
	auto it = std::find_if(members.begin(), members.end(), [](auto &m) { return m.first == "packed_file"; });
	CHECK(it != members.end());
	if (it != members.end()) members.erase(it);

	const std::vector<uint8_t> edited = pack(u);
	const de::ObjectsFile after(edited);
	CHECK(after.objects().size() == before.objects().size());
	CHECK(after.guid_pool().size() == before.guid_pool().size());
	if (after.objects().size() != before.objects().size()) return;

	const auto new_pilot = game::read_pilot(after, after.objects()[pilot], 0);
	CHECK(new_pilot && new_pilot->stats.power == 77.5f);
	CHECK(new_pilot && new_pilot->stats.agility == old_pilot->stats.agility);
	CHECK(new_pilot && new_pilot->stats.focus == old_pilot->stats.focus);
	CHECK(new_pilot && new_pilot->ai.size() == old_pilot->ai.size());

	de::Contents dc(after.object_data(after.objects()[desc]).contents);
	de::begin_chunk(dc);
	CHECK(dc.string() == new_name);

	const de::ObjectInfo &pa = after.objects()[packed];
	CHECK(pa.flags & de::ObjectsFile::kFlagCompressed);
	CHECK(stored_block(edited, pa).at(0) == 0);
	CHECK(after.object_data(pa).contents == before.object_data(before.objects()[packed]).contents);

	// Everything else is byte-identical, compressed streams and extended data included.
	size_t same = 0;
	for (size_t i = 0; i < before.objects().size(); ++i) {
		const de::ObjectInfo &a = before.objects()[i], &b = after.objects()[i];
		CHECK(a.name == b.name && a.class_path == b.class_path && a.flags == b.flags && a.value84 == b.value84);
		auto da = before.object_data(a), db = after.object_data(b);
		CHECK(da.references == db.references);
		CHECK(da.hierarchy == db.hierarchy);
		CHECK(da.ex.size() == db.ex.size());
		for (size_t k = 0; k < da.ex.size() && k < db.ex.size(); ++k) {
			CHECK(da.ex[k].header == db.ex[k].header);
			auto pa_ = before.ex_payload(da.ex[k]), pb = after.ex_payload(db.ex[k]);
			CHECK(pa_.flag == pb.flag && pa_.data == pb.data);
		}
		if (i == pilot || i == desc || i == packed) continue;
		bool identical = stored_block(original, a) == stored_block(edited, b);
		CHECK(identical);
		same += identical;
	}
	CHECK(same == before.objects().size() - 3);
}

static void test_round_trip_all(const fs::path &dir) {
	int checked = 0;
	for (auto &entry : fs::directory_iterator(dir)) {
		std::string ext = entry.path().extension().string();
		for (char &c : ext) c = char(tolower(uint8_t(c)));
		if (ext != ".deobjects") continue;
		++checked;
		const std::vector<uint8_t> data = read_file(entry.path());
		try {
			deo::File f = deo::File::read(data);
			bool same = f.write() == data && deo::repack(f).write() == data;
			if (!same) fprintf(stderr, "round trip differs: %s\n", entry.path().string().c_str());
			CHECK(same);
		} catch (const std::exception &e) {
			fprintf(stderr, "%s: %s\n", entry.path().string().c_str(), e.what());
			++failures;
		}
	}
	printf("%d files round-tripped\n", checked);
	CHECK(checked > 0);
}

int main(int argc, char **argv) {
	de::init_platform();
	if (argc < 2) {
		fprintf(stderr, "usage: deobjects-data-tests <Data dir> [--all]\n");
		return 2;
	}
	try {
		test_edit_and_reload(argv[1]);
		if (argc > 2 && !strcmp(argv[2], "--all")) test_round_trip_all(argv[1]);
	} catch (const std::exception &e) {
		fprintf(stderr, "%s\n", e.what());
		++failures;
	}
	printf("%s (%d failures)\n", failures ? "FAIL" : "OK", failures);
	return failures != 0;
}
