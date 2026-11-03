// Tests for deobjects-edit. Without arguments they run on a synthetic file built here (the game's
// files are not redistributable); with a folder argument they also round-trip every .DEObjects
// file in it.
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
#include "json.h"
#include "model.h"

namespace fs = std::filesystem;
using deo::Bytes;

static int g_failures = 0;

#define CHECK(cond)                                                          \
	do {                                                                     \
		if (!(cond)) {                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++g_failures;                                                    \
		}                                                                    \
	} while (0)

namespace {

Bytes text(const char *s) {
	Bytes b(s, s + strlen(s));
	b.push_back(0);
	return b;
}

deo::Guid guid(uint8_t seed) {
	deo::Guid g;
	for (int i = 0; i < 16; ++i) g.b[i] = uint8_t(seed + i * 7);
	return g;
}

deo::Info info(const char *name, const char *cls, uint8_t seed, uint32_t flags = 0) {
	deo::Info i;
	i.name = text(name);
	i.class_path = text(cls);
	i.guid = guid(seed);
	i.value84 = seed;
	i.flags = flags;
	return i;
}

void f32(deo::Writer &w, float v) {
	uint32_t b;
	memcpy(&b, &v, 4);
	w.u32(b);
}

deo::File sample() {
	deo::File f;
	f.signature = text("Diversions Entertainment Objects File2");
	f.master = info("Test Arenas", "TransferAgent\\Win\\File", 1);
	f.guid_pool = {guid(50), guid(60)};

	// #0 an arena description.
	deo::Object arena;
	arena.info = info("The Test Pit", "Description\\Environment", 10);
	{
		deo::Writer w;
		size_t at = w.begin_chunk(3);
		w.block(text("The Test Pit"));
		w.u8(1);  // passive reference
		w.u8(32);
		w.guid(guid(70));
		w.guid(guid(80));
		w.u8(1);  // listed
		f32(w, 0.5f);
		for (uint8_t b : {0, 1, 0, 0, 0}) w.u8(b);
		w.bytes(Bytes(30, 0));
		w.end_chunk(at);
		at = w.begin_chunk(1);
		w.u32(2);
		w.u32(4);
		w.bytes(Bytes(20, 0));
		w.end_chunk(at);
		arena.contents = w.data();
	}
	{
		deo::Writer w;
		w.end_chunk(w.begin_chunk(2));
		size_t at = w.begin_chunk(6);
		w.u8(1), w.u8(2), w.u8(0);  // casts: ^0
		w.u8(1), w.u8(2), w.u8(1);  //        ^1
		w.u8(0);
		w.u8(1), w.u8(0x32 + 1);    // icon #1
		w.u8(0);                    // no large representation
		w.u8(0);                    // components
		w.u8(0);                    // replacements
		w.u8(1), w.u8(0x32 + 1);    // medium #1
		w.bytes(Bytes(0x28, 0));
		w.end_chunk(at);
		w.end_chunk(w.begin_chunk(1));
		arena.references = w.data();
	}
	{
		deo::Writer w;
		size_t at = w.begin_chunk(1);
		w.u8(1), w.u8(0x32 + 1), w.u8(0);
		w.end_chunk(at);
		arena.hierarchy = w.data();
	}
	f.objects.push_back(arena);

	// #1 a string value with extended data.
	deo::Object label;
	label.info = info("Label", "Value\\String", 20);
	{
		deo::Writer w;
		size_t at = w.begin_chunk(1);
		w.block(text("Label"));
		w.end_chunk(at);
		at = w.begin_chunk(2);
		w.block(text("Line one\r\nLine two"));
		w.end_chunk(at);
		label.contents = w.data();
	}
	label.references = {2, 0, 1, 0, 1, 0};
	label.hierarchy = {1, 1, 0};
	label.ex.push_back({text("OBJSavedTimeStamp"), 0, text("1004385878")});
	Bytes pixels(300);
	for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i * 13);
	label.ex.push_back({Bytes{0, 0}, 2, pixels});
	label.ex.push_back({Bytes{0, 1}, 2, Bytes{8, 0, 0, 0}});
	f.objects.push_back(label);

	// #2 a compressed value (method 0, stored).
	deo::Object value;
	value.info = info("Speed", "Value\\FLOAT", 30, deo::Object::kCompressed);
	{
		deo::Writer w;
		size_t at = w.begin_chunk(1);
		w.block(text("Speed"));
		w.end_chunk(at);
		at = w.begin_chunk(1);
		f32(w, 1.25f);
		w.end_chunk(at);
		value.contents = w.data();
		deo::Writer p;
		p.u8(0);
		p.block(value.contents);
		value.packed = p.data();
	}
	value.hierarchy = {1, 1, 0};
	f.objects.push_back(value);

	// #3 an unknown class with a long (u32-sized) chunk and odd bytes.
	deo::Object odd;
	odd.info = info("Mystery", "Something\\Unknown", 40);
	{
		deo::Writer w;
		size_t at = w.begin_chunk(7);
		w.bytes(Bytes{0xcd, 0xcd, 1, 2, 3});
		deo::ChunkSize long_size;
		long_size.long_size = true;
		w.end_chunk(at, long_size);
		odd.contents = w.data();
	}
	odd.references = {0xFF, 0xFF, 0xFF};  // not a chunk run
	f.objects.push_back(odd);
	return f;
}

struct Unpacked {
	json::Value index;
	std::vector<json::Value> objects;
	std::map<std::string, Bytes> blobs;
};

Unpacked unpack(const deo::File &f) {
	Unpacked u;
	auto save = [&](size_t object, const std::string &tag, const Bytes &data) {
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

deo::File pack(const Unpacked &u, std::vector<std::string> *warnings = nullptr) {
	return deo::file_from_json(u.index, u.objects, [&](const std::string &k) { return u.blobs.at(k); }, warnings);
}

const json::Value &chunk_fields(const json::Value &object, const char *block, size_t index) {
	return object.at(block).items().at(index).at("fields");
}

void test_container_round_trip() {
	Bytes data = sample().write();
	deo::File f = deo::File::read(data);
	CHECK(f.warnings.empty());
	CHECK(f.objects.size() == 4);
	CHECK(f.write() == data);
	CHECK(f.objects[2].contents.size() > 0 && f.objects[2].packed.size() > f.objects[2].contents.size());
}

void test_json_round_trip() {
	Bytes data = sample().write();
	deo::File f = deo::File::read(data);
	Unpacked u = unpack(f);
	CHECK(pack(u).write() == data);
	// Text and blobs: small text payloads inline, the 300-byte one as a blob.
	CHECK(u.objects[1].at("ex").items()[0].at("text").as_string() == "1004385878");
	CHECK(u.objects[1].at("ex").items()[1].has("file"));
	CHECK(u.objects[1].at("ex").items()[2].at("hex").as_string() == "08000000");
	CHECK(chunk_fields(u.objects[1], "contents", 1).at("text").as_string() == "Line one\r\nLine two");
}

void test_decoded_fields() {
	deo::File f = deo::File::read(sample().write());
	Unpacked u = unpack(f);
	const json::Value &desc = chunk_fields(u.objects[0], "contents", 0);
	CHECK(desc.at("name").as_string() == "The Test Pit");
	CHECK(desc.at("listed").as_bool());
	CHECK(desc.at("unknown_d4").as_real() == 0.5);
	CHECK(desc.at("resource").at("object").as_string() == guid(80).str());
	CHECK(chunk_fields(u.objects[0], "contents", 1).at("max_recommended_players").as_int() == 4);
	const json::Value &refs = chunk_fields(u.objects[0], "references", 1);
	CHECK(refs.at("casts").items().size() == 2);
	CHECK(refs.at("casts").items()[1].as_string() == "^1 " + guid(60).str());
	CHECK(refs.at("icon").as_string() == "#1 Label");
	CHECK(refs.at("large_representation").is_null());
	CHECK(refs.at("medium_representation").as_string() == "#1 Label");
	CHECK(chunk_fields(u.objects[0], "hierarchy", 0).at("children").items()[0].as_string() == "#1 Label");
	CHECK(chunk_fields(u.objects[2], "contents", 1).at("value").as_real() == 1.25);
	// Names for GUID pool entries, when given.
	deo::GuidNames names{{guid(60).str(), "\"Other\" in Elsewhere"}};
	json::Value named = deo::object_to_json(f, 0, nullptr, &names);
	CHECK(chunk_fields(named, "references", 1).at("casts").items()[1].as_string() ==
	      "^1 " + guid(60).str() + " \"Other\" in Elsewhere");
	// Unknown class: hex chunks, the long size kept; a block that is not chunks: hex.
	const json::Value &odd = u.objects[3].at("contents").items()[0];
	CHECK(odd.at("long_size").as_bool());
	CHECK(odd.at("hex").as_string() == "cdcd010203");
	CHECK(u.objects[3].at("references").at("hex").as_string() == "ffffff");
}

void test_edits() {
	deo::File f = deo::File::read(sample().write());
	Unpacked u = unpack(f);
	// Change values, rename, add a reference, edit the compressed object.
	json::Value &arena = u.objects[0];
	arena["contents"].items()[1]["fields"]["max_recommended_players"] = json::Value(int64_t(8));
	arena["contents"].items()[0]["fields"]["name"] = json::Value("The Renamed Pit");
	arena["references"].items()[1]["fields"]["large_representation"] = json::Value("#2 Speed");
	u.objects[1]["contents"].items()[1]["fields"]["text"] = json::Value("Caf\xc3\xa9");
	u.objects[2]["contents"].items()[1]["fields"]["value"] = json::Value(2.5);
	std::vector<std::string> warnings;
	Bytes data = pack(u, &warnings).write();
	CHECK(warnings.empty());
	deo::File g = deo::File::read(data);
	Unpacked v = unpack(g);
	CHECK(chunk_fields(v.objects[0], "contents", 1).at("max_recommended_players").as_int() == 8);
	CHECK(chunk_fields(v.objects[0], "contents", 0).at("name").as_string() == "The Renamed Pit");
	CHECK(chunk_fields(v.objects[0], "references", 1).at("large_representation").as_string() == "#2 Speed");
	CHECK(chunk_fields(v.objects[1], "contents", 1).at("text").as_string() == "Caf\xc3\xa9");
	// Latin-1 on disk.
	Bytes cafe = text("Caf\xe9");
	CHECK(std::search(g.objects[1].contents.begin(), g.objects[1].contents.end(), cafe.begin(), cafe.end()) !=
	      g.objects[1].contents.end());
	CHECK(chunk_fields(v.objects[2], "contents", 1).at("value").as_real() == 2.5);
	CHECK(g.objects[2].packed[0] == 0);  // re-stored with method 0
	CHECK(deo::decompress(g.objects[2].packed) == g.objects[2].contents);
	CHECK(g.write() == data);
	// Everything else is untouched.
	CHECK(g.objects[1].ex[1].data == f.objects[1].ex[1].data);
	CHECK(g.objects[3].contents == f.objects[3].contents);
}

void test_errors() {
	deo::File f = deo::File::read(sample().write());
	auto expect_error = [&](auto edit, const char *needle) {
		Unpacked u = unpack(f);
		edit(u);
		try {
			pack(u);
			fprintf(stderr, "expected an error containing \"%s\"\n", needle);
			++g_failures;
		} catch (const std::exception &e) {
			if (!strstr(e.what(), needle)) {
				fprintf(stderr, "error \"%s\" lacks \"%s\"\n", e.what(), needle);
				++g_failures;
			}
		}
	};
	expect_error([](Unpacked &u) { u.objects[0]["contents"].items()[1]["fields"]["max_players"] = json::Value(1); },
	             "not a field");
	expect_error([](Unpacked &u) { u.objects[0]["references"].items()[1]["fields"]["icon"] = json::Value("#9"); },
	             "no object #9");
	expect_error([](Unpacked &u) { u.objects[0]["references"].items()[1]["fields"]["icon"] = json::Value("^7"); },
	             "no GUID pool entry");
	expect_error([](Unpacked &u) { u.objects[1]["contents"].items()[1]["fields"]["text"] = json::Value("\xe2\x82\xac"); },
	             "Latin-1");
	expect_error(
	    [](Unpacked &u) {
		    u.objects[0]["contents"].items()[1]["fields"]["min_recommended_players"] = json::Value(int64_t(1) << 40);
	    },
	    "out of range");
	// A wrong GUID after a pool index only warns, as does a renamed target.
	{
		Unpacked w = unpack(f);
		w.objects[0]["references"].items()[1]["fields"]["casts"].items()[0] = json::Value("^0 " + guid(99).str());
		std::vector<std::string> notes;
		pack(w, &notes);
		CHECK(notes.size() == 1);
	}
	// A renamed target only warns.
	Unpacked u = unpack(f);
	u.objects[0]["references"].items()[1]["fields"]["icon"] = json::Value("#1 Wrong Name");
	std::vector<std::string> warnings;
	pack(u, &warnings);
	CHECK(warnings.size() == 1);
}

void test_json() {
	json::Value v = json::Value::parse(R"({"a": [1, 2.5, "xé\n"], "b": {"c": null, "d": true}, "e": -3})");
	CHECK(v.at("a").items()[2].as_string() == "x\xc3\xa9\n");
	CHECK(v.at("e").as_int() == -3);
	json::Value again = json::Value::parse(v.dump());
	CHECK(again.dump() == v.dump());
	bool threw = false;
	try {
		json::Value::parse("{\"a\": 1,}");
	} catch (const json::Error &) {
		threw = true;
	}
	CHECK(threw);
}

int verify_folder(const fs::path &dir) {
	int checked = 0;
	for (auto &entry : fs::recursive_directory_iterator(dir)) {
		std::string ext = entry.path().extension().string();
		for (char &c : ext) c = char(tolower(c));
		if (ext != ".deobjects") continue;
		std::ifstream in(entry.path(), std::ios::binary);
		Bytes data{std::istreambuf_iterator<char>(in), {}};
		try {
			deo::File f = deo::File::read(data);
			bool same = f.write() == data && pack(unpack(f)).write() == data;
			printf("%s %s\n", same ? "OK     " : "DIFFERS", entry.path().string().c_str());
			if (!same) ++g_failures;
		} catch (const std::exception &e) {
			printf("ERROR   %s: %s\n", entry.path().string().c_str(), e.what());
			++g_failures;
		}
		++checked;
	}
	printf("%d files checked\n", checked);
	return checked;
}

}  // namespace

int main(int argc, char **argv) {
	test_json();
	test_container_round_trip();
	test_json_round_trip();
	test_decoded_fields();
	test_edits();
	test_errors();
	if (argc > 1 && verify_folder(argv[1]) == 0) ++g_failures;
	if (g_failures) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
