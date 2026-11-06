#include "pilot.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "de/contents.h"
#include "image_io.h"

namespace ed {

const char *const kTraitLevels[9] = {"None", "Abysmal", "Low", "Below Average", "Average", "Above Average", "High", "Huge", "Massive"};

const std::array<TraitInfo, 31> &trait_infos() {
	static const std::array<TraitInfo, 31> t = {{
	    {"AffinityLeft", "Left Attacks", "Affinity"},
	    {"AffinityRight", "Right Attacks", "Affinity"},
	    {"AffinityLight", "Light Attacks", "Affinity"},
	    {"AffinityHeavy", "Heavy Attacks", "Affinity"},
	    {"AffinityPunch", "Punches", "Affinity"},
	    {"AffinityKick", "Kicks", "Affinity"},
	    {"AffinitySpecial", "Special Moves", "Affinity"},
	    {"AffinitySuper", "Super Moves", "Affinity"},
	    {"AffinityMelee", "Melee Attacks", "Affinity"},
	    {"AffinityPreimptiveAttack", "Pre-emptive Attacks", "Affinity"},
	    {"AffinityBackAttack", "Back Attacks", "Affinity"},
	    {"AffinityCounter", "Counter Attacks", "Affinity"},
	    {"AffinityJump", "Jumping", "Affinity"},
	    {"AffinityGrapple", "Grapples", "Affinity"},
	    {"AffinityCarryable", "Carryable Weapons", "Affinity"},
	    {"AffinityPowerup", "Powerups", "Affinity"},
	    {"AffinityAttackRange", "Attack Range", "Affinity"},
	    {"PerceptionDistance", "Distance", "Perception"},
	    {"PerceptionTiming", "Timing", "Perception"},
	    {"PerceptionTargetAngle", "Target Angle", "Perception"},
	    {"PerceptionSelfAngle", "Self Angle", "Perception"},
	    {"EmotionExcitability", "Excitability", "Emotion"},
	    {"EmotionCalmDown", "Calm Down", "Emotion"},
	    {"EmotionAnger", "Anger", "Emotion"},
	    {"EmotionFear", "Fear", "Emotion"},
	    {"EmotionValor", "Valor", "Emotion"},
	    {"AffinityVariation", "Attack Variation", "Affinity"},
	    {"AffinityMovementUrgency", "Movement Urgency", "Affinity"},
	    {"AffinityEvade", "Evasion", "Affinity"},
	    {"AffinityMercy", "Mercy", "Affinity"},
	    {"AffinityDefendObjective", "Defend Objective", "Affinity"},
	}};
	return t;
}

namespace {

std::string to_utf8(const std::string &latin1) { return deo::latin1_to_utf8(Bytes(latin1.begin(), latin1.end())); }

Bytes to_latin1(const std::string &utf8) {
	try {
		return deo::utf8_to_latin1(utf8);
	} catch (const deo::Error &) {
		Bytes out;
		for (unsigned char c : utf8) out.push_back(c < 0x80 ? c : '?');
		return out;
	}
}

Bytes stored_text(const std::string &utf8) {
	Bytes b = to_latin1(utf8);
	b.push_back(0);
	return b;
}

float get_f32(const Bytes &b, size_t at) {
	float v;
	memcpy(&v, b.data() + at, 4);
	return v;
}

void put_f32(Bytes &b, size_t at, float v) { memcpy(b.data() + at, &v, 4); }

Bytes f32_bytes(float v) {
	Bytes b(4);
	put_f32(b, 0, v);
	return b;
}

void set_name(deo::Object &o, const std::string &utf8) { o.info.name = stored_text(utf8); }

// DE_CPilot's chunk (v4): offsets of its fields inside the chunk body.
struct PilotLayout {
	size_t rgba[3] = {}, weight[3] = {}, stats = 0, traits[31] = {};
	size_t sex = SIZE_MAX;
};

PilotLayout pilot_layout(const Bytes &body) {
	PilotLayout l;
	deo::Reader r(body);
	for (int k = 0; k < 3; ++k) {
		size_t end;
		r.chunk(&end);
		size_t obj_end;
		r.chunk(&obj_end);
		r.seek(obj_end);
		size_t rgba_end;
		r.chunk(&rgba_end);
		if (rgba_end - r.pos() < 16) throw deo::Error("pilot colour without RGBA");
		l.rgba[k] = r.pos();
		r.seek(end);
		l.weight[k] = r.pos();
		r.u32();
	}
	l.stats = r.pos();
	r.bytes(16);
	for (size_t &t : l.traits) {
		t = r.pos();
		r.u8();
		for (int k = 0; k < 3; ++k) {
			if (r.u8() >= 0xFE) throw deo::Error("pilot trait in a long form");
		}
	}
	if (r.remaining() >= 8) l.sex = r.pos() + 4;
	return l;
}

// Description chunk 0 (DE_CDescription v3): name, resource, listed, f32, locked, visible, ...
struct DescriptionLayout {
	size_t name_end = 0, listed = 0, locked = 0, visible = 0;
};

DescriptionLayout description_layout(const RawChunk &c) {
	if (c.version < 3) throw deo::Error("description chunk v" + std::to_string(c.version));
	DescriptionLayout l;
	deo::Reader r(c.body);
	r.block();
	l.name_end = r.pos();
	if (r.u8()) r.seek(r.sized());
	l.listed = r.pos();
	r.u8();
	r.u32();
	l.locked = r.pos();
	r.u8();
	l.visible = r.pos();
	r.u8();
	return l;
}

// DE_CPilotDescription's chunk (v3): photo passive reference, endurance, agility, power, focus,
// the pilot's name and its three colours (what Choose Pilot's panel shows).
struct PilotDescriptionLayout {
	size_t stats = 0, name = 0, name_end = 0;
};

PilotDescriptionLayout pilot_description_layout(const Bytes &body) {
	PilotDescriptionLayout l;
	deo::Reader r(body);
	if (r.u8()) r.seek(r.sized());
	l.stats = r.pos();
	r.bytes(16);
	l.name = r.pos();
	r.block();
	l.name_end = r.pos();
	r.bytes(36);
	return l;
}

RawChunk &chunk_at(Chunks &c, size_t i, const char *what) {
	if (i >= c.chunks.size()) throw deo::Error(std::string("missing ") + what + " chunk");
	return c.chunks[i];
}

std::string ex_text(const deo::Object &o, const char *key) {
	for (auto &e : o.ex) {
		if (text_of(e.key) == key) return text_of(e.data);
	}
	return {};
}

}  // namespace

std::vector<uint32_t> addon_pilots(const Project &p) {
	std::vector<uint32_t> out;
	for (uint32_t i = 0; i < p.file().objects.size(); ++i) {
		if (p.file().objects[i].info.class_text() == "Actor\\Creature\\Pilot") out.push_back(i);
	}
	return out;
}

PilotObjects pilot_objects(const Project &p, uint32_t pilot) {
	PilotObjects o;
	o.pilot = int(pilot);
	for (uint32_t c : p.children(pilot)) {
		const std::string cls = p.file().objects[c].info.class_text();
		if (cls == "Description\\Pilot" && o.description < 0) o.description = int(c);
		if (cls.rfind("Image", 0) == 0 && o.photo < 0) o.photo = int(c);
	}
	if (o.description >= 0) {
		for (uint32_t c : p.children(uint32_t(o.description))) {
			const auto &info = p.file().objects[c].info;
			if (info.class_text() == "Value\\String" && info.name_text() == "PilotDescription") o.bio = int(c);
		}
	}
	return o;
}

PilotData read_pilot(const Project &p, const PilotObjects &o) {
	PilotData d;
	const auto &objs = p.file().objects;
	if (o.pilot < 0) throw deo::Error("no pilot");
	const deo::Object &pilot = objs[size_t(o.pilot)];
	d.name = to_utf8(pilot.info.name_text());
	Chunks pc = split_chunks(pilot.contents);
	const RawChunk &last = chunk_at(pc, pc.chunks.size() - 1, "pilot");
	if (last.version != 4) throw deo::Error("pilot chunk v" + std::to_string(last.version));
	PilotLayout pl = pilot_layout(last.body);
	for (int k = 0; k < 3; ++k) {
		for (int j = 0; j < 4; ++j) d.colours[size_t(k)][size_t(j)] = get_f32(last.body, pl.rgba[k] + size_t(j) * 4);
		d.weights[size_t(k)] = get_f32(last.body, pl.weight[k]);
	}
	for (int j = 0; j < 4; ++j) d.stats[size_t(j)] = get_f32(last.body, pl.stats + size_t(j) * 4);
	for (int t = 0; t < 31; ++t) {
		d.traits[size_t(t)] = {last.body[pl.traits[t] + 1], last.body[pl.traits[t] + 2], last.body[pl.traits[t] + 3]};
	}
	if (pl.sex != SIZE_MAX) memcpy(&d.sex, last.body.data() + pl.sex, 4);

	if (o.description >= 0) {
		const deo::Object &desc = objs[size_t(o.description)];
		Chunks dc = split_chunks(desc.contents);
		const RawChunk &c0 = chunk_at(dc, 0, "description");
		DescriptionLayout l = description_layout(c0);
		d.listed = c0.body[l.listed] != 0;
		d.locked = c0.body[l.locked] != 0;
		d.visible_when_locked = c0.body[l.visible] != 0;
		Chunks rc = split_chunks(desc.references);
		if (rc.chunks.size() > 1) {
			deo::Reader r(rc.chunks[1].body);
			while (r.remaining() && r.u8() == 1) {
				Ref ref = read_ref(r);
				if (ref.kind != Ref::Null) d.casts.push_back(p.target(ref).guid);
			}
		}
	}
	if (o.bio >= 0) {
		Chunks bc = split_chunks(objs[size_t(o.bio)].contents);
		if (bc.chunks.size() > 1) {
			deo::Reader r(bc.chunks[1].body);
			Bytes t = r.block();
			d.bio = to_utf8(text_of(t));
		}
	}
	if (o.photo >= 0) {
		std::string src = ex_text(objs[size_t(o.photo)], "OBJImportFilename");
		size_t slash = src.find_last_of("\\/");
		d.photo_source = to_utf8(slash == std::string::npos ? src : src.substr(slash + 1));
	}
	return d;
}

void write_pilot(Project &p, const PilotObjects &o, const PilotData &d) {
	auto &objs = p.file().objects;
	deo::Object &pilot = objs.at(size_t(o.pilot));
	set_name(pilot, d.name);
	{
		Chunks pc = split_chunks(pilot.contents);
		RawChunk &actor = chunk_at(pc, 0, "actor");
		deo::Reader r(actor.body);
		r.block();
		Bytes rest(actor.body.begin() + long(r.pos()), actor.body.end());
		deo::Writer w;
		w.block(stored_text(d.name));
		w.bytes(rest);
		actor.body = w.data();
		RawChunk &last = pc.chunks.back();
		PilotLayout pl = pilot_layout(last.body);
		for (int k = 0; k < 3; ++k) {
			for (int j = 0; j < 4; ++j) put_f32(last.body, pl.rgba[k] + size_t(j) * 4, d.colours[size_t(k)][size_t(j)]);
			put_f32(last.body, pl.weight[k], d.weights[size_t(k)]);
		}
		for (int j = 0; j < 4; ++j) put_f32(last.body, pl.stats + size_t(j) * 4, d.stats[size_t(j)]);
		for (int t = 0; t < 31; ++t) {
			const PilotTrait &v = d.traits[size_t(t)];
			last.body[pl.traits[t] + 1] = std::min<uint8_t>(v.level, 8);
			last.body[pl.traits[t] + 2] = std::min<uint8_t>(v.variation, 5);
			last.body[pl.traits[t] + 3] = std::min<uint8_t>(v.wave, 3);
		}
		if (pl.sex != SIZE_MAX) memcpy(last.body.data() + pl.sex, &d.sex, 4);
		pilot.contents = join_chunks(pc);
	}
	if (o.description >= 0) {
		deo::Object &desc = objs[size_t(o.description)];
		set_name(desc, d.name);
		Chunks dc = split_chunks(desc.contents);
		RawChunk &c0 = chunk_at(dc, 0, "description");
		DescriptionLayout l = description_layout(c0);
		c0.body[l.listed] = d.listed, c0.body[l.locked] = d.locked, c0.body[l.visible] = d.visible_when_locked;
		{
			deo::Writer w;
			w.block(stored_text(d.name));
			w.bytes(Bytes(c0.body.begin() + long(l.name_end), c0.body.end()));
			c0.body = w.data();
		}
		if (dc.chunks.size() > 1) {
			RawChunk &c1 = dc.chunks[1];
			PilotDescriptionLayout pl = pilot_description_layout(c1.body);
			const float order[4] = {d.stats[2], d.stats[1], d.stats[0], d.stats[3]};
			for (int j = 0; j < 4; ++j) put_f32(c1.body, pl.stats + size_t(j) * 4, order[j]);
			Bytes tail(c1.body.begin() + long(pl.name_end) + 36, c1.body.end());
			deo::Writer w;
			w.bytes(Bytes(c1.body.begin(), c1.body.begin() + long(pl.name)));
			w.block(stored_text(d.name));
			for (auto &c : d.colours) {
				for (int j = 0; j < 3; ++j) w.bytes(f32_bytes(c[size_t(j)]));
			}
			w.bytes(tail);
			c1.body = w.data();
		}
		desc.contents = join_chunks(dc);

		Chunks rc = split_chunks(desc.references);
		if (rc.chunks.size() > 1) {
			RawChunk &c1 = rc.chunks[1];
			deo::Reader r(c1.body);
			while (r.u8() == 1) read_ref(r);
			Bytes rest(c1.body.begin() + long(r.pos()), c1.body.end());
			deo::Writer w;
			for (const Guid &g : d.casts) {
				Ref ref;
				int local = p.find(g);
				if (local >= 0) ref.kind = Ref::Local, ref.index = uint32_t(local);
				else ref.kind = Ref::Pool, ref.index = p.pool_index(g);
				w.u8(1);
				write_ref(w, ref);
			}
			w.u8(0);
			w.bytes(rest);
			c1.body = w.data();
			desc.references = join_chunks(rc);
		}
	}
	if (o.bio >= 0) {
		deo::Object &bio = objs[size_t(o.bio)];
		Chunks bc = split_chunks(bio.contents);
		RawChunk &c1 = chunk_at(bc, 1, "string value");
		deo::Reader r(c1.body);
		r.block();
		Bytes rest(c1.body.begin() + long(r.pos()), c1.body.end());
		deo::Writer w;
		w.block(stored_text(d.bio));
		w.bytes(rest);
		c1.body = w.data();
		bio.contents = join_chunks(bc);
	}
	if (o.photo >= 0) {
		std::string stem;
		for (char ch : d.name) {
			if (std::isalnum(uint8_t(ch)) || uint8_t(ch) >= 0x80) stem += ch;
		}
		set_name(objs[size_t(o.photo)], stem.empty() ? "Photo" : stem);
		Chunks ic = split_chunks(objs[size_t(o.photo)].contents);
		RawChunk &c0 = chunk_at(ic, 0, "image");
		deo::Reader r(c0.body);
		r.block();
		deo::Writer w;
		w.block(stored_text(stem.empty() ? "Photo" : stem));
		w.bytes(Bytes(c0.body.begin() + long(r.pos()), c0.body.end()));
		c0.body = w.data();
		objs[size_t(o.photo)].contents = join_chunks(ic);
	}
	p.touch();
}

uint32_t create_pilot(Project &p, const std::string &base_pilot, const std::string &name) {
	const BaseData &base = p.base();
	for (size_t f = 0; f < base.files().size(); ++f) {
		for (const BaseObject &bo : base.files()[f].objects) {
			if (bo.class_path != "Actor\\Creature\\Pilot" || bo.name != base_pilot) continue;
			uint32_t made = p.copy_from_base(f, {bo.index}).at(0);
			PilotObjects o = pilot_objects(p, made);
			PilotData d = read_pilot(p, o);
			d.name = name;
			d.listed = true, d.locked = false;
			write_pilot(p, o, d);
			return made;
		}
	}
	throw deo::Error("no pilot named " + base_pilot + " in the base data");
}

void import_photo(Project &p, const PilotObjects &o, int width, int height, const std::vector<uint8_t> &rgba,
                  const std::string &source_name) {
	if (o.photo < 0) throw deo::Error("this pilot has no photo object");
	const int side = 256;
	std::vector<uint8_t> square = crop_square_resize(width, height, rgba, side);
	deo::Object &img = p.file().objects[size_t(o.photo)];
	Chunks ic = split_chunks(img.contents);
	RawChunk &c0 = chunk_at(ic, 0, "image");
	deo::Reader r(c0.body);
	r.block();
	size_t at = r.pos();
	if (c0.body.size() < at + 12) throw deo::Error("image header too short");
	int32_t wh[2] = {side, side};
	memcpy(c0.body.data() + at + 4, wh, 8);
	img.contents = join_chunks(ic);

	std::vector<deo::Ex> ex;
	for (auto &e : img.ex) {
		bool pixels = e.key.size() == 2 && e.key[0] == 0;
		std::string key = text_of(e.key);
		if (!pixels && key != "ImportPath" && key != "OBJImportFilename") ex.push_back(e);
	}
	deo::Writer plain;
	for (int ch = 0; ch < 4; ++ch) {
		plain.u32(uint32_t(ch));
		for (int i = 0; i < side * side; ++i) plain.u8(square[size_t(i) * 4 + size_t(ch)]);
	}
	plain.u32(0xFFFFFFFFu);
	deo::Writer stream;
	stream.u8(0);  // DE_CContents::Decompress method 0: stored
	stream.block(plain.data());
	deo::Writer payload;
	payload.u8(2);
	payload.block(stream.data());
	deo::Ex px;
	px.key = {0, 0};
	px.flag = 0;
	px.data = payload.data();
	ex.insert(ex.begin(), px);
	deo::Ex name;
	name.key = text_bytes("OBJImportFilename");
	name.flag = 1;
	name.data = stored_text(source_name);
	ex.push_back(name);
	img.ex = std::move(ex);
	p.touch();
}

std::vector<uint8_t> image_rgba(const deo::Object &img, int *width, int *height) {
	*width = *height = 0;
	Chunks ic = split_chunks(img.contents);
	if (ic.chunks.empty()) return {};
	int32_t w = 0, h = 0;
	try {
		deo::Reader r(ic.chunks[0].body);
		r.block();
		r.u32();
		w = int32_t(r.u32()), h = int32_t(r.u32());
	} catch (const deo::Error &) {
		return {};
	}
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return {};
	for (auto &e : img.ex) {
		if (e.key.size() != 2 || e.key[0] != 0 || e.key[1] != 0 || e.data.empty()) continue;
		try {
			de::Contents c(e.data);
			uint8_t version = c.u8();
			std::vector<uint8_t> data =
			    version > 1 ? de::Contents::decompress(c.block()) : std::vector<uint8_t>(e.data.begin() + 1, e.data.end());
			size_t n = size_t(w) * size_t(h);
			std::vector<uint8_t> out(n * 4, 255);
			de::Contents q(std::move(data));
			for (;;) {
				int32_t id = q.i32();
				if (id < 0) break;
				auto px = q.bytes(n);
				if (id < 4) {
					for (size_t i = 0; i < n; ++i) out[i * 4 + size_t(id)] = px[i];
				} else if (id == 4) {
					for (size_t i = 0; i < n; ++i) out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = px[i];
				}
			}
			*width = w, *height = h;
			return out;
		} catch (const std::exception &) {
			return {};
		}
	}
	return {};
}

std::vector<uint8_t> photo_rgba(const Project &p, const PilotObjects &o, int *width, int *height) {
	*width = *height = 0;
	if (o.photo < 0) return {};
	return image_rgba(p.file().objects[size_t(o.photo)], width, height);
}

std::vector<uint8_t> base_pilot_photo(const BaseData &base, const BaseObject &pilot, int *width, int *height) {
	*width = *height = 0;
	const deo::File &f = base.model(pilot.file);
	for (uint32_t c : hierarchy_children(f.objects.at(pilot.index))) {
		if (c < f.objects.size() && f.objects[c].info.class_text().rfind("Image", 0) == 0) return image_rgba(f.objects[c], width, height);
	}
	return {};
}

std::vector<std::string> base_pilot_names(const BaseData &base) {
	std::vector<std::string> out;
	for (const BaseObject *o : base.of_class("Actor\\Creature\\Pilot")) out.push_back(o->name);
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

std::string object_name(const Project &p, const Guid &g) {
	int local = p.find(g);
	if (local >= 0) return to_utf8(p.file().objects[size_t(local)].info.name_text());
	if (const BaseObject *o = p.base().by_guid(g)) return to_utf8(o->name);
	return g.str();
}

}  // namespace ed
