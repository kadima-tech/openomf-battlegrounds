#include "content.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>

#include "de/contents.h"
#include "de/pcode.h"
#include "pilot.h"

namespace ed {

namespace {

bool starts(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

bool under(const std::string &s, const char *root) {
	size_t n = strlen(root);
	return s.compare(0, n, root) == 0 && (s.size() == n || s[n] == '\\');
}

std::string latin1(const std::string &s) { return deo::latin1_to_utf8(Bytes(s.begin(), s.end())); }

std::string fmt_float(float v) {
	char b[32];
	snprintf(b, sizeof b, "%g", double(v));
	return b;
}

float f32_at(const Bytes &b, size_t at) {
	float v = 0;
	if (at + 4 <= b.size()) memcpy(&v, b.data() + at, 4);
	return v;
}

int32_t i32_at(const Bytes &b, size_t at) {
	int32_t v = 0;
	if (at + 4 <= b.size()) memcpy(&v, b.data() + at, 4);
	return v;
}

}  // namespace

Category category_of(const std::string &c) {
	if (c == "Actor\\Creature\\Pilot") return Category::Pilots;
	if (c == "Description\\Creature\\Fighter" || c == "Actor\\Creature\\OMF2Fighter" || under(c, "FighterModel") ||
	    c == "Description\\FighterModelDescription" || c == "ImageFilter\\OMF2Fighter" || under(c, "Physics\\Simple\\Fighter"))
		return Category::Robots;
	if (c == "Description\\Environment" || c == "Actor\\Creature\\Environment") return Category::Arenas;
	if (c == "Description\\SessionMaster" || c == "SessionModifier\\StandardSessionMaster" || c == "Description\\SessionModifier" ||
	    under(c, "SessionModifier") || c == "StandardSessionSlotModifier")
		return Category::Tournaments;
	if (under(c, "Image")) return Category::Images;
	if (under(c, "Mesh\\ParticleRenderer") || under(c, "ParticleGenerator") || under(c, "ParticleModifier")) return Category::Particles;
	if (under(c, "Mesh")) return Category::Meshes;
	if (c == "Noise2\\Stream") return Category::Sounds;
	if (under(c, "Noise2") || under(c, "DirectMusicContainer")) return Category::Music;
	if (c == "Behavior" || c == "Class" || under(c, "ClassExecutor")) return Category::Scripts;
	if (under(c, "Description")) return Category::Descriptions;
	if (under(c, "Value")) return Category::Values;
	if (under(c, "TextureManager") || under(c, "TextureProcess") || under(c, "UVModifier") || under(c, "RenderMaterialProperties") ||
	    under(c, "ImageFilter"))
		return Category::Materials;
	if (under(c, "DataChannel") || under(c, "AnimationStreamPlayer") || under(c, "AnimationSequenceGroup") ||
	    under(c, "AnimationSequenceManager") || under(c, "StateManager") || under(c, "ActorDeformInfo") ||
	    under(c, "JoystickInputSequenceGroup") || under(c, "ScaleModifier"))
		return Category::Animation;
	if (under(c, "Actor") || under(c, "Light") || under(c, "Camera") || under(c, "UniqueActorProperty") || under(c, "Physics") ||
	    under(c, "CreatureController") || under(c, "CommandExecutor") || under(c, "CommandModifier"))
		return Category::Scene;
	return Category::Other;
}

const char *category_name(Category c) {
	static const char *const names[kCategoryCount] = {"Pilots", "HARs", "Arenas", "Tournaments", "Images", "Meshes", "Sounds",
	                                                   "Music", "Scripts", "Descriptions", "Values", "Particles", "Materials", "Animation", "Scene", "Other"};
	return names[std::clamp(int(c), 0, kCategoryCount - 1)];
}

// ---------------------------------------------------------------------------------------------
// References

namespace {

struct Grammar {
	const Bytes &b;
	size_t objects, pool;
	std::vector<Ref> &out;
	int budget = 20000;

	bool valid(const Ref &r) const {
		if (r.kind == Ref::Local) return r.index < objects;
		if (r.kind == Ref::Pool) return r.index < pool;
		return true;
	}
	bool ref(deo::Reader &r, size_t end) {
		Ref f = read_ref(r);
		if (r.pos() > end || !valid(f)) return false;
		out.push_back(f);
		return true;
	}
	// (List | Counted | Opt | Chunk)* filling [at, end) exactly.
	bool seq(size_t at, size_t end, int depth) {
		if (at == end) return true;
		if (--budget < 0 || depth > 6) return false;
		const size_t mark = out.size();
		try {
			deo::Reader r(b, at, end);
			bool ok = true;
			for (;;) {
				uint8_t x = r.u8();
				if (x == 0) break;
				if (x != 1 || !ref(r, end)) {
					ok = false;
					break;
				}
			}
			if (ok && seq(r.pos(), end, depth)) return true;
		} catch (const deo::Error &) {
		}
		out.resize(mark);
		try {
			deo::Reader r(b, at, end);
			uint32_t n = r.u32();
			if (n > 0 && n < 4096) {
				bool ok = true;
				for (uint32_t k = 0; k < n && ok; ++k) ok = ref(r, end);
				if (ok && seq(r.pos(), end, depth)) return true;
			}
		} catch (const deo::Error &) {
		}
		out.resize(mark);
		try {
			deo::Reader r(b, at, end);
			if (r.u8() == 1 && ref(r, end) && seq(r.pos(), end, depth)) return true;
		} catch (const deo::Error &) {
		}
		out.resize(mark);
		try {
			deo::Reader r(b, at, end);
			size_t e;
			r.chunk(&e);
			if (e <= end && seq(r.pos(), e, depth + 1) && seq(e, end, depth)) return true;
		} catch (const deo::Error &) {
		}
		out.resize(mark);
		return false;
	}
};

}  // namespace

std::vector<Ref> decode_references(const std::string &class_path, const Bytes &references, size_t objects, size_t pool, bool *complete) {
	std::vector<Ref> out;
	bool ok = true;
	Chunks cs = split_chunks(references);
	for (size_t i = 0; i < cs.chunks.size(); ++i) {
		RawChunk dummy;
		std::vector<Ref> part;
		if (map_reference_chunk(class_path, i, cs.chunks[i], dummy, [&](const Ref &r) {
			    part.push_back(r);
			    return r;
		    })) {
			out.insert(out.end(), part.begin(), part.end());
			continue;
		}
		Grammar g{cs.chunks[i].body, objects, pool, part};
		if (g.seq(0, cs.chunks[i].body.size(), 0)) out.insert(out.end(), part.begin(), part.end());
		else ok = false;
	}
	if (complete) *complete = ok && cs.tail.empty();
	return out;
}

Guid ref_guid(const deo::File &file, const Ref &r) {
	if (r.kind == Ref::Local && r.index < file.objects.size()) return file.objects[r.index].info.guid;
	if (r.kind == Ref::Pool && r.index < file.guid_pool.size()) return file.guid_pool[r.index];
	return {};
}

Guid description_resource(const deo::Object &d) {
	Chunks c = split_chunks(d.contents);
	if (c.chunks.empty()) return {};
	try {
		deo::Reader r(c.chunks[0].body);
		r.block();
		if (!r.u8()) return {};
		uint8_t n = r.u8();
		if (n < 32) return {};
		r.guid();
		return r.guid();
	} catch (const deo::Error &) {
		return {};
	}
}

Guid description_icon(const deo::File &file, const deo::Object &d) {
	Chunks c = split_chunks(d.references);
	if (c.chunks.size() < 2 || c.chunks[1].version != 6) return {};
	try {
		deo::Reader r(c.chunks[1].body);
		while (r.remaining() && r.u8() == 1) read_ref(r);
		if (r.remaining() && r.u8() == 1) return ref_guid(file, read_ref(r));
	} catch (const deo::Error &) {
	}
	return {};
}

std::vector<int> object_parents(const deo::File &file) {
	std::vector<int> out(file.objects.size(), -1);
	for (uint32_t i = 0; i < file.objects.size(); ++i) {
		for (uint32_t c : hierarchy_children(file.objects[i])) {
			if (c < out.size() && out[c] < 0) out[c] = int(i);
		}
	}
	return out;
}

bool image_header(const deo::Object &img, int *type, int *width, int *height) {
	Chunks ic = split_chunks(img.contents);
	if (ic.chunks.empty()) return false;
	try {
		deo::Reader r(ic.chunks[0].body);
		r.block();
		*type = int32_t(r.u32());
		*width = int32_t(r.u32());
		*height = int32_t(r.u32());
		return true;
	} catch (const deo::Error &) {
		return false;
	}
}

std::string behavior_source(const deo::Object &b) {
	try {
		de::Contents c(std::vector<uint8_t>(b.contents.begin(), b.contents.end()));
		std::string text = de::PCode::parse(c).text();
		std::string clean;
		clean.reserve(text.size());
		for (char ch : text) {
			uint8_t u = uint8_t(ch);
			if (u == '\t' || u == '\n' || (u >= 0x20 && u != 0x7f)) clean += ch;
			else if (u != '\r') clean += ' ';
		}
		return latin1(clean);
	} catch (const std::exception &) {
		return {};
	}
}

Bytes sound_wave(const deo::Object &noise) {
	for (auto &e : noise.ex) {
		if (text_of(e.key) == "DeNoiseSampleExDataId" && e.data.size() >= 12 && !memcmp(e.data.data(), "RIFF", 4)) return e.data;
	}
	return {};
}

// ---------------------------------------------------------------------------------------------
// Decoded fields

namespace {

std::string guid_name(const BaseData &base, const deo::File &file, const Guid &g) {
	if (guid_zero(g)) return "(none)";
	for (auto &o : file.objects) {
		if (o.info.guid == g) return latin1(o.info.name_text()) + "  (" + o.info.class_text() + ")";
	}
	if (const BaseObject *b = base.by_guid(g)) return latin1(b->name) + "  (" + b->class_path + ", " + base.files()[b->file].stem + ")";
	return g.str() + "  (not loaded)";
}

void describe_description(const BaseData &base, const deo::File &file, const deo::Object &o, std::vector<Field> &f) {
	Chunks c = split_chunks(o.contents);
	if (c.chunks.empty()) return;
	try {
		const RawChunk &c0 = c.chunks[0];
		deo::Reader r(c0.body);
		f.push_back({"Display name", latin1(text_of(r.block()))});
		Guid res = description_resource(o);
		if (!guid_zero(res)) f.push_back({"Resource", guid_name(base, file, res), res});
		if (r.u8()) r.seek(r.sized());
		if (c0.version >= 2 && r.remaining()) f.push_back({"Listed", r.u8() ? "yes" : "no"});
		if (c0.version >= 3 && r.remaining() >= 6) {
			float sort;
			Bytes s = r.bytes(4);
			memcpy(&sort, s.data(), 4);
			f.push_back({"Sort order", fmt_float(sort)});
			f.push_back({"Locked by default", r.u8() ? "yes" : "no"});
			f.push_back({"Visible when locked", r.u8() ? "yes" : "no"});
		}
	} catch (const deo::Error &) {
	}
	Chunks rc = split_chunks(o.references);
	if (rc.chunks.size() > 1 && rc.chunks[1].version == 6) {
		try {
			deo::Reader r(rc.chunks[1].body);
			int n = 0;
			while (r.remaining() && r.u8() == 1) {
				Guid g = ref_guid(file, read_ref(r));
				f.push_back({n++ ? "" : "Casts", guid_name(base, file, g), g});
			}
			if (!n) f.push_back({"Casts", "(none)"});
			if (r.remaining() && r.u8() == 1) {
				Guid g = ref_guid(file, read_ref(r));
				f.push_back({"Icon", guid_name(base, file, g), g});
			}
			if (r.remaining() && r.u8() == 1) {
				Guid g = ref_guid(file, read_ref(r));
				f.push_back({"Large representation", guid_name(base, file, g), g});
			}
		} catch (const deo::Error &) {
		}
	}
	const std::string cls = o.info.class_text();
	if (c.chunks.size() < 2) return;
	const Bytes &b = c.chunks[1].body;
	try {
		if (cls == "Description\\Pilot") {
			deo::Reader r(b);
			if (r.u8()) r.seek(r.sized());
			size_t at = r.pos();
			f.push_back({"Power / Agility", fmt_float(f32_at(b, at + 8)) + " / " + fmt_float(f32_at(b, at + 4))});
			f.push_back({"Endurance / Focus", fmt_float(f32_at(b, at)) + " / " + fmt_float(f32_at(b, at + 12))});
			r.seek(at + 16);
			f.push_back({"Pilot name", latin1(text_of(r.block()))});
		} else if (cls == "Description\\Environment") {
			f.push_back({"Players", std::to_string(i32_at(b, 0)) + " - " + std::to_string(i32_at(b, 4))});
		} else if (cls == "Description\\FighterModelDescription" && b.size() >= 16) {
			Guid g;
			memcpy(g.b, b.data(), 16);
			f.push_back({"Fighter", guid_name(base, file, g), g});
		}
	} catch (const deo::Error &) {
	}
}

void describe_value(const deo::Object &o, std::vector<Field> &f) {
	Chunks c = split_chunks(o.contents);
	if (c.chunks.size() < 2) return;
	const std::string cls = o.info.class_text();
	const Bytes &b = c.chunks[1].body;
	try {
		if (cls == "Value\\String") {
			deo::Reader r(b);
			f.push_back({"Text", latin1(text_of(r.block()))});
		} else if (cls == "Value\\FLOAT" && b.size() >= 4) {
			f.push_back({"Value", fmt_float(f32_at(b, 0))});
		} else if (cls == "Value\\INT" && b.size() >= 4) {
			f.push_back({"Value", std::to_string(i32_at(b, 0))});
		} else if (cls == "Value\\BOOL" && !b.empty()) {
			f.push_back({"Value", b[0] ? "true" : "false"});
		} else if (cls == "Value\\RGBA" && b.size() >= 16) {
			f.push_back({"RGBA", fmt_float(f32_at(b, 0)) + ", " + fmt_float(f32_at(b, 4)) + ", " + fmt_float(f32_at(b, 8)) + ", " +
			                         fmt_float(f32_at(b, 12))});
		} else if (cls == "Value\\FLOAT\\Inter" && b.size() >= 4) {
			f.push_back({"Value", fmt_float(f32_at(b, 0))});
			if (c.chunks.size() > 2 && c.chunks[2].body.size() >= 8) {
				f.push_back({"Range", fmt_float(f32_at(c.chunks[2].body, 0)) + " - " + fmt_float(f32_at(c.chunks[2].body, 4))});
			}
		}
	} catch (const deo::Error &) {
	}
}

}  // namespace

std::vector<Field> describe_object(const BaseData &base, const deo::File &file, uint32_t index, const Project *addon) {
	std::vector<Field> f;
	if (index >= file.objects.size()) return f;
	const deo::Object &o = file.objects[index];
	const std::string cls = o.info.class_text();
	if (starts(cls, "Description")) describe_description(base, file, o, f);
	else if (starts(cls, "Value")) describe_value(o, f);
	else if (starts(cls, "Image")) {
		int type, w, h;
		if (image_header(o, &type, &w, &h)) {
			f.push_back({"Size", std::to_string(w) + " x " + std::to_string(h)});
			f.push_back({"Image type", std::to_string(type)});
		}
		int levels = 0;
		for (auto &e : o.ex) levels += e.key.size() == 2 && e.key[0] == 0;
		if (levels) f.push_back({"Pixel levels", std::to_string(levels)});
		for (auto &e : o.ex) {
			if (text_of(e.key) == "OBJImportFilename") f.push_back({"Imported from", latin1(text_of(e.data))});
		}
	} else if (cls == "Noise2\\Stream") {
		Chunks c = split_chunks(o.contents);
		if (!c.chunks.empty()) {
			try {
				deo::Reader r(c.chunks[0].body);
				r.block();
				size_t at = r.pos();
				const Bytes &b = c.chunks[0].body;
				f.push_back({"Volume", fmt_float(f32_at(b, at))});
				if (c.chunks[0].version >= 2) f.push_back({"Frequency", fmt_float(f32_at(b, at + 4))});
			} catch (const deo::Error &) {
			}
		}
		Bytes wav = sound_wave(o);
		for (size_t at = 12; at + 8 <= wav.size();) {
			uint32_t len = uint32_t(i32_at(wav, at + 4));
			if (!memcmp(&wav[at], "fmt ", 4) && len >= 16) {
				uint16_t ch = uint16_t(wav[at + 10] | wav[at + 11] << 8), bits = uint16_t(wav[at + 22] | wav[at + 23] << 8);
				f.push_back({"Format", std::to_string(i32_at(wav, at + 12)) + " Hz, " + std::to_string(bits) + "-bit, " +
				                           (ch == 1 ? "mono" : ch == 2 ? "stereo" : std::to_string(ch) + " channels")});
			}
			if (!memcmp(&wav[at], "data", 4)) f.push_back({"Samples", std::to_string(len) + " bytes"});
			at += 8 + len + (len & 1);
		}
	} else if (cls == "Behavior") {
		try {
			de::Contents c(std::vector<uint8_t>(o.contents.begin(), o.contents.end()));
			de::PCode p = de::PCode::parse(c);
			int methods = 0;
			for (auto &e : p.elements) methods += e.type == de::PCodeElement::kMethod;
			f.push_back({"Methods", std::to_string(methods)});
			f.push_back({"Globals", std::to_string(p.global.globals.size())});
		} catch (const std::exception &) {
		}
	} else if (cls == "Actor\\Creature\\Pilot") {
		try {
			PilotData d;
			if (addon && &addon->file() == &file) {
				d = read_pilot(*addon, pilot_objects(*addon, index));
			} else if (const BaseObject *bo = base.by_guid(o.info.guid)) {
				Project scratch(base, "", Project::create(base, "Scratch.DEObjects")->file());
				uint32_t made = scratch.copy_from_base(bo->file, {bo->index}).at(0);
				d = read_pilot(scratch, pilot_objects(scratch, made));
			}
			f.push_back({"Sex", d.sex == kMale ? "Male" : d.sex == kFemale ? "Female" : "Undefined"});
			f.push_back({"Power / Agility", fmt_float(d.stats[0]) + " / " + fmt_float(d.stats[1])});
			f.push_back({"Endurance / Focus", fmt_float(d.stats[2]) + " / " + fmt_float(d.stats[3])});
			f.push_back({"Listed / Locked", std::string(d.listed ? "listed" : "not listed") + ", " + (d.locked ? "locked" : "unlocked")});
		} catch (const std::exception &) {
		}
	}
	for (auto &e : o.ex) {
		std::string key = text_of(e.key);
		if (key == "VMC2") f.push_back({"Bytecode", "VMC2, " + std::to_string(e.data.size()) + " bytes"});
	}
	return f;
}

// ---------------------------------------------------------------------------------------------
// Reference index

ReferenceIndex::ReferenceIndex(const BaseData &base) : base_(base), total_(base.files().size()) {}

ReferenceIndex::~ReferenceIndex() {
	stop_ = true;
	if (worker_.joinable()) worker_.join();
}

void ReferenceIndex::start() {
	if (started_) return;
	started_ = true;
	worker_ = std::thread([this] {
		for (size_t fi = 0; fi < base_.files().size() && !stop_; ++fi) {
			try {
				const auto &index = base_.files()[fi];
				std::ifstream in(index.path, std::ios::binary);
				Bytes data{std::istreambuf_iterator<char>(in), {}};
				deo::File file = deo::File::read(data);
				std::map<std::string, std::vector<const BaseObject *>> found;
				for (uint32_t i = 0; i < file.objects.size() && i < index.objects.size(); ++i) {
					const deo::Object &o = file.objects[i];
					std::set<std::string> seen;
					for (const Ref &r : decode_references(o.info.class_text(), o.references, file.objects.size(), file.guid_pool.size())) {
						Guid g = ref_guid(file, r);
						if (guid_zero(g) || !seen.insert(guid_key(g)).second) continue;
						found[guid_key(g)].push_back(&index.objects[i]);
					}
				}
				std::lock_guard<std::mutex> lock(mutex_);
				for (auto &[k, v] : found) {
					auto &dst = by_target_[k];
					dst.insert(dst.end(), v.begin(), v.end());
				}
			} catch (const std::exception &) {
			}
			++done_;
		}
		done_ = total_;
	});
}

std::vector<const BaseObject *> ReferenceIndex::referencers(const Guid &g) const {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = by_target_.find(guid_key(g));
	return it == by_target_.end() ? std::vector<const BaseObject *>{} : it->second;
}

}  // namespace ed
