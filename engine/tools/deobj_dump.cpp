// deobj-dump: inspect original .DEObjects files.
//
//   deobj-dump header <file>            print the container header
//   deobj-dump toc[+] <file>             list the header's object table (+ decode each object's contents)
//   deobj-dump scripts <file> <outdir>  write every Behavior (DE_CPCode) as readable source
//   deobj-dump ex <file> <object> <key> <out>  write an EX payload (e.g. key VMC2 = compiled VMCode)
//   deobj-dump block <file> <object> contents|references|hierarchy <out>
//   deobj-dump disasm <file> <class>   disassemble a Class's compiled VMCode
//   deobj-dump image <file> <object> <out.png> [level]  export a texture
//   deobj-dump tree <file>             print the object hierarchy
//   deobj-dump scene <Data dir> <stem>   list an environment's draw items
//   deobj-dump blocks <file> [outdir]   find every MD5-verified compressed block, optionally write them out
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>

#include "de/font.h"
#include "de/particles.h"
#include "de/avi.h"
#include "de/contents.h"
#include <zlib.h>

#include "de/image.h"
#include "de/lighting.h"
#include "de/md5.h"
#include "de/mesh.h"
#include "de/objects_file.h"
#include "de/pcode.h"
#include "de/scene.h"
#include "vm/disasm.h"
#include "vm/vmcode.h"

namespace fs = std::filesystem;

static std::vector<uint8_t> read_file(const char *path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		throw std::runtime_error(std::string("cannot open ") + path);
	}
	return {std::istreambuf_iterator<char>(f), {}};
}

// Object selector: "#<index>" or a name (first match).
static bool matches(const de::ObjectsFile &f, const de::ObjectInfo &o, const char *spec) {
	if (spec[0] == '#') {
		return &o == &f.objects()[strtoul(spec + 1, nullptr, 10)];
	}
	return o.name == spec;
}

// Walk an object's contents as consecutive versioned chunks (one per class in its hierarchy,
// base first) and print each chunk's version, size and leading floats.
static int cmd_chunks(const char *path, const char *spec) {
	de::ObjectsFile f(read_file(path));
	for (auto &o : f.objects()) {
		if (!matches(f, o, spec)) {
			continue;
		}
		de::Contents c(f.object_data(o).contents);
		printf("%s (%s): %zu bytes\n", o.name.c_str(), o.class_path.c_str(), c.size());
		while (c.remaining() > 1) {
			size_t start = c.pos();
			de::Chunk ch = de::begin_chunk(c);
			size_t body = c.pos();
			if (ch.end > c.size() || ch.end < body) {
				printf("  @%zu: not a chunk\n", start);
				break;
			}
			printf("  @%zu v%u %zu bytes:", start, ch.version, ch.end - body);
			for (size_t k = body; k + 4 <= ch.end && k < body + 4 * 48; k += 4) {
				float v;
				memcpy(&v, c.data().data() + k, 4);
				printf(" %g", v);
			}
			printf("\n");
			if (getenv("DEOBJ_HEX")) {
				printf("    hex ");
				for (size_t k = body; k < ch.end; ++k) {
					printf("%02x", c.data()[k]);
				}
				printf("\n");
			}
			c.seek(ch.end);
		}
		if (getenv("DEOBJ_HEX")) {
			auto od = f.object_data(o);
			auto &refs = od.references;
			printf("  references %zu bytes: ", refs.size());
			for (uint8_t b : refs) printf("%02x", b);
			printf("\n");
			for (size_t g = 0; g < f.guid_pool().size() && g < 32; ++g) printf("  guid[%zu] %s\n", g, f.guid_pool()[g].str().c_str());
		}
		return 0;
	}
	return 1;
}

static int cmd_header(const char *path) {
	de::Contents c(read_file(path));
	printf("signature: %s\n", c.string().c_str());
	// DE_CTransferAgentWinFile::LoadHeader: "...File2" files carry a u8 format version; for
	// version > 1 the master object reads a further field (vtable +0xAC, 4 bytes in shipped data).
	unsigned version = c.u8();
	printf("format version: %u\n", version);
	if (version > 1) {
		printf("master pre-header: 0x%08x (meaning unknown)\n", c.u32());
	}
	auto header = c.block();
	printf("header block: %zu bytes, version %u\n", header.size(), header.empty() ? 0 : header[0]);
	printf("body: %zu bytes\n", c.remaining());
	return 0;
}

static int cmd_blocks(const char *path, const char *outdir) {
	auto data = read_file(path);
	if (outdir) {
		fs::create_directories(outdir);
	}
	size_t found = 0, bytes_out = 0;
	// A method-7 block inside an Extract(DE_CContents&) field: u32 len, 0x07, md5[16], squeezed size, LZRW3-A.
	for (size_t i = 4; i + 22 < data.size(); ++i) {
		if (data[i] != 7) {
			continue;
		}
		uint32_t len = data[i - 4] | (data[i - 3] << 8) | (data[i - 2] << 16) | (uint32_t(data[i - 1]) << 24);
		if (len < 22 || i + len > data.size()) {
			continue;
		}
		try {
			auto out = de::Contents::decompress({data.data() + i, len});
			if (outdir) {
				char name[64];
				snprintf(name, sizeof name, "%08zx.bin", i - 4);
				std::ofstream(fs::path(outdir) / name, std::ios::binary)
					.write(reinterpret_cast<const char *>(out.data()), out.size());
			}
			printf("%08zx packed=%u unpacked=%zu\n", i - 4, len, out.size());
			++found;
			bytes_out += out.size();
			i += len - 1;
		} catch (const de::FormatError &) {
		}
	}
	fprintf(stderr, "%zu blocks, %zu bytes unpacked\n", found, bytes_out);
	return 0;
}

static int cmd_toc(const char *path, bool contents) {
	de::ObjectsFile f(read_file(path));
	auto print = [](const de::ObjectInfo &o) {
		printf("%s\t%s\t%s\toff=%08x\tflag=%d\tv84=%08x\tflags=%x\n", o.name.c_str(), o.class_path.c_str(),
		       o.guid.str().c_str(), o.data_offset, o.flag, o.value84, o.flags);
	};
	printf("format %u, master: ", f.format());
	print(f.master());
	for (auto &o : f.objects()) {
		print(o);
		if (contents) {
			auto d = f.object_data(o);
			size_t ex = 0;
			for (auto &e : d.ex) {
				ex += e.header.size();
			}
			printf("    contents %zu, references %zu, hierarchy %zu, ex %zu entries (%zu header bytes)\n",
			       d.contents.size(), d.references.size(), d.hierarchy.size(), d.ex.size(), ex);
			for (auto &e : d.ex) {
				printf("    ex \"");
				for (auto b : e.header) {
					putchar(b >= 32 && b < 127 ? b : '.');
				}
				printf("\" [");
				for (auto b : e.header) {
					printf("%02x", b);
				}
				printf("] value=%u (0x%x)\n", e.value, e.value);
			}
		}
	}
	fprintf(stderr, "%zu objects\n", f.objects().size());
	return 0;
}

static int cmd_scripts(const char *path, const char *outdir) {
	de::ObjectsFile f(read_file(path));
	fs::create_directories(outdir);
	size_t n = 0;
	for (auto &o : f.objects()) {
		if (o.class_path != "Behavior") {
			continue;
		}
		de::Contents c(f.object_data(o).contents);
		auto p = de::PCode::parse(c);
		std::string file = o.name;
		for (auto &ch : file) {
			if (ch == '/' || ch == '\\' || ch == ':') {
				ch = '_';
			}
		}
		auto out = fs::path(outdir) / (file + ".dec");
		for (int k = 2; fs::exists(out); ++k) {
			out = fs::path(outdir) / (file + "~" + std::to_string(k) + ".dec");
		}
		std::ofstream(out) << p.text();
		++n;
	}
	fprintf(stderr, "%zu behaviors\n", n);
	return 0;
}

// Write one EX payload of a named object: ex <file> <object> <key> <out>
static int cmd_ex(const char *path, const char *object, const char *key, const char *out) {
	de::ObjectsFile f(read_file(path));
	for (auto &o : f.objects()) {
		if (!matches(f, o, object)) {
			continue;
		}
		auto p = f.ex_payload(f.object_data(o), key);
		if (!p) {
			fprintf(stderr, "%s has no EX entry %s\n", object, key);
			return 1;
		}
		std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char *>(p->data.data()), p->data.size());
		printf("flag %u, %zu bytes\n", p->flag, p->data.size());
		return 0;
	}
	fprintf(stderr, "no object %s\n", object);
	return 1;
}

// Write one of an object's blocks: block <file> <object> contents|references|hierarchy <out>
static int cmd_block(const char *path, const char *object, const char *which, const char *out) {
	de::ObjectsFile f(read_file(path));
	for (auto &o : f.objects()) {
		if (!matches(f, o, object)) {
			continue;
		}
		auto d = f.object_data(o);
		auto &b = !strcmp(which, "references") ? d.references : !strcmp(which, "hierarchy") ? d.hierarchy : d.contents;
		std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char *>(b.data()), b.size());
		return 0;
	}
	fprintf(stderr, "no object %s\n", object);
	return 1;
}

// Disassemble a Class object's compiled code: disasm <file> <class>
static int cmd_disasm(const char *path, const char *object) {
	de::ObjectsFile f(read_file(path));
	for (auto &o : f.objects()) {
		if (!matches(f, o, object)) {
			continue;
		}
		auto d = f.object_data(o);
		auto code = f.ex_payload(d, "VMC2");
		if (!code) {
			fprintf(stderr, "%s has no VMC2\n", object);
			return 1;
		}
		auto v = vm::VMClass::parse(code->data, vm::class_behavior_refs(d.references).size());
		printf("classes:");
		for (auto &k : v.classes) {
			printf(" %s", k.name.c_str());
		}
		printf("\nglobal refs:\n");
		for (size_t i = 0; i < v.global_refs.size(); ++i) {
			printf("  [%zu] kind %u %s\n", i, v.global_refs[i].kind, v.global_refs[i].name.c_str());
		}
		printf("member interfaces: %zu, finals: %zu%s%s, tail %zu bytes\n", v.member_interfaces.size(), v.finals.size(),
		       v.unknown_final_class.empty() ? "" : " (stopped at unknown class ", v.unknown_final_class.empty() ? "" : (v.unknown_final_class + ")").c_str(),
		       v.tail.size());
		if (getenv("DEOBJ_FINALS")) {
			for (auto &f : v.finals) {
				printf("  final %s:", v.classes[f.class_index].name.c_str());
				for (uint8_t b : f.data) printf(" %02x", b);
				printf("\n");
			}
			printf("  tail:");
			for (uint8_t b : v.tail) printf(" %02x", b);
			printf("\n");
		}
		printf("intrinsics:\n");
		for (size_t i = 0; i < v.intrinsics.size(); ++i) {
			printf("  [%zu] %s::%s\n", i, v.classes[v.intrinsics[i].class_index].name.c_str(),
			       v.intrinsics[i].signature.c_str());
		}
		for (auto &b : v.behaviors) {
			for (auto &m : b.methods) {
				if (!m) {
					continue;
				}
				printf("\n%s  (counts %u %u %u %u %u) v78 %d bases", m->signature.c_str(), m->counts[0], m->counts[1],
				       m->counts[2], m->counts[3], m->counts[4], m->v78);
				for (uint32_t b : m->base_methods) printf(" %u", b);
				printf("\n");
				for (auto &ins : vm::decode(m->code)) {
					printf("  %s\n", vm::format(ins).c_str());
				}
			}
		}
		return 0;
	}
	fprintf(stderr, "no object %s\n", object);
	return 1;
}

static void write_png(const char *path, int w, int h, const std::vector<uint8_t> &rgba) {
	std::vector<uint8_t> raw;
	raw.reserve(size_t(h) * (size_t(w) * 4 + 1));
	for (int y = 0; y < h; ++y) {
		raw.push_back(0);
		raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
	}
	uLongf zn = compressBound(uLong(raw.size()));
	std::vector<uint8_t> z(zn);
	compress2(z.data(), &zn, raw.data(), uLong(raw.size()), 6);
	z.resize(zn);
	std::ofstream f(path, std::ios::binary);
	auto be32 = [&](uint32_t v) {
		uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
		f.write(reinterpret_cast<char *>(b), 4);
	};
	auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
		be32(uint32_t(data.size()));
		f.write(type, 4);
		f.write(reinterpret_cast<const char *>(data.data()), data.size());
		uLong crc = crc32(0, reinterpret_cast<const Bytef *>(type), 4);
		crc = crc32(crc, data.data(), uInt(data.size()));
		be32(uint32_t(crc));
	};
	f.write("\x89PNG\r\n\x1a\n", 8);
	std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
	                             uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 6, 0, 0, 0};
	chunk("IHDR", ihdr);
	chunk("IDAT", z);
	chunk("IEND", {});
}

// Export an Image\Standard object as PNG: image <file> <object> <out.png> [level]
static int cmd_image(const char *path, const char *object, const char *out, int level) {
	// A Data directory plus "Stem:Object" reads from the World (engine-module resources too).
	std::unique_ptr<de::World> world;
	std::unique_ptr<de::ObjectsFile> owned;
	const de::ObjectsFile *fp = nullptr;
	if (std::filesystem::is_directory(path) && strchr(object, ':')) {
		world = std::make_unique<de::World>(path);
		std::string stem(object, strchr(object, ':'));
		fp = world->file(stem);
		object = strchr(object, ':') + 1;
		if (!fp) {
			fprintf(stderr, "no file %s\n", stem.c_str());
			return 1;
		}
	} else {
		owned = std::make_unique<de::ObjectsFile>(read_file(path));
		fp = owned.get();
	}
	const de::ObjectsFile &f = *fp;
	for (auto &o : f.objects()) {
		if (!matches(f, o, object) || o.class_path != "Image\\Standard") {
			continue;
		}
		auto img = de::read_image(f, o, uint8_t(level));
		if (img.channels.empty()) {
			continue;  // names repeat; take the first one with stored pixels
		}
		write_png(out, img.width, img.height, img.rgba());
		printf("%s: %dx%d, %zu channels\n", out, img.width, img.height, img.channels.size());
		return 0;
	}
	fprintf(stderr, "no image %s with stored pixels\n", object);
	return 1;
}

// Print the object tree: tree <file>
static int cmd_tree(const char *path) {
	de::ObjectsFile f(read_file(path));
	auto &objs = f.objects();
	auto parents = f.parents();
	std::function<void(size_t, int)> show = [&](size_t i, int depth) {
		printf("%*s%s  [%s] #%zu\n", depth * 2, "", objs[i].name.c_str(), objs[i].class_path.c_str(), i);
		for (uint32_t c : de::ObjectsFile::children(f.object_data(objs[i]))) {
			if (c < objs.size()) {
				show(c, depth + 1);
			}
		}
	};
	for (size_t i = 0; i < objs.size(); ++i) {
		if (parents[i] < 0) {
			show(i, 0);
		}
	}
	return 0;
}

// List what an environment draws: scene <Data dir> <file stem>
// Every reference list of the matching objects, resolved across the data directory: chunk.list
// followed by "name [class] (file)" per entry.
static int cmd_refs(const char *dir, const char *stem, const char *spec) {
	de::World w(dir);
	auto *f = w.file(stem);
	if (!f) {
		fprintf(stderr, "no file %s\n", stem);
		return 1;
	}
	for (uint32_t i = 0; i < f->objects().size(); ++i) {
		auto &o = f->objects()[i];
		if (!matches(*f, o, spec)) {
			continue;
		}
		printf("%s [%s] #%u\n", o.name.c_str(), o.class_path.c_str(), i);
		auto d = f->object_data(o);
		de::Contents c(d.references);
		int chunk = 0;
		while (c.remaining() > 1) {
			de::Chunk ch = de::begin_chunk(c);
			if (ch.end > c.size()) break;
			int list = 0;
			while (c.pos() < ch.end) {
				printf("  %d.%d (v%u):", chunk, list, ch.version);
				while (c.pos() < ch.end && c.boolean()) {
					auto r = de::Reference::read(c);
					auto t = w.resolve(*f, r);
					if (t) {
						printf(" %s [%s]", t.info().name.c_str(), t.info().class_path.c_str());
					} else {
						printf(" <%s %u>", r.kind == de::Reference::kNull ? "null" : "unresolved", r.index);
					}
				}
				printf("\n");
				++list;
			}
			c.seek(ch.end);
			++chunk;
		}
	}
	return 0;
}

// Every particle renderer in the data directory (or one file): renderer modes, generator and
// modifiers, one line each, for surveying which features the data uses.
// stages <Data dir> [file stem]: a survey of TextureProcess\MultiStage stage modes and colours.
static int cmd_stages(const char *dir, const char *only) {
	de::World w(dir);
	std::map<std::string, int> combos, versions;
	int total = 0, coloured = 0, colour_refs = 0, f2c = 0, f30 = 0, disabled = 0;
	for (auto &entry : std::filesystem::directory_iterator(dir)) {
		if (entry.path().extension() != ".DEObjects") continue;
		std::string stem = entry.path().stem().string();
		if (only && stem != only) continue;
		auto *f = w.file(stem);
		if (!f) continue;
		for (uint32_t i = 0; i < f->objects().size(); ++i) {
			const auto &o = f->objects()[i];
			if (o.class_path != "TextureProcess\\MultiStage") continue;
			auto ms = de::read_multistage(f->object_data(o));
			++total;
			versions["v" + std::to_string(ms.version)]++;
			std::string combo;
			for (auto &st : ms.stages) {
				de::ObjectRef m = w.resolve(*f, st.manager);
				std::string kind = m ? (m.info().class_path == "TextureManager\\Reflection" ? "R" : "T") : "?";
				combo += kind + std::to_string(st.mode) + (st.flag_2c ? "*" : "") + " ";
				if (st.colour_value) ++colour_refs;
				else if (st.colour != std::array<float, 4>{1, 1, 1, 1}) ++coloured;
				f2c += st.flag_2c, f30 += st.flag_30, disabled += st.disabled;
				static const bool all = getenv("DEOBJ_STAGES_ALL") != nullptr;
				if (only && (all || st.colour_value || st.colour != std::array<float, 4>{1, 1, 1, 1}))
					printf("%s/%s stage %s mode %d%s colour %.2f %.2f %.2f %.2f%s\n", stem.c_str(), o.name.c_str(), m ? m.info().name.c_str() : "-", st.mode,
					       st.flag_2c ? " stacked" : "", st.colour[0], st.colour[1], st.colour[2], st.colour[3], st.colour_value ? " (value)" : "");
			}
			combos[combo]++;
		}
	}
	printf("%d multistage processes;", total);
	for (auto &[v, n] : versions) printf(" %s %d", v.c_str(), n);
	printf("\nstages: %d non-white colours, %d colour values, %d flag +0x2c, %d flag +0x30, %d disabled\n", coloured, colour_refs, f2c, f30, disabled);
	std::vector<std::pair<int, std::string>> sorted;
	for (auto &[c, n] : combos) sorted.push_back({n, c});
	std::sort(sorted.rbegin(), sorted.rend());
	for (auto &[n, c] : sorted) printf("%6d  %s\n", n, c.c_str());
	return 0;
}

static int cmd_particles(const char *dir, const char *only) {
	de::World w(dir);
	for (auto &entry : std::filesystem::directory_iterator(dir)) {
		if (entry.path().extension() != ".DEObjects") continue;
		std::string stem = entry.path().stem().string();
		if (only && stem != only) continue;
		auto *f = w.file(stem);
		if (!f) continue;
		for (uint32_t i = 0; i < f->objects().size(); ++i) {
			de::ObjectRef r{f, i};
			if (!de::is_particle_renderer(r)) continue;
			auto p = de::read_particle_renderer(w, r);
			printf("%s/%s [%s] blend %d mode %d uv %d color %d size %gx%g cone %d norm %d off %g,%g,%g rgba %.2f,%.2f,%.2f,%.2f%s image %s\n",
			       stem.c_str(), r.info().name.c_str(), p.kind.c_str(), p.blend, p.mode, p.uv_mode, p.color_mode, p.width,
			       p.height, p.cone_mode, p.normalize, p.offset[0], p.offset[1], p.offset[2], p.color.rgba[0], p.color.rgba[1],
			       p.color.rgba[2], p.color.rgba[3], p.color.color ? " (modulated)" : "",
			       p.image ? p.image.info().name.c_str() : "-");
			if (p.kind == "FourFaceTriangle")
				printf("    triangle %g,%g,%g %g,%g,%g %g,%g,%g jitter %g,%g,%g,%g,%g,%g,%g,%g,%g b4 %g,%g,%g,%g facing %d uv plane %d\n", p.tri_corners[0],
				       p.tri_corners[1], p.tri_corners[2], p.tri_corners[3], p.tri_corners[4], p.tri_corners[5], p.tri_corners[6], p.tri_corners[7],
				       p.tri_corners[8], p.tri_jitter[0], p.tri_jitter[1], p.tri_jitter[2], p.tri_jitter[3], p.tri_jitter[4], p.tri_jitter[5],
				       p.tri_jitter[6], p.tri_jitter[7], p.tri_jitter[8], p.tri_b4[0], p.tri_b4[1], p.tri_b4[2], p.tri_b4[3], p.tri_facing, p.tri_uv_plane);
			if (p.kind == "StreamParticleRenderer")
				printf("    stream texture %d colour %d width %g uv x%g +%g/ms breaks %d gap %g\n", p.stream_texture_mode, p.stream_color_mode,
				       p.stream_width, p.stream_uv_scale, p.stream_uv_scroll, p.stream_breaks, p.stream_max_gap);
			auto &g = p.generator;
			printf("    generator %s local %d channels %d%d%d%d%d%d reset %d\n", g.object ? g.object.info().name.c_str() : "-",
			       g.local, g.channels[0], g.channels[1], g.channels[2], g.channels[3], g.channels[4], g.channels[5],
			       g.reset_per_instance);
			for (auto &m : g.modifiers) {
				printf("    %-30s %-28s t %g~%g life %g~%g size %g~%g spawn %d loop %d init %d interp %d extra %d", m.kind.c_str(),
				       m.object.info().name.c_str(), m.spawn_time, m.spawn_time_variation, m.life_span, m.life_span_variation,
				       m.spawn_size, m.spawn_size_variation, m.spawning, m.looping, m.initial_count, m.interpolate,
				       m.extra_per_spawn);
				if (m.kind == "NewParticleModifier") {
					printf(" | rot %d orient %d momentum %d x%g color %d inherit %d cap %g x%g add %d", m.rotation_mode,
					       m.orientation_mode, m.momentum_mode, m.momentum_scale, m.color_mode, m.inherit_momentum,
					       m.inherit_cap, m.inherit_scale, m.add_momentum);
				} else if (m.kind == "MomentumAndAcceleration") {
					printf(" | inherit %g,%g,%g random %g,%g,%g initial %g,%g,%g accel %d %g angle %g", m.inherit_axes[0],
					       m.inherit_axes[1], m.inherit_axes[2], m.random_momentum[0], m.random_momentum[1],
					       m.random_momentum[2], m.initial_momentum[0], m.initial_momentum[1], m.initial_momentum[2],
					       m.accel_mode, m.accel, m.accel_angle);
				} else if (m.kind == "Gravity" || m.kind == "Wind") {
					printf(" | strength %g", m.strength);
				} else if (m.kind == "MiscellaneousParticleFunctions") {
					printf(" | mode %u curve %s %g..%g", m.misc_mode, m.curve.inter ? m.curve.shape.type.c_str() : "const",
					       m.curve.inter ? m.curve.min : m.curve.value, m.curve.max);
				} else if (m.kind.find("Box") != std::string::npos || m.kind.find("Ellipsoid") != std::string::npos) {
					printf(" | box %g,%g,%g..%g,%g,%g wrap %d%d%d", m.box[0], m.box[1], m.box[2], m.box[3], m.box[4], m.box[5],
					       m.wrap[0], m.wrap[1], m.wrap[2]);
				} else if (m.kind == "MeshSpawner") {
					printf(" | burst %d mesh %s", m.mesh_burst, m.mesh ? m.mesh.info().name.c_str() : "-");
				} else if (m.kind == "LighteningSpawner") {
					printf(" | jitter %g min %g 78 %g,%g,%g,%g 8c %d 90 %g 94 %d taper %d follow %d targets", m.bolt_jitter, m.bolt_min_segment, m.bolt_78[0],
					       m.bolt_78[1], m.bolt_78[2], m.bolt_78[3], m.bolt_8c, m.bolt_90, m.bolt_94, m.bolt_taper, m.bolt_follow);
					for (auto &t : m.bolt_targets) printf(" %s", t.info().name.c_str());
				} else if (m.kind == "BeamSpawner") {
					printf(" | length %g spacing %g~%g radius %g~%g start %g", m.beam_length, m.beam_spacing, m.beam_spacing_variation, m.beam_radius,
					       m.beam_radius_variation, m.beam_start);
				}
				printf("\n");
			}
		}
	}
	return 0;
}

static int cmd_scene(const char *dir, const char *stem) {
	de::World w(dir);
	auto *f = w.file(stem);
	if (!f) {
		fprintf(stderr, "no file %s\n", stem);
		return 1;
	}
	auto items = de::collect_scene(w, *f);
	size_t tris = 0, materials = 0;
	for (auto &it : items) {
		tris += it.mesh.faces.size();
		printf("%-28s %-36s %6zu tris  pos %8.1f %8.1f %8.1f\n", it.actor.c_str(), it.mesh.name.c_str(),
		       it.mesh.faces.size(), it.world.t[0], it.world.t[1], it.world.t[2]);
		for (auto &m : it.materials) {
			++materials;
			auto *b = m.base();
			auto *l = m.light();
			printf("    %-24s base %-28s light %-28s (%zu layers)\n", m.name.c_str(),
			       b ? b->image.info().name.c_str() : "-", l ? l->image.info().name.c_str() : "-", m.layers.size());
		}
	}
	fprintf(stderr, "%zu draw items, %zu triangles, %zu materials\n", items.size(), tris, materials);
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s header|toc|toc+|blocks <file> [outdir]\n", argv[0]);
		return 2;
	}
	try {
		if (!strcmp(argv[1], "header")) {
			return cmd_header(argv[2]);
		}
		if (!strcmp(argv[1], "toc") || !strcmp(argv[1], "toc+")) {
			return cmd_toc(argv[2], argv[1][3] == '+');
		}
		if (!strcmp(argv[1], "scripts") && argc > 3) {
			return cmd_scripts(argv[2], argv[3]);
		}
		if (!strcmp(argv[1], "scene") && argc > 3) {
			return cmd_scene(argv[2], argv[3]);
		}
		if (!strcmp(argv[1], "tree")) {
			return cmd_tree(argv[2]);
		}
		if (!strcmp(argv[1], "image") && argc > 4) {
			return cmd_image(argv[2], argv[3], argv[4], argc > 5 ? atoi(argv[5]) : 0);
		}
		if (!strcmp(argv[1], "disasm") && argc > 3) {
			return cmd_disasm(argv[2], argv[3]);
		}
		if (!strcmp(argv[1], "block") && argc > 5) {
			return cmd_block(argv[2], argv[3], argv[4], argv[5]);
		}
		if (!strcmp(argv[1], "ex") && argc > 5) {
			return cmd_ex(argv[2], argv[3], argv[4], argv[5]);
		}
		if (!strcmp(argv[1], "font") && argc > 3) {  // font <Data dir> <Font_Ariel|Font_Metalic>
			de::World w(argv[2]);
			de::Font f = de::read_font(w, argv[3]);
			printf("height %g, image %s\n", f.height, f.image ? f.image.info().name.c_str() : "-");
			for (int ch = 32; ch < 128; ++ch) {
				auto &g = f.glyphs[size_t(ch)];
				printf("  '%c' x %.2f..%.2f y %.2f..%.2f uv (%.3f %.3f)-(%.3f %.3f)\n", ch, g.x0, g.x1, g.y0, g.y1, g.u0, g.v0, g.u1, g.v1);
			}
			return 0;
		}
		if (!strcmp(argv[1], "passive") && argc > 2) {  // passive <Data>: every Actor\PassiveReference's flags
			de::World w(argv[2]);
			std::map<std::string, int> counts;
			for (auto &e : std::filesystem::directory_iterator(argv[2])) {
				if (e.path().extension() != ".DEObjects") continue;
				const de::ObjectsFile *f = w.file(e.path().stem().string());
				if (!f) continue;
				for (auto &o : f->objects()) {
					if (o.class_path != "Actor\\PassiveReference") continue;
					try {
						de::Contents c(f->object_data(o).contents);
						size_t body = 0;
						int version = 0;
						while (c.remaining() > 1) {
							de::Chunk ch = de::begin_chunk(c);
							if (ch.end > c.size()) break;
							body = c.pos(), version = ch.version;
							c.seek(ch.end);
						}
						if (version != 7) continue;
						c.seek(body);
						if (!c.boolean() || c.u8() != 0x20) continue;
						c.guid();
						de::Guid g = c.guid();
						uint32_t place = c.u32(), add = c.u32(), deact = c.u32();
						bool rename = c.boolean();
						uint32_t mode = c.squeezed();
						de::ObjectRef t = w.by_guid(g);
						char key[128];
						snprintf(key, sizeof key, "mode %u add %u deactivate %u place %u rename %d", mode, add, deact, place, rename);
						counts[key]++;
						if (getenv("DEOBJ_PASSIVE_ALL"))
							printf("%s/%s -> %s: %s\n", e.path().stem().string().c_str(), o.name.c_str(), t ? t.info().name.c_str() : "?", key);
					} catch (const std::exception &) {
					}
				}
			}
			for (auto &[k, n] : counts) printf("%5d  %s\n", n, k.c_str());
			return 0;
		}
		if (!strcmp(argv[1], "avis") && argc > 3) {  // avis <Data> <out dir>: every Image\AVI's video as a .avi file
			std::filesystem::create_directories(argv[3]);
			for (auto &e : std::filesystem::directory_iterator(argv[2])) {
				if (e.path().extension() != ".DEObjects") continue;
				de::ObjectsFile f(read_file(e.path().string().c_str()));
				for (auto &o : f.objects()) {
					if (o.class_path != "Image\\AVI") continue;
					auto riff = de::avi_of_image(f.object_data(o).contents);
					if (riff.empty()) continue;
					auto v = de::decode_avi(riff);
					std::string name = e.path().stem().string() + "_" + o.name;
					for (char &ch : name) {
						if (!isalnum(uint8_t(ch)) && ch != '_' && ch != '-') ch = '_';
					}
					std::ofstream(std::filesystem::path(argv[3]) / (name + ".avi"), std::ios::binary).write((const char *)riff.data(), std::streamsize(riff.size()));
					printf("%-4s %4dx%-4d %4u frames %8zu bytes  %s\n", v.codec.c_str(), v.width, v.height, v.length, riff.size(), name.c_str());
				}
			}
			return 0;
		}
		if (!strcmp(argv[1], "lights") && argc > 2) {  // lights <Data>: every placed Light, its mode and image
			de::World w(argv[2]);
			for (auto &e : std::filesystem::directory_iterator(argv[2])) {
				if (e.path().extension() != ".DEObjects") continue;
				const de::ObjectsFile *f = w.file(e.path().stem().string());
				if (!f) continue;
				for (auto &l : de::read_scene_lights(w, *f)) {
					printf("%-14s %-6s mode %d +0x54 %d blend %d intensity %.2f outer %.0f image %s\n", e.path().stem().string().c_str(),
					       l.cone ? "cone" : "sphere", l.info.mode, l.info.dynamic, de::projector_blend(l.info), l.info.intensity, l.outer,
					       l.info.image ? (l.info.image.info().name + " (" + l.info.image.info().class_path + ")").c_str() : "-");
				}
			}
			return 0;
		}
		if (!strcmp(argv[1], "executors") && argc > 2) {  // executors <Data>: every ClassExecutor's version and interval (+0x44)
			de::World w(argv[2]);
			for (auto &e : std::filesystem::directory_iterator(argv[2])) {
				if (e.path().extension() != ".DEObjects") continue;
				const de::ObjectsFile *fp = w.file(e.path().stem().string());
				if (!fp) continue;
				const de::ObjectsFile &f = *fp;
				for (const auto &o : f.objects()) {
					if (o.class_path != "ClassExecutor") continue;
					de::Contents c(f.object_data(o).contents);
					de::Chunk b = de::begin_chunk(c);
					auto block = [&] {
						uint32_t n = c.u32();
						auto v = c.bytes(n);
						std::string s(v.begin(), v.end());
						while (!s.empty() && s.back() == 0) s.pop_back();
						return s;
					};
					std::string name = block(), entry = b.version >= 2 ? block() : "";
					int32_t interval = b.version == 2 ? 16 : b.version >= 3 ? c.i32() : 0;
					printf("%-28s %-36s v%d interval %d entry %s\n", e.path().stem().string().c_str(), o.name.c_str(), b.version, interval, entry.c_str());
				}
			}
			return 0;
		}
		if (!strcmp(argv[1], "intrinsics") && argc > 2) {  // intrinsics <Data>: every native the compiled classes name
			std::set<std::string> keys;
			for (auto &e : std::filesystem::directory_iterator(argv[2])) {
				if (e.path().extension() != ".DEObjects") continue;
				de::ObjectsFile f(read_file(e.path().string().c_str()));
				for (const auto &o : f.objects()) {
					if (o.class_path != "Class") continue;
					auto d = f.object_data(o);
					auto code = f.ex_payload(d, "VMC2");
					if (!code) continue;
					try {
						auto v = vm::VMClass::parse(code->data, vm::class_behavior_refs(d.references).size());
						for (auto &in : v.intrinsics) keys.insert(v.classes[in.class_index].name + "::" + in.signature);
					} catch (const std::exception &) {
					}
				}
			}
			for (auto &k : keys) printf("%s\n", k.c_str());
			return 0;
		}
		if (!strcmp(argv[1], "meshflags") && argc > 2) {  // meshflags <file>: DE_CMesh +0x40 per mesh
			de::ObjectsFile f(read_file(argv[2]));
			for (auto &o : f.objects()) {
				if (o.class_path.rfind("Mesh\\Standard", 0) != 0) continue;
				try {
					auto m = de::read_mesh(f, o);
					printf("%08x %s%s %s\n", m.flags, m.vertex_lit() ? "vertex-lit" : m.projector_lit() ? "projector-lit" : "unlit",
					       (m.flags & 0x20) ? " projected" : "", o.name.c_str());
				} catch (const std::exception &e) {
					printf("error %s: %s\n", o.name.c_str(), e.what());
				}
			}
			return 0;
		}
		if (!strcmp(argv[1], "stages") && argc > 2) {  // stages <Data dir> [file stem]
			return cmd_stages(argv[2], argc > 3 ? argv[3] : nullptr);
		}
		if (!strcmp(argv[1], "particles") && argc > 2) {  // particles <Data dir> [file stem]
			return cmd_particles(argv[2], argc > 3 ? argv[3] : nullptr);
		}
		if (!strcmp(argv[1], "descriptions") && argc > 2) {  // descriptions <Data>: each description's v6 reference sections
			de::World w(argv[2]);
			for (auto &e : std::filesystem::directory_iterator(argv[2])) {
				if (e.path().extension() != ".DEObjects") continue;
				const de::ObjectsFile *f = w.file(e.path().stem().string());
				if (!f) continue;
				for (const auto &o : f->objects()) {
					if (o.class_path.rfind("Description", 0) != 0) continue;
					de::Contents c(f->object_data(o).references);
					c.seek(de::begin_chunk(c).end);  // DE_CObject's
					if (c.remaining() < 2) continue;
					de::Chunk ch = de::begin_chunk(c);
					if (ch.version != 6) continue;
					auto name = [&] {
						auto t = w.resolve(*f, de::Reference::read(c));
						return t ? t.info().name + " [" + t.info().class_path + "]" : std::string("<null>");
					};
					auto list = [&] {
						std::string out;
						while (c.pos() < ch.end && c.boolean()) out += " " + name();
						return out;
					};
					auto single = [&] { return c.pos() < ch.end && c.boolean() ? " " + name() : std::string(); };
					std::string casts = list(), icon = single(), large = single(), comps = list(), repl = list(), medium = single();
					printf("%s: %s [%s]\n  casts:%s\n  icon:%s large:%s medium:%s\n  components:%s\n  replacements:%s\n",
					       e.path().stem().string().c_str(), o.name.c_str(), o.class_path.c_str(), casts.c_str(), icon.c_str(), large.c_str(),
					       medium.c_str(), comps.c_str(), repl.c_str());
				}
			}
			return 0;
		}
		if (!strcmp(argv[1], "refs") && argc > 4) {  // refs <Data dir> <file stem> <object>
			return cmd_refs(argv[2], argv[3], argv[4]);
		}
		if (!strcmp(argv[1], "chunks") && argc > 3) {
			return cmd_chunks(argv[2], argv[3]);
		}
		if (!strcmp(argv[1], "blocks")) {
			return cmd_blocks(argv[2], argc > 3 ? argv[3] : nullptr);
		}
	} catch (const std::exception &e) {
		fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}
