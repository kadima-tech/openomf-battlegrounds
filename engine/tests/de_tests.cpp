// Data-layer tests against the original game files. Usage: de-tests <Data dir>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "de/avi.h"
#include "de/contents.h"
#include "de/image.h"
#include "de/mesh.h"
#include "de/md5.h"
#include "de/objects_file.h"
#include "vm/disasm.h"
#include "vm/executor.h"
#include "vm/script.h"
#include "vm/vmcode.h"

static int failures = 0;

#define CHECK(cond)                                                       \
	do {                                                                  \
		if (!(cond)) {                                                    \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                   \
		}                                                                 \
	} while (0)

static std::vector<uint8_t> read_file(const std::filesystem::path &p) {
	std::ifstream f(p, std::ios::binary);
	return {std::istreambuf_iterator<char>(f), {}};
}

static void test_squeezed() {
	de::Contents c({0x05, 0xFE, 0x90, 0x01, 0xFF, 0x78, 0x56, 0x34, 0x12});
	CHECK(c.squeezed() == 5);
	CHECK(c.squeezed() == 0x190);
	CHECK(c.squeezed() == 0x12345678);
	CHECK(c.remaining() == 0);
}

static void test_md5() {
	const char *s = "abc";
	auto d = de::md5({reinterpret_cast<const uint8_t *>(s), 3});
	const uint8_t want[16] = {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
	                          0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};
	CHECK(memcmp(d.data(), want, 16) == 0);
}

static void test_creature(const std::filesystem::path &dir) {
	de::Contents c(read_file(dir / "Creature.DEObjects"));
	CHECK(c.string() == "Diversions Entertainment Objects File2");
	CHECK(c.u8() == 2);  // format version (LoadHeader, Core 0x70051bb0)
	CHECK(c.u32() == 0);  // read by master object vtable +0xAC when version > 1
	auto header = c.block();
	CHECK(header.size() == 0xa1);
	CHECK(header[0] == 9);  // DE_CTransferAgent::ProcessHeader version
	// The Health behavior follows directly: a method-7 block (u32 len 0x163).
	CHECK(c.pos() == 0xd5);
	auto packed = c.block();
	CHECK(packed.size() == 0x163);
	auto out = de::Contents::decompress(packed);  // throws on MD5 mismatch
	CHECK(out.size() == 400);
	std::string text(out.begin(), out.end());
	CHECK(text.find("float m_Health;") != std::string::npos);
	CHECK(text.find("HealthChanged") != std::string::npos);
}

static void test_creature_toc(const std::filesystem::path &dir) {
	de::ObjectsFile f(read_file(dir / "Creature.DEObjects"));
	CHECK(f.format() == 2);
	CHECK(f.master().name == "Creature");
	CHECK(f.master().class_path == "TransferAgent\\Win\\File");
	CHECK(f.objects().size() == 1);
	auto &o = f.objects()[0];
	CHECK(o.name == "BHealth");
	CHECK(o.class_path == "Behavior");
	CHECK(o.guid.str() == "22ebd776-e7a7-11d3-bf5c-004033de174c");
	CHECK(o.data_offset == 0xd5);
	CHECK(o.flags & de::ObjectsFile::kFlagCompressed);
	auto d = f.object_data(o);
	CHECK(d.contents.size() == 400);
	CHECK(d.references.size() == 11);
	CHECK(d.hierarchy.size() == 3);
}

// SaveToFile's layout reproduces Creature.DEObjects byte for byte, except the never-initialised
// fields (0xcd fill) of the agent's own entry and +0x84 (meaning unknown, 0 in ours).
static void test_write_creature(const std::filesystem::path &dir) {
	auto orig = read_file(dir / "Creature.DEObjects");
	de::ObjectsFile f(orig);
	auto &o = f.objects()[0];
	de::Contents raw(orig);
	raw.seek(o.data_offset);
	de::NewObject n{o.name, o.class_path, o.guid, raw.block(), raw.block(), raw.block(), o.flags};
	auto out = de::write_objects_file(f.master().name, f.master().guid, {n});
	CHECK(out.size() == orig.size());
	std::vector<size_t> diff;
	for (size_t i = 0; i < std::min(out.size(), orig.size()); ++i) {
		if (out[i] != orig[i]) diff.push_back(i);
	}
	for (size_t i : diff) {
		bool garbage = (i >= 0x71 && i < 0x75) || (i >= 0x7a && i < 0x7e) || (i >= 0xc8 && i < 0xcc);
		if (!garbage) fprintf(stderr, "write: byte %zx is %02x, want %02x\n", i, out[i], orig[i]);
		CHECK(garbage);
	}
	de::ObjectsFile back(out);
	CHECK(back.objects().size() == 1 && back.objects()[0].data_offset == o.data_offset);
	CHECK(back.object_data(back.objects()[0]).contents == f.object_data(o).contents);
}

static void test_vmc2_health(const std::filesystem::path &dir) {
	de::ObjectsFile f(read_file(dir / "Fighter.DEObjects"));
	for (auto &o : f.objects()) {
		if (o.name != "PFighterStats") {
			continue;
		}
		auto d = f.object_data(o);
		auto refs = vm::class_behavior_refs(d.references);
		CHECK(refs.size() == 1);
		CHECK(refs[0].kind == de::Reference::kLocal && refs[0].index == 5);  // BFighterStats
		auto v = vm::VMClass::parse(f.ex_payload(d, "VMC2")->data, refs.size());
		CHECK(v.classes.size() == 11);
		CHECK(v.intrinsics.size() == 27);
		auto &m = *v.behaviors[0].methods[0];
		CHECK(m.signature == "BFighterStats:float Health()");
		auto ins = vm::decode(m.code);
		CHECK(ins.size() == 4);
		CHECK(ins[0].opcode == 1 && ins[1].opcode == 66 && ins[2].opcode == 5 && ins[3].opcode == 319);
		CHECK(ins[1].args == std::vector<int64_t>({4, 0, 4}));  // member m_Health @0 -> sp+4
		return;
	}
	CHECK(!"PFighterStats not found");
}

// Run shipped BFighterStats bytecode natively and compare with the original source's math.
struct NoIntrinsics : vm::Host {
	void intrinsic(vm::Executor &, uint32_t index, uint32_t) override {
		throw vm::VMError("unexpected intrinsic " + std::to_string(index));
	}
	void message(vm::Executor &, const std::string &) override {}
};

static void test_vm_fighter_stats(const std::filesystem::path &dir) {
	de::ObjectsFile f(read_file(dir / "Fighter.DEObjects"));
	const de::ObjectInfo *info = nullptr;
	for (auto &o : f.objects()) {
		if (o.name == "PFighterStats") {
			info = &o;
		}
	}
	auto d = f.object_data(*info);
	vm::Memory mem;
	vm::ScriptClass cls;
	vm::load_class(cls, mem, d.contents,
	               vm::VMClass::parse(f.ex_payload(d, "VMC2")->data, vm::class_behavior_refs(d.references).size()));
	CHECK(cls.name == "PFighterStats");
	CHECK(cls.base_path == "Actor\\Creature");

	vm::Instance inst;
	inst.cls = &cls;
	inst.members_size = 256;
	inst.members = mem.alloc(inst.members_size);
	// Member layout from BFighterStats "Members" (bool = 4 bytes, finals take no storage).
	mem.set<float>(inst.members + 0, 75.0f);    // m_Health
	mem.set<float>(inst.members + 4, 100.0f);   // m_MaxHealth
	mem.set<float>(inst.members + 16, 50.0f);   // m_StunResistance
	mem.set<float>(inst.members + 20, 100.0f);  // m_MaxStunResistance
	mem.set<float>(inst.members + 24, -10.0f);  // m_StaminaLoss
	mem.set<float>(inst.members + 36, 2.0f);    // m_StaminaBuildRate
	NoIntrinsics host;
	vm::Executor ex(mem, host, inst);

	auto call_float = [&](const char *sig) {
		uint8_t args[4] = {};
		ex.call(*cls.find(sig), args);
		float r;
		memcpy(&r, args, 4);
		return r;
	};
	CHECK(call_float("BFighterStats:float Health()") == 75.0f);
	CHECK(call_float("BFighterStats:float MaxHealth()") == 100.0f);
	CHECK(call_float("BFighterStats:float NormalizedHealth()") == 0.75f);
	CHECK(std::fabs(call_float("BFighterStats:float NormalizedStamina()") - 0.4f) < 1e-6f);

	auto lose = [&](float amount) {
		uint8_t args[4];
		memcpy(args, &amount, 4);
		ex.call(*cls.find("BFighterStats:FighterLoseStamina(float)"), args);
		return mem.get<float>(inst.members + 24);
	};
	CHECK(std::fabs(lose(10.0f) - -11.5f) < 1e-5f);  // 10 * .15
	CHECK(std::fabs(lose(-10.0f) - -8.5f) < 1e-5f);  // -10 * .15 * 2
	CHECK(!ex.running());
}

// Every object in every shipped file: header parses and contents decode (MD5-checked).
static void test_all_files(const std::filesystem::path &dir) {
	size_t files = 0, objects = 0;
	bool v2123 = false;  // 2.123's Effects carry the get-hit debris (PATCHES.md)
	for (auto &e : std::filesystem::directory_iterator(dir)) {
		if (e.path().extension() != ".DEObjects") {
			continue;
		}
		try {
			de::ObjectsFile f(read_file(e.path()));
			for (auto &o : f.objects()) {
				f.object_data(o);
				++objects;
				v2123 |= o.name == "GetHitBitsParent";
			}
			++files;
		} catch (const std::exception &ex) {
			fprintf(stderr, "%s: %s\n", e.path().filename().c_str(), ex.what());
			++failures;
		}
	}
	printf("all files: %zu files, %zu objects\n", files, objects);

	// Every compiled method of every Class decodes to its exact end.
	size_t classes = 0, methods = 0, instructions = 0;
	for (auto &e : std::filesystem::directory_iterator(dir)) {
		if (e.path().extension() != ".DEObjects") {
			continue;
		}
		de::ObjectsFile f(read_file(e.path()));
		for (auto &o : f.objects()) {
			if (o.class_path != "Class") {
				continue;
			}
			auto d = f.object_data(o);
			auto code = f.ex_payload(d, "VMC2");
			if (!code) {
				continue;
			}
			try {
				auto v = vm::VMClass::parse(code->data, vm::class_behavior_refs(d.references).size());
				for (auto &b : v.behaviors) {
					for (auto &m : b.methods) {
						if (m) {
							instructions += vm::decode(m->code).size();
							++methods;
						}
					}
				}
				++classes;
			} catch (const std::exception &ex) {
				fprintf(stderr, "%s/%s: %s\n", e.path().filename().c_str(), o.name.c_str(), ex.what());
				++failures;
			}
		}
	}
	printf("vm: %zu classes, %zu methods, %zu instructions\n", classes, methods, instructions);

	// Every Image\Standard decodes at every stored level (EX keys {0, level}).
	size_t images = 0, levels = 0, procedural = 0, pixels = 0, obsolete = 0, empty = 0;
	for (auto &e : std::filesystem::directory_iterator(dir)) {
		if (e.path().extension() != ".DEObjects") {
			continue;
		}
		de::ObjectsFile f(read_file(e.path()));
		for (auto &o : f.objects()) {
			if (o.class_path != "Image\\Standard") {
				continue;
			}
			try {
				auto d = f.object_data(o);
				bool any = false;
				for (auto &x : d.ex) {
					auto k = x.key();
					if (k.size() == 2 && k[0] == 0) {
						de::Image img;
						try {
							img = de::read_image(f, o, uint8_t(k[1]));
						} catch (const de::ObsoleteCompression &) {
							++obsolete;
							continue;
						}
						if (img.channels.empty()) {
							++empty;  // stored but channel-less (e.g. OMFBGGUI "Bump": only the terminator)
						}
						pixels += size_t(img.width) * img.height;
						++levels;
						any = true;
					}
				}
				any ? ++images : ++procedural;
			} catch (const std::exception &ex) {
				fprintf(stderr, "%s/%s: %s\n", e.path().filename().c_str(), o.name.c_str(), ex.what());
				++failures;
			}
		}
	}
	printf("images: %zu with stored data (%zu levels, %.1f Mpx, %zu empty), %zu without, %zu obsolete DZip levels\n",
	       images, levels, pixels / 1e6, empty, procedural, obsolete);
	CHECK(empty <= 6);

	// Every Mesh\Standard's geometry decodes exactly.
	size_t meshes = 0, verts = 0, tris = 0, no_geometry = 0;
	for (auto &e : std::filesystem::directory_iterator(dir)) {
		if (e.path().extension() != ".DEObjects") {
			continue;
		}
		de::ObjectsFile f(read_file(e.path()));
		for (auto &o : f.objects()) {
			if (o.class_path != "Mesh\\Standard") {
				continue;
			}
			try {
				auto m = de::read_mesh(f, o);
				if (m.faces.empty()) {
					++no_geometry;
				}
				verts += m.vertices.size();
				tris += m.faces.size();
				++meshes;
			} catch (const std::exception &ex) {
				fprintf(stderr, "%s/%s: %s\n", e.path().filename().c_str(), o.name.c_str(), ex.what());
				++failures;
			}
		}
	}
	printf("meshes: %zu (%zu vertices, %zu triangles, %zu without geometry)\n", meshes, verts, tris, no_geometry);
	CHECK(meshes == 966);
	CHECK(obsolete == (v2123 ? 36u : 38u));  // on 20 images in 2.090; their index-0 pixels still decode
	CHECK(classes == (v2123 ? 255u : 249u));
	CHECK(files == 65);
	CHECK(objects == (v2123 ? 28337u : 28115u));
}

// Every Image\\AVI in the data: DIB, Cinepak and Indeo 4/5 videos decode to one frame per stream
// frame, of the video's size, and are not blank. DE_TESTS_AVI_DUMP=<dir> writes each frame 0 as a PPM.
static void test_avi(const std::filesystem::path &dir) {
	int decoded = 0, skipped = 0, empty = 0;
	const char *dump = getenv("DE_TESTS_AVI_DUMP");
	for (auto &e : std::filesystem::directory_iterator(dir)) {
		if (e.path().extension() != ".DEObjects") continue;
		de::ObjectsFile f(read_file(e.path()));
		for (auto &o : f.objects()) {
			if (o.class_path != "Image\\AVI") continue;
			auto v = de::decode_avi(de::avi_of_image(f.object_data(o).contents));
			if (v.codec.empty()) {  // an image with no video in it (Jaguar's)
				++empty;
				continue;
			}
			std::string c = v.codec;
			for (char &ch : c) ch = char(toupper(uint8_t(ch)));
			if (c != "DIB " && c != "CVID" && c != "IV41" && c != "IV50") {
				printf("avi: %s/%s has codec \"%s\"\n", e.path().stem().string().c_str(), o.name.c_str(), v.codec.c_str());
				++skipped;
				continue;
			}
			++decoded;
			CHECK(v.decoded());
			CHECK(v.width > 0 && v.height > 0);
			CHECK(v.frames.size() >= v.length);
			if (!v.decoded()) continue;
			auto img = de::read_image(f, o);
			CHECK(img.width == v.width && img.channels.size() == 3);
			int lit = 0;  // some videos start black (Pickups fades in)
			for (auto &fr : v.frames) {
				for (uint8_t b : fr) lit += b > 8;
			}
			CHECK(lit > 0);
			if (dump) {
				std::ofstream out(std::filesystem::path(dump) / (e.path().stem().string() + "_" + o.name + ".ppm"), std::ios::binary);
				out << "P6\n" << v.width << " " << v.height << "\n255\n";
				out.write(reinterpret_cast<const char *>(v.frames[0].data()), std::streamsize(v.frames[0].size()));
			}
		}
	}
	printf("avi: %d decoded, %d without video, %d skipped (unknown codec)\n", decoded, empty, skipped);
	CHECK(decoded > 0);
	CHECK(skipped == 0);
}

int main(int argc, char **argv) {
	test_squeezed();
	test_md5();
	if (argc > 1) {
		try {
			test_creature(argv[1]);
			test_creature_toc(argv[1]);
			test_write_creature(argv[1]);
			test_vmc2_health(argv[1]);
			test_vm_fighter_stats(argv[1]);
			test_avi(argv[1]);
			if (argc > 2 && !strcmp(argv[2], "--all")) {
				test_all_files(argv[1]);
			}
		} catch (const std::exception &e) {
			fprintf(stderr, "creature: %s\n", e.what());
			++failures;
		}
	}
	printf("%s (%d failures)\n", failures ? "FAIL" : "OK", failures);
	return failures != 0;
}
