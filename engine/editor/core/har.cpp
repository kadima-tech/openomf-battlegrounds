#include "har.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "arena.h"
#include "content.h"
#include "image_io.h"
#include "model_build.h"
#include "pilot.h"
#include "script.h"

namespace ed {

const std::array<HarStatInfo, 30> &har_stat_infos() {
	static const std::array<HarStatInfo, 30> k = {{
	    {"Run speed", "Run", false},
	    {"Run speed while turning", "Run", true},
	    {"Run turn rate (max)", "Run", false},
	    {"Run turn rate (step)", "Run", false},
	    {"Turn rate (max)", "Turn", false},
	    {"Turn rate (step)", "Turn", false},
	    {"Walk speed", "Walk", false},
	    {"Walk speed while turning", "Walk", true},
	    {"Walk turn rate (max)", "Walk", false},
	    {"Walk turn rate (step)", "Walk", false},
	    {"Back-up speed", "Walk", false},
	    {"Back-up speed while turning", "Walk", true},
	    {"Back-up turn rate (max)", "Walk", false},
	    {"Back-up turn rate (step)", "Walk", false},
	    {"Air turn rate (max)", "Air", false},
	    {"Air turn rate (step)", "Air", false},
	    {"Air control forward", "Air", false},
	    {"Air control backward", "Air", false},
	    {"Walk lean", "Lean", false},
	    {"(unused)", "Lean", true},
	    {"Run lean", "Lean", false},
	    {"Lean step", "Lean", false},
	    {"Push sphere size", "Body", false},
	    {"(unused)", "Body", true},
	    {"Jump forward", "Jump", false},
	    {"Jump backward", "Jump", false},
	    {"Jump height", "Jump", false},
	    {"Super jump forward", "Jump", false},
	    {"Super jump backward", "Jump", false},
	    {"Super jump height", "Jump", false},
	}};
	return k;
}

namespace {

Bytes stored(const std::string &utf8) {
	Bytes b;
	try {
		b = deo::utf8_to_latin1(utf8);
	} catch (const deo::Error &) {
		for (unsigned char c : utf8) b.push_back(c < 0x80 ? c : '?');
	}
	b.push_back(0);
	return b;
}

std::string utf8(const Bytes &latin1) {
	Bytes b(latin1.begin(), latin1.end());
	while (!b.empty() && b.back() == 0) b.pop_back();
	return deo::latin1_to_utf8(b);
}

void replace_bytes(Bytes &b, const Bytes &from, const Bytes &to) {
	if (from.empty() || b.size() < from.size()) return;
	for (size_t i = 0; i + from.size() <= b.size();) {
		if (std::equal(from.begin(), from.end(), b.begin() + long(i))) {
			std::copy(to.begin(), to.end(), b.begin() + long(i));
			i += from.size();
		} else {
			++i;
		}
	}
}

Bytes gb(const Guid &g) { return Bytes(g.b, g.b + 16); }

// The last chunk of an object's contents (its most-derived class's).
bool last_chunk(const Bytes &contents, size_t *body, size_t *end, uint8_t *version) {
	Chunks c = split_chunks(contents);
	if (c.chunks.empty()) return false;
	size_t at = 0;
	deo::Reader r(contents);
	for (size_t k = 0; k < c.chunks.size(); ++k) {
		size_t e;
		*version = r.chunk(&e);
		at = r.pos();
		if (k + 1 == c.chunks.size()) {
			*body = at, *end = e;
			return true;
		}
		r.seek(e);
	}
	return false;
}

// The description's resource: the passive reference's object GUID.
Guid resource_of(const deo::Object &d) { return description_resource(d); }

// DE_CFighterModelDescription's fighter GUID (+0xf0) sits after the description chunk.
size_t model_fighter_offset(const deo::Object &m) {
	Chunks c = split_chunks(m.contents);
	if (c.chunks.size() < 2) return SIZE_MAX;
	size_t off = 0;
	deo::Reader r(m.contents);
	size_t e;
	r.chunk(&e);
	r.seek(e);
	r.chunk(&e);
	off = r.pos();
	if (e - off < 16) return SIZE_MAX;
	return off;
}

std::vector<std::pair<int, std::string>> numeric_finals(const std::string &members) {
	std::vector<std::pair<int, std::string>> out;
	size_t at = 0;
	while ((at = members.find("final ", at)) != std::string::npos) {
		size_t line_end = members.find('\n', at);
		out.emplace_back(int(at), members.substr(at, (line_end == std::string::npos ? members.size() : line_end) - at));
		at += 6;
	}
	return out;
}

bool parse_final(const std::string &line, HarConstant *c) {
	char type[16], name[128];
	double v;
	std::string s = line;
	for (char &ch : s) {
		if (ch == '=') ch = ' ';
	}
	if (sscanf(s.c_str(), "final %15s %127s %lf", type, name, &v) != 3) return false;
	std::string t = type;
	if (t != "float" && t != "int" && t != "byte" && t != "dword") return false;
	c->type = t;
	c->name = name;
	if (!c->name.empty() && c->name.back() == ';') c->name.pop_back();
	c->value = v;
	size_t cm = line.find("//");
	c->comment = cm == std::string::npos ? "" : line.substr(cm + 2);
	while (!c->comment.empty() && (c->comment.back() == '\r' || c->comment.back() == ' ')) c->comment.pop_back();
	while (!c->comment.empty() && c->comment.front() == ' ') c->comment.erase(0, 1);
	return true;
}

bool tunable(const std::string &name) {
	static const char *const keys[] = {"Damage", "Stun", "Armor", "Recover", "Speed", "Momentum", "Traction", "Energy", "Multiplier"};
	for (const char *k : keys) {
		if (name.find(k) != std::string::npos) return true;
	}
	return false;
}

std::string fmt(double v, const std::string &type) {
	char b[48];
	if (type == "float") {
		snprintf(b, sizeof b, "%.6g", v);
		std::string s = b;
		if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
		return s;
	}
	snprintf(b, sizeof b, "%lld", (long long)std::llround(v));
	return b;
}

}  // namespace

std::vector<std::string> base_fighters(const BaseData &base) {
	std::vector<std::string> out;
	for (auto *d : base.of_class("Description\\Creature\\Fighter")) {
		if (base.by_guid(description_resource(base.model(d->file).objects[d->index]))) out.push_back(d->name);
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

std::vector<uint32_t> addon_hars(const Project &p) {
	std::vector<uint32_t> out;
	const auto &objs = p.file().objects;
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (objs[i].info.class_text() != "Description\\Creature\\Fighter") continue;
		int r = p.find(resource_of(objs[i]));
		if (r >= 0 && objs[size_t(r)].info.class_text() == "Actor\\Creature\\OMF2Fighter") out.push_back(i);
	}
	return out;
}

HarObjects har_objects(const Project &p, uint32_t description) {
	HarObjects o;
	const auto &objs = p.file().objects;
	o.description = int(description);
	o.fighter = p.find(resource_of(objs[description]));
	const Guid dg = objs[description].info.guid;
	for (uint32_t i = 0; i < objs.size(); ++i) {
		const std::string cls = objs[i].info.class_text();
		if (cls == "Description\\FighterModelDescription") {
			size_t off = model_fighter_offset(objs[i]);
			if (off != SIZE_MAX && !memcmp(objs[i].contents.data() + off, dg.b, 16)) o.model_descriptions.push_back(int(i));
		}
	}
	if (o.fighter < 0) return o;
	for (auto &r : object_references(objs[size_t(o.fighter)].info.class_text(), objs[size_t(o.fighter)].references)) {
		int t = p.target(r).local;
		if (t >= 0 && objs[size_t(t)].info.class_text() == "JoystickInputSequenceGroup") o.inputs = t;
	}
	for (uint32_t i = 0; i < objs.size(); ++i) {
		const std::string cls = objs[i].info.class_text();
		if (o.inputs < 0 && cls == "JoystickInputSequenceGroup") o.inputs = int(i);
		if (cls == "Class" && o.behavior_class < 0 && objs[i].info.name_text() == "P" + objs[size_t(o.fighter)].info.name_text()) o.behavior_class = int(i);
	}
	if (o.behavior_class >= 0) {
		for (auto &r : object_references("Class", objs[size_t(o.behavior_class)].references)) {
			int t = p.target(r).local;
			if (t >= 0 && objs[size_t(t)].info.class_text() == "Behavior") o.behavior = t;
		}
	}
	return o;
}

uint32_t create_har_variant(Project &p, const std::string &base_fighter, const std::string &name) {
	const BaseData &base = p.base();
	const BaseObject *d = nullptr;
	for (auto *x : base.of_class("Description\\Creature\\Fighter")) {
		if (x->name == base_fighter && base.by_guid(description_resource(base.model(x->file).objects[x->index]))) d = x;
	}
	if (!d) throw deo::Error("no robot named " + base_fighter);
	const BaseObject *actor = base.by_guid(description_resource(base.model(d->file).objects[d->index]));
	// The whole fighter file: the actor tree, its classes, behaviors, banks, effects and sounds.
	const deo::File &ff = base.model(actor->file);
	std::vector<int> parents = object_parents(ff);
	std::vector<uint32_t> roots;
	for (uint32_t i = 0; i < ff.objects.size(); ++i) {
		if (parents[i] < 0) roots.push_back(i);
	}
	struct Copied {
		size_t file;
		std::map<uint32_t, uint32_t> map;
	};
	std::vector<Copied> copies(1);
	copies[0].file = actor->file;
	const size_t first_new = p.file().objects.size();
	p.copy_from_base(actor->file, roots, &copies[0].map, true);
	if (d->file != actor->file) {
		copies.push_back({d->file, {}});
		p.copy_from_base(d->file, {d->index}, &copies.back().map, true);
	}
	// The paint jobs: their descriptions, still using the base model.
	std::vector<uint32_t> model_roots;
	size_t model_file = SIZE_MAX;
	for (auto *m : base.of_class("Description\\FighterModelDescription")) {
		const deo::Object &mo = base.model(m->file).objects[m->index];
		size_t off = model_fighter_offset(mo);
		if (off == SIZE_MAX || memcmp(mo.contents.data() + off, d->guid.b, 16)) continue;
		if (model_file != SIZE_MAX && model_file != m->file) continue;
		model_file = m->file;
		model_roots.push_back(m->index);
	}
	if (model_roots.empty()) throw deo::Error(base_fighter + " has no FighterModelDescription");
	copies.push_back({model_file, {}});
	p.copy_from_base(model_file, model_roots, &copies.back().map, true);
	// Cross-file links among the copies (the description's resource, the models' fighter GUID).
	auto &objs = p.file().objects;
	for (auto &c : copies) {
		const deo::File &src = base.model(c.file);
		for (auto &[from, to] : c.map) {
			Bytes og = gb(src.objects[from].info.guid), ng = gb(objs[to].info.guid);
			Bytes oa = gb(src.master.guid), na = gb(p.agent());
			Bytes from_pair = oa, to_pair = na;
			from_pair.insert(from_pair.end(), og.begin(), og.end());
			to_pair.insert(to_pair.end(), ng.begin(), ng.end());
			for (size_t i = first_new; i < objs.size(); ++i) {
				replace_bytes(objs[i].contents, from_pair, to_pair);
				replace_bytes(objs[i].contents, og, ng);
			}
		}
	}
	uint32_t desc = copies.size() == 3 ? copies[1].map.at(d->index) : copies[0].map.at(d->index);
	HarObjects o = har_objects(p, desc);
	HarData h = read_har(p, o);
	h.name = name;
	h.listed = true, h.locked = false;
	write_har(p, o, h);
	for (int m : o.model_descriptions) {
		deo::Object &mo = objs[size_t(m)];
		Chunks c = split_chunks(mo.contents);
		deo::Reader r(c.chunks[0].body);
		r.block();
		deo::Writer w;
		std::string mname = name + " " + utf8(mo.info.name);
		w.block(stored(mname));
		w.bytes(Bytes(c.chunks[0].body.begin() + long(r.pos()), c.chunks[0].body.end()));
		c.chunks[0].body = w.data();
		mo.contents = join_chunks(c);
	}
	p.touch();
	return desc;
}

HarData read_har(const Project &p, const HarObjects &o) {
	HarData h;
	const auto &objs = p.file().objects;
	if (o.description >= 0) {
		const deo::Object &d = objs[size_t(o.description)];
		h.name = utf8(d.info.name);
		Chunks c = split_chunks(d.contents);
		if (!c.chunks.empty() && c.chunks[0].version >= 3) {
			deo::Reader r(c.chunks[0].body);
			r.block();
			if (r.u8()) r.seek(r.sized());
			h.listed = r.u8() != 0;
			r.u32();
			h.locked = r.u8() != 0;
		}
	}
	if (o.fighter >= 0) {
		size_t body, end;
		uint8_t v;
		const Bytes &c = objs[size_t(o.fighter)].contents;
		if (last_chunk(c, &body, &end, &v) && v == 2 && end - body >= 120) memcpy(h.stats.data(), c.data() + body, 120);
		// Spheres: every sphere affector below the fighter.
		std::vector<uint32_t> stack{uint32_t(o.fighter)};
		std::vector<std::pair<uint32_t, std::string>> todo;
		while (!stack.empty()) {
			uint32_t i = stack.back();
			stack.pop_back();
			for (uint32_t ch : p.children(i)) {
				stack.push_back(ch);
				if (objs[ch].info.class_text() != "Actor\\Affector\\Sphere") continue;
				Chunks cs = split_chunks(objs[ch].contents);
				if (cs.chunks.size() < 3 || cs.chunks[2].version != 2 || cs.chunks[2].body.size() < 8) continue;
				HarSphere s;
				s.object = int(ch);
				s.name = objs[ch].info.name_text();
				memcpy(&s.hotspot, cs.chunks[2].body.data(), 4);
				memcpy(&s.falloff, cs.chunks[2].body.data() + 4, 4);
				s.bone = sphere_bone(p, int(ch));
				h.spheres.push_back(s);
			}
		}
		std::sort(h.spheres.begin(), h.spheres.end(), [](const HarSphere &a, const HarSphere &b) { return a.object < b.object; });
	}
	if (o.inputs >= 0) {
		try {
			Chunks cs = split_chunks(objs[size_t(o.inputs)].contents);
			deo::Reader r(cs.chunks.at(0).body);
			r.block();
			uint32_t n = r.u32();
			for (uint32_t k = 0; k < n; ++k) {
				HarInput in;
				in.name = text_of(r.block());
				uint32_t steps = r.u32();
				for (uint32_t s = 0; s < steps; ++s) {
					HarStep st;
					st.value = r.u16(), st.mask = r.u16(), st.directions = r.u16(), st.min_age = r.u16(), st.min_gap = r.u16();
					st.max_age = r.u16(), st.max_gap = r.u16();
					in.steps.push_back(st);
				}
				h.inputs.push_back(in);
			}
		} catch (const std::exception &) {
		}
	}
	// Sounds: the copies' Noise2\Stream objects with samples.
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (objs[i].info.class_text() != "Noise2\\Stream" || sound_wave(objs[i]).empty()) continue;
		HarSound s;
		s.object = int(i);
		s.name = objs[i].info.name_text();
		for (auto &e : objs[i].ex) {
			if (text_of(e.key) == "OBJImportFilename") s.source = text_of(e.data);
		}
		h.sounds.push_back(s);
	}
	if (o.behavior >= 0) {
		try {
			de::PCode code = read_pcode(objs[size_t(o.behavior)]);
			std::string members = code.members.source.substr(0, code.members.source.find('\0'));
			for (auto &[at, line] : numeric_finals(members)) {
				HarConstant c;
				if (parse_final(line, &c) && tunable(c.name)) h.constants.push_back(c);
			}
			auto rank = [](const HarConstant &c) {
				return c.name.find("Damage") != std::string::npos ? 0 : c.name.find("Stun") != std::string::npos ? 1
				       : c.name.find("Armor") != std::string::npos                                                ? 2
				                                                                                                   : 3;
			};
			std::stable_sort(h.constants.begin(), h.constants.end(), [&](const HarConstant &a, const HarConstant &b) { return rank(a) < rank(b); });
		} catch (const std::exception &) {
		}
	}
	return h;
}

void write_har(Project &p, const HarObjects &o, const HarData &d) {
	auto &objs = p.file().objects;
	HarData before = read_har(p, o);
	if (o.description >= 0) {
		deo::Object &desc = objs[size_t(o.description)];
		desc.info.name = stored(d.name);
		Chunks c = split_chunks(desc.contents);
		if (!c.chunks.empty() && c.chunks[0].version >= 3) {
			deo::Reader r(c.chunks[0].body);
			r.block();
			size_t name_end = r.pos();
			if (r.u8()) r.seek(r.sized());
			size_t listed = r.pos();
			r.u8();
			r.u32();
			size_t locked = r.pos();
			Bytes body = c.chunks[0].body;
			body[listed] = d.listed;
			body[locked] = d.locked;
			deo::Writer w;
			w.block(stored(d.name));
			w.bytes(Bytes(body.begin() + long(name_end), body.end()));
			c.chunks[0].body = w.data();
			desc.contents = join_chunks(c);
		}
	}
	if (o.fighter >= 0) {
		size_t body, end;
		uint8_t v;
		Bytes &c = objs[size_t(o.fighter)].contents;
		if (last_chunk(c, &body, &end, &v) && v == 2 && end - body >= 120) memcpy(c.data() + body, d.stats.data(), 120);
		objs[size_t(o.fighter)].packed.clear();
	}
	for (auto &s : d.spheres) {
		if (s.object < 0 || size_t(s.object) >= objs.size()) continue;
		Chunks cs = split_chunks(objs[size_t(s.object)].contents);
		if (cs.chunks.size() < 3 || cs.chunks[2].body.size() < 8) continue;
		memcpy(cs.chunks[2].body.data(), &s.hotspot, 4);
		memcpy(cs.chunks[2].body.data() + 4, &s.falloff, 4);
		objs[size_t(s.object)].contents = join_chunks(cs);
		objs[size_t(s.object)].packed.clear();
	}
	if (o.inputs >= 0 && !(d.inputs == before.inputs)) {
		deo::Object &bank = objs[size_t(o.inputs)];
		Chunks cs = split_chunks(bank.contents);
		deo::Reader r(cs.chunks.at(0).body);
		Bytes name = r.block();
		deo::Writer w;
		w.block(name);
		w.u32(uint32_t(d.inputs.size()));
		for (auto &in : d.inputs) {
			w.block(text_bytes(in.name));
			w.u32(uint32_t(in.steps.size()));
			for (auto &st : in.steps) {
				for (uint16_t x : {st.value, st.mask, st.directions, st.min_age, st.min_gap, st.max_age, st.max_gap}) w.u16(x);
			}
		}
		cs.chunks[0].body = w.data();
		bank.contents = join_chunks(cs);
		bank.packed.clear();
	}
	if (o.behavior >= 0 && !(d.constants == before.constants)) {
		de::PCode code = read_pcode(objs[size_t(o.behavior)]);
		std::string members = code.members.source.substr(0, code.members.source.find('\0'));
		for (auto &c : d.constants) {
			for (auto &old : before.constants) {
				if (old.name != c.name || old.value == c.value) continue;
				for (auto &[at, line] : numeric_finals(members)) {
					HarConstant x;
					if (!parse_final(line, &x) || x.name != c.name) continue;
					size_t eq = line.find('=');
					size_t semi = line.find(';', eq);
					if (eq == std::string::npos || semi == std::string::npos) continue;
					members.replace(size_t(at) + eq + 1, semi - eq - 1, fmt(c.value, c.type));
					break;
				}
			}
		}
		code.members.source = members;
		for (auto &e : code.elements) e.source = e.source.substr(0, e.source.find('\0'));
		write_behavior(p, uint32_t(o.behavior), code, behavior_global_targets(p, uint32_t(o.behavior)));
	}
	p.touch();
}

void import_sound(Project &p, uint32_t noise, const Bytes &wav, const std::string &source) {
	deo::Object &o = p.file().objects.at(noise);
	bool done = false;
	for (auto &e : o.ex) {
		if (text_of(e.key) == "DeNoiseSampleExDataId") e.data = wav, done = true;
	}
	if (!done) throw deo::Error("the sound has no sample to replace");
	o.ex.erase(std::remove_if(o.ex.begin(), o.ex.end(), [](const deo::Ex &e) { return text_of(e.key) == "OBJImportFilename"; }), o.ex.end());
	deo::Ex name;
	name.key = text_bytes("OBJImportFilename");
	name.flag = 1;
	name.data = text_bytes(source);
	o.ex.push_back(name);
	p.touch();
}

uint32_t create_paint_job(Project &p, const HarObjects &o, const std::string &name, int from) {
	if (o.model_descriptions.empty()) throw deo::Error("the robot has no paint job to start from");
	if (from < 0) from = o.model_descriptions[0];
	auto &objs = p.file().objects;
	const Guid model = description_resource(objs.at(size_t(from)));
	Bytes from_pair, to_pair;
	uint32_t root;
	if (int local = p.find(model); local >= 0) {
		// An add-on model (an imported robot's, or a paint job made before): copied within the add-on.
		const deo::File snapshot = p.file();
		root = copy_tree(p, snapshot, true, {uint32_t(local)})[0];
		from_pair = gb(p.agent()), to_pair = gb(p.agent());
		Bytes og = gb(snapshot.objects[size_t(local)].info.guid), ng = gb(objs[root].info.guid);
		from_pair.insert(from_pair.end(), og.begin(), og.end());
		to_pair.insert(to_pair.end(), ng.begin(), ng.end());
	} else {
		const BaseObject *bm = p.base().by_guid(model);
		if (!bm) throw deo::Error("the paint job's model is in neither the add-on nor the base game");
		std::map<uint32_t, uint32_t> map;
		root = p.copy_from_base(bm->file, {bm->index}, &map, true).at(0);
		const deo::File &src = p.base().model(bm->file);
		from_pair = gb(src.master.guid), to_pair = gb(p.agent());
		Bytes og = gb(src.objects[bm->index].info.guid), ng = gb(objs[root].info.guid);
		from_pair.insert(from_pair.end(), og.begin(), og.end());
		to_pair.insert(to_pair.end(), ng.begin(), ng.end());
	}
	// The new description: a copy of the source's, its resource the new model, not the default.
	deo::Object d = objs[size_t(from)];
	d.info.guid = fresh_guid();
	d.info.name = stored(name);
	replace_bytes(d.contents, from_pair, to_pair);
	{
		Chunks c = split_chunks(d.contents);
		deo::Reader r(c.chunks[0].body);
		r.block();
		deo::Writer w;
		w.block(stored(name));
		w.bytes(Bytes(c.chunks[0].body.begin() + long(r.pos()), c.chunks[0].body.end()));
		c.chunks[0].body = w.data();
		// v3 [resource][listed][f32 sort][locked][visible when locked][default model]: one default.
		if (c.chunks[0].version >= 3) {
			deo::Reader q(c.chunks[0].body);
			q.block();
			if (q.u8()) q.seek(q.sized());
			size_t flag = q.pos() + 1 + 4 + 1 + 1;
			if (flag < c.chunks[0].body.size()) c.chunks[0].body[flag] = 0;
		}
		d.contents = join_chunks(c);
	}
	d.packed.clear();
	d.packed_as_is = false;
	d.hierarchy = {1, 1, 0};
	objs.push_back(d);
	build::rename(p, root, name + " Model");
	p.touch();
	return uint32_t(objs.size() - 1);
}

int paint_job_model(const Project &p, uint32_t model_description) {
	int m = p.find(description_resource(p.file().objects.at(model_description)));
	return m >= 0 && p.file().objects[size_t(m)].info.class_text() == "FighterModel" ? m : -1;
}

std::vector<PaintImage> paint_images(const Project &p, uint32_t model_description) {
	std::vector<PaintImage> out;
	int root = paint_job_model(p, model_description);
	if (root < 0) return out;
	std::vector<int> parents = object_parents(p.file());
	std::vector<uint32_t> stack{uint32_t(root)};
	std::set<uint32_t> seen;
	while (!stack.empty()) {
		uint32_t i = stack.back();
		stack.pop_back();
		if (!seen.insert(i).second) continue;
		auto kids = p.children(i);
		for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
		const auto &o = p.file().objects[i];
		std::string n = o.info.name_text();
		if (o.info.class_text() != "Image\\Standard") continue;
		if (n != "RGBStatic" && n != "RGBMain" && n != "ReflectionMask" && n != "Bump") continue;
		bool again = false;
		for (auto &x : out) again = again || x.name == n;
		// A mesh's own images (Gargoyle's wings) carry their mesh's name.
		out.push_back({int(i), again && parents[i] >= 0 ? n + " (" + p.file().objects[size_t(parents[i])].info.name_text() + ")" : n});
	}
	return out;
}

namespace {

// A Value\String's text: its second chunk's first block.
std::string string_text(const deo::Object &o) {
	Chunks c = split_chunks(o.contents);
	if (c.chunks.size() < 2) return {};
	try {
		deo::Reader r(c.chunks[1].body);
		return utf8(r.block());
	} catch (const deo::Error &) {
		return {};
	}
}

void set_string_text(deo::Object &o, const std::string &text) {
	Chunks c = split_chunks(o.contents);
	if (c.chunks.size() < 2) return;
	deo::Reader r(c.chunks[1].body);
	r.block();
	deo::Writer w;
	w.block(stored(text));
	w.bytes(Bytes(c.chunks[1].body.begin() + long(r.pos()), c.chunks[1].body.end()));
	c.chunks[1].body = w.data();
	o.contents = join_chunks(c);
	o.packed.clear();
	o.packed_as_is = false;
}

// The description's large representation and, under it, the panel's pieces, by GUID.
struct Panel {
	Guid creator, title_creator, title, picture, text;
	bool found = false;
};

template <typename Lookup>
Panel panel_of(const std::vector<Ref> &creator_refs, Lookup &&class_of_and_refs) {
	Panel out;
	for (auto &r : creator_refs) {
		Guid g;
		std::string cls;
		std::vector<Ref> sub;
		if (!class_of_and_refs(r, &g, &cls, &sub)) continue;
		if (cls == "Image\\Standard" && guid_zero(out.picture)) out.picture = g;
		else if (cls == "Value\\String") out.text = g;
		else if (cls == "ControlCreator" && guid_zero(out.title_creator)) {
			for (auto &t : sub) {
				Guid tg;
				std::string tcls;
				std::vector<Ref> none;
				if (class_of_and_refs(t, &tg, &tcls, &none) && tcls == "Value\\String") {
					out.title_creator = g, out.title = tg;
					break;
				}
			}
		}
	}
	out.found = !guid_zero(out.picture) || !guid_zero(out.text);
	return out;
}

// The add-on description's large representation's GUID (DescriptionInfo order: casts, icon, large).
bool large_of(const Project &p, const deo::Object &d, Guid *out) {
	for (auto &r : object_references(d.info.class_text(), d.references)) {
		Project::Target t = p.target(r);
		if (!t.resolved) continue;
		std::string cls;
		if (t.local >= 0) cls = p.file().objects[size_t(t.local)].info.class_text();
		else if (const BaseObject *b = p.base().by_guid(t.guid)) cls = b->class_path;
		if (cls == "ControlCreator") {
			*out = t.guid;
			return true;
		}
	}
	return false;
}

Panel find_panel(const Project &p, const HarObjects &o, bool *own) {
	Panel none;
	*own = false;
	if (o.description < 0) return none;
	Guid large;
	if (!large_of(p, p.file().objects[size_t(o.description)], &large)) return none;
	if (int local = p.find(large); local >= 0) {
		*own = true;
		auto lookup = [&](const Ref &r, Guid *g, std::string *cls, std::vector<Ref> *sub) {
			Project::Target t = p.target(r);
			if (t.local < 0) {
				const BaseObject *b = t.resolved ? p.base().by_guid(t.guid) : nullptr;
				if (!b) return false;
				*g = t.guid, *cls = b->class_path;
				return true;
			}
			const deo::Object &x = p.file().objects[size_t(t.local)];
			*g = x.info.guid, *cls = x.info.class_text(), *sub = object_references(*cls, x.references);
			return true;
		};
		const deo::Object &c = p.file().objects[size_t(local)];
		Panel out = panel_of(object_references("ControlCreator", c.references), lookup);
		out.creator = large;
		return out;
	}
	const BaseObject *b = p.base().by_guid(large);
	if (!b) return none;
	const deo::File &f = p.base().model(b->file);
	auto lookup = [&](const Ref &r, Guid *g, std::string *cls, std::vector<Ref> *sub) {
		const deo::Object *x = nullptr;
		if (r.kind == Ref::Local && r.index < f.objects.size()) x = &f.objects[r.index];
		else if (r.kind == Ref::Pool && r.index < f.guid_pool.size()) {
			if (const BaseObject *bo = p.base().by_guid(f.guid_pool[r.index]); bo && bo->file == b->file) x = &f.objects[bo->index];
		}
		if (!x) return false;
		*g = x->info.guid, *cls = x->info.class_text(), *sub = object_references(*cls, x->references);
		return true;
	};
	Panel out = panel_of(object_references("ControlCreator", f.objects[b->index].references), lookup);
	out.creator = large;
	return out;
}

const deo::Object *panel_object(const Project &p, const Guid &g) {
	if (guid_zero(g)) return nullptr;
	if (int l = p.find(g); l >= 0) return &p.file().objects[size_t(l)];
	const BaseObject *b = p.base().by_guid(g);
	return b ? &p.base().model(b->file).objects[b->index] : nullptr;
}

}  // namespace

HarPresentation read_presentation(const Project &p, const HarObjects &o) {
	HarPresentation h;
	Panel panel = find_panel(p, o, &h.own);
	h.found = panel.found;
	if (!panel.found) return h;
	if (const deo::Object *t = panel_object(p, panel.title)) h.title = string_text(*t);
	if (const deo::Object *t = panel_object(p, panel.text)) h.text = string_text(*t);
	if (const deo::Object *img = panel_object(p, panel.picture)) {
		int type = 0;
		image_header(*img, &type, &h.width, &h.height);
		if (h.own) h.picture = p.find(panel.picture);
	}
	return h;
}

void own_presentation(Project &p, const HarObjects &o) {
	bool own = false;
	Panel panel = find_panel(p, o, &own);
	if (!panel.found) throw deo::Error("the robot's description has no Choose Creature panel to copy");
	if (own) return;
	const BaseObject *creator = p.base().by_guid(panel.creator);
	if (!creator) throw deo::Error("the panel is not a base object");
	std::vector<uint32_t> roots{creator->index};
	for (const Guid *g : {&panel.title_creator, &panel.title, &panel.picture, &panel.text}) {
		const BaseObject *b = guid_zero(*g) ? nullptr : p.base().by_guid(*g);
		if (b && b->file == creator->file) roots.push_back(b->index);
	}
	std::map<uint32_t, uint32_t> map;
	auto made = p.copy_from_base(creator->file, roots, &map, true);
	auto &objs = p.file().objects;
	const uint32_t copy = made[0];
	std::string name = objs[size_t(o.description)].info.name_text();
	build::rename(p, copy, name + "Creator");
	// The description's large representation: the copy.
	deo::Object &d = objs[size_t(o.description)];
	Chunks rc = split_chunks(d.references);
	const std::string cls = d.info.class_text();
	for (size_t k = 0; k < rc.chunks.size(); ++k) {
		RawChunk out;
		bool changed = false;
		bool ok = map_reference_chunk(cls, k, rc.chunks[k], out, [&](const Ref &r) {
			Project::Target t = p.target(r);
			if (t.resolved && !memcmp(t.guid.b, panel.creator.b, 16)) {
				Ref l;
				l.kind = Ref::Local, l.index = copy;
				changed = true;
				return l;
			}
			return r;
		});
		if (ok && changed) rc.chunks[k] = std::move(out);
	}
	d.references = join_chunks(rc);
	d.packed.clear();
	d.packed_as_is = false;
	p.touch();
}

void write_presentation(Project &p, const HarObjects &o, const std::string &title, const std::string &text) {
	own_presentation(p, o);
	bool own = false;
	Panel panel = find_panel(p, o, &own);
	if (int t = p.find(panel.title); t >= 0) set_string_text(p.file().objects[size_t(t)], title);
	if (int t = p.find(panel.text); t >= 0) set_string_text(p.file().objects[size_t(t)], text);
	p.touch();
}

void set_presentation_picture(Project &p, const HarObjects &o, int width, int height, const std::vector<uint8_t> &rgba, const std::string &source) {
	own_presentation(p, o);
	HarPresentation h = read_presentation(p, o);
	if (h.picture < 0) throw deo::Error("the panel has no picture");
	int w = h.width > 0 ? h.width : width, hh = h.height > 0 ? h.height : height;
	set_image_pixels(p, uint32_t(h.picture), w, hh, resize_rgba(width, height, rgba, w, hh), source);
}

bool attack_sphere(const std::string &name) { return name.find("HitDetector") != std::string::npos || name.find("AttackDetector") != std::string::npos; }

std::string sphere_bone(const Project &p, int sphere) {
	std::vector<int> parents = object_parents(p.file());
	for (int up = parents[size_t(sphere)]; up >= 0; up = parents[size_t(up)]) {
		if (p.file().objects[size_t(up)].info.class_text() == "Actor") return p.file().objects[size_t(up)].info.name_text();
	}
	return "";
}

}  // namespace ed
