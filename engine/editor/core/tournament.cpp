#include "tournament.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "content.h"
#include "json.h"
#include "pilot.h"
#include "script.h"

namespace ed {

namespace {

const char *const kModelName = "OpenBG Tournament";

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

std::string lines_unix(const std::string &s) {
	std::string out;
	for (char c : s) {
		if (c != '\r') out += c;
	}
	return out;
}

std::string lines_dos(const std::string &s) {
	std::string out;
	for (char c : lines_unix(s)) {
		if (c == '\n') out += '\r';
		out += c;
	}
	return out;
}

std::string trim(const std::string &s) {
	size_t a = 0, b = s.size();
	while (a < b && isspace(uint8_t(s[a]))) ++a;
	while (b > a && isspace(uint8_t(s[b - 1]))) --b;
	return s.substr(a, b - a);
}

std::string guid_hex(const Guid &g) { return guid_zero(g) ? std::string() : g.str(); }
Guid guid_of(const std::string &s) { return s.empty() ? Guid{} : Guid::parse(s); }

Ref ref_to(Project &p, const Guid &g) {
	Ref r;
	if (guid_zero(g)) return r;
	int local = p.find(g);
	if (local >= 0) r.kind = Ref::Local, r.index = uint32_t(local);
	else r.kind = Ref::Pool, r.index = p.pool_index(g);
	return r;
}

// ---- reading a structure in any file -------------------------------------------------------

int local_of(const deo::File &f, const Guid &g) {
	if (guid_zero(g)) return -1;
	for (size_t i = 0; i < f.objects.size(); ++i) {
		if (f.objects[i].info.guid == g) return int(i);
	}
	return -1;
}

int local_ref(const deo::File &f, const Ref &r) {
	if (r.kind == Ref::Local && r.index < f.objects.size()) return int(r.index);
	if (r.kind == Ref::Pool && r.index < f.guid_pool.size()) return local_of(f, f.guid_pool[r.index]);
	return -1;
}

std::string cls(const deo::File &f, int i) { return i >= 0 ? f.objects[size_t(i)].info.class_text() : std::string(); }
std::string nm(const deo::File &f, int i) { return i >= 0 ? f.objects[size_t(i)].info.name_text() : std::string(); }

std::vector<int> local_refs(const deo::File &f, int i) {
	std::vector<int> out;
	if (i < 0) return out;
	const auto &o = f.objects[size_t(i)];
	for (auto &r : object_references(o.info.class_text(), o.references)) out.push_back(local_ref(f, r));
	return out;
}

int first_of_class(const deo::File &f, const std::vector<int> &v, const std::string &prefix) {
	for (int i : v) {
		if (i >= 0 && cls(f, i).rfind(prefix, 0) == 0) return i;
	}
	return -1;
}

// DE_CDescription references chunk v6: casts, icon, large representation, components,
// replacements, medium representation.
struct DescriptionRefs {
	Ref icon, large, medium;
};
DescriptionRefs description_refs(const deo::Object &d) {
	DescriptionRefs out;
	Chunks c = split_chunks(d.references);
	if (c.chunks.size() < 2 || c.chunks[1].version != 6) return out;
	try {
		deo::Reader r(c.chunks[1].body);
		while (r.u8() == 1) read_ref(r);
		if (r.u8() == 1) out.icon = read_ref(r);
		if (r.u8() == 1) out.large = read_ref(r);
		while (r.u8() == 1) read_ref(r);
		while (r.u8() == 1) read_ref(r);
		if (r.u8() == 1) out.medium = read_ref(r);
	} catch (const deo::Error &) {
	}
	return out;
}

struct BehaviorGlobals {
	std::vector<std::string> names;
	std::vector<Ref> refs;
};
BehaviorGlobals behavior_globals_of(const deo::Object &b) {
	BehaviorGlobals g;
	try {
		g.names = read_pcode(b).global.globals;
		Chunks cs = split_chunks(b.references);
		if (cs.chunks.size() >= 2) {
			deo::Reader r(cs.chunks[1].body);
			size_t end;
			r.chunk(&end);
			while (r.pos() < end) g.refs.push_back(r.u8() ? read_ref(r) : Ref{});
		}
	} catch (const std::exception &) {
	}
	return g;
}

TournamentObjects find_structure(const deo::File &f, uint32_t description) {
	TournamentObjects o;
	o.description = int(description);
	const deo::Object &d = f.objects.at(description);
	o.master = local_of(f, description_resource(d));
	for (uint32_t c : hierarchy_children(d)) {
		if (cls(f, int(c)) == "Value\\String" && nm(f, int(c)) == "Matches") o.matches = int(c);
	}
	DescriptionRefs dr = description_refs(d);
	o.creator = local_ref(f, dr.large);
	o.icon = local_ref(f, dr.medium) >= 0 ? local_ref(f, dr.medium) : local_ref(f, dr.icon);
	for (int k : local_refs(f, o.creator)) {
		if (cls(f, k) == "Value\\String") o.text = k;
		else if (cls(f, k).rfind("Image", 0) == 0) o.image = k;
	}
	if (o.master < 0) return o;
	int exec = first_of_class(f, local_refs(f, o.master), "ClassExecutor");
	o.master_class = first_of_class(f, local_refs(f, exec), "Class");
	for (int b : local_refs(f, o.master_class)) {
		if (b >= 0 && cls(f, b) == "Behavior" && f.objects[size_t(b)].info.name_text().rfind("BMaster", 0) == 0) o.bmaster = b;
	}
	if (o.bmaster < 0) return o;
	BehaviorGlobals g = behavior_globals_of(f.objects[size_t(o.bmaster)]);
	for (size_t k = 0; k < g.refs.size(); ++k) {
		int t = local_ref(f, g.refs[k]);
		if (t >= 0 && cls(f, t) == "Description\\SessionModifier") o.handler_description = t;
	}
	if (o.handler_description < 0) return o;
	o.sub_master = local_of(f, description_resource(f.objects[size_t(o.handler_description)]));
	int sexec = first_of_class(f, local_refs(f, o.sub_master), "ClassExecutor");
	o.sub_class = first_of_class(f, local_refs(f, sexec), "Class");
	for (int b : local_refs(f, o.sub_class)) {
		if (b >= 0 && cls(f, b) == "Behavior" && f.objects[size_t(b)].info.name_text().rfind("BSubSession", 0) == 0) o.bsub = b;
	}
	return o;
}

// ---- small object edits -------------------------------------------------------------------

void set_first_block(deo::Object &o, size_t chunk, const Bytes &text) {
	Chunks c = split_chunks(o.contents);
	if (chunk >= c.chunks.size()) throw deo::Error("missing chunk in " + o.info.name_text());
	deo::Reader r(c.chunks[chunk].body);
	r.block();
	deo::Writer w;
	w.block(text);
	w.bytes(Bytes(c.chunks[chunk].body.begin() + long(r.pos()), c.chunks[chunk].body.end()));
	c.chunks[chunk].body = w.data();
	o.contents = join_chunks(c);
}

std::string string_value(const deo::Object &o) {
	Chunks c = split_chunks(o.contents);
	if (c.chunks.size() < 2) return {};
	try {
		deo::Reader r(c.chunks[1].body);
		return utf8(r.block());
	} catch (const deo::Error &) {
		return {};
	}
}

void set_string_value(deo::Object &o, const std::string &text) { set_first_block(o, 1, stored(text)); }

// DE_CDescription chunk 0 (v3): name, resource, listed, sort, locked by default, visible when locked.
void set_description(deo::Object &d, const std::string &name, bool listed, bool locked) {
	d.info.name = stored(name);
	Chunks c = split_chunks(d.contents);
	if (c.chunks.empty() || c.chunks[0].version < 3) throw deo::Error("unexpected description layout");
	deo::Reader r(c.chunks[0].body);
	r.block();
	size_t name_end = r.pos();
	if (r.u8()) r.seek(r.sized());
	size_t at_listed = r.pos();
	r.u8();
	r.u32();
	size_t at_locked = r.pos();
	Bytes body = c.chunks[0].body;
	body[at_listed] = listed;
	body[at_locked] = locked;
	deo::Writer w;
	w.block(stored(name));
	w.bytes(Bytes(body.begin() + long(name_end), body.end()));
	c.chunks[0].body = w.data();
	d.contents = join_chunks(c);
}

void read_description_flags(const deo::Object &d, bool *listed, bool *locked) {
	Chunks c = split_chunks(d.contents);
	if (c.chunks.empty() || c.chunks[0].version < 3) return;
	deo::Reader r(c.chunks[0].body);
	r.block();
	if (r.u8()) r.seek(r.sized());
	*listed = r.u8() != 0;
	r.u32();
	*locked = r.u8() != 0;
}

void add_child(Project &p, uint32_t parent, uint32_t child) {
	deo::Object &o = p.file().objects.at(parent);
	Chunks h = split_chunks(o.hierarchy);
	if (h.chunks.empty()) {
		RawChunk c;
		c.version = 1;
		c.body = {0};
		h.chunks.push_back(c);
	}
	Bytes &b = h.chunks[0].body;
	if (b.empty() || b.back() != 0) throw deo::Error("unexpected hierarchy in " + o.info.name_text());
	b.pop_back();
	deo::Writer w;
	w.u8(1);
	Ref r;
	r.kind = Ref::Local, r.index = child;
	write_ref(w, r);
	w.u8(0);
	b.insert(b.end(), w.data().begin(), w.data().end());
	o.hierarchy = join_chunks(h);
}

uint32_t add_object(Project &p, const std::string &class_path, const std::string &name, Bytes contents, Bytes references) {
	deo::Object o;
	o.info.name = stored(name);
	o.info.class_path = text_bytes(class_path);
	o.info.guid = fresh_guid();
	o.info.value84 = 1;
	o.contents = std::move(contents);
	o.references = std::move(references);
	o.hierarchy = {1, 1, 0};
	p.file().objects.push_back(std::move(o));
	p.touch();
	return uint32_t(p.file().objects.size() - 1);
}

Bytes name_chunk_contents(const std::string &name, uint8_t version) {
	deo::Writer w;
	size_t at = w.begin_chunk(version);
	w.block(stored(name));
	w.end_chunk(at);
	return w.data();
}

uint32_t add_folder(Project &p, const std::string &name) {
	return add_object(p, "Folder", name, name_chunk_contents(name, 1), {2, 0, 1, 0});
}

uint32_t add_string_value(Project &p, const std::string &name, const std::string &text) {
	deo::Writer w;
	size_t a = w.begin_chunk(1);
	w.block(stored(name));
	w.end_chunk(a);
	size_t b = w.begin_chunk(2);
	w.block(stored(text));
	w.end_chunk(b);
	return add_object(p, "Value\\String", name, w.data(), {2, 0, 1, 0, 1, 0});
}

// A Value\ObjectReference's targets (its references' last chunk, a list).
std::vector<Guid> object_reference_value(const Project &p, int value) {
	std::vector<Guid> out;
	if (value < 0) return out;
	for (auto &r : object_references("Value\\ObjectReference", p.file().objects[size_t(value)].references)) out.push_back(p.target(r).guid);
	return out;
}

void set_object_reference_value(Project &p, int value, const std::vector<Guid> &targets) {
	if (value < 0) return;
	deo::Object &o = p.file().objects[size_t(value)];
	Chunks c = split_chunks(o.references);
	if (c.chunks.empty()) return;
	deo::Writer w;
	for (auto &g : targets) {
		w.u8(1);
		write_ref(w, ref_to(p, g));
	}
	w.u8(0);
	c.chunks.back().body = w.data();
	p.file().objects[size_t(value)].references = join_chunks(c);
}

// ---- the model as JSON ----------------------------------------------------------------------

json::Value guids_json(const std::vector<Guid> &v) {
	json::Value a = json::Value::array();
	for (auto &g : v) a.push(g.str());
	return a;
}
std::vector<Guid> guids_from(const json::Value *v) {
	std::vector<Guid> out;
	if (v && v->is_array()) {
		for (auto &x : v->items()) out.push_back(guid_of(x.as_string()));
	}
	return out;
}
json::Value quotes_json(const std::vector<TQuote> &v) {
	json::Value a = json::Value::array();
	for (auto &q : v) {
		json::Value o = json::Value::object();
		o["slot"] = q.slot;
		o["text"] = q.text;
		a.push(o);
	}
	return a;
}
std::vector<TQuote> quotes_from(const json::Value *v) {
	std::vector<TQuote> out;
	if (v && v->is_array()) {
		for (auto &x : v->items()) out.push_back({int(x.at("slot").as_int()), x.at("text").as_string()});
	}
	return out;
}
std::string str_at(const json::Value &o, const char *k) {
	const json::Value *v = o.find(k);
	return v && v->is_string() ? v->as_string() : std::string();
}
int int_at(const json::Value &o, const char *k, int def) {
	const json::Value *v = o.find(k);
	return v && v->is_number() ? int(v->as_int()) : def;
}

std::string model_json(const Tournament &t) {
	json::Value m = json::Value::object();
	m["format"] = 1;
	m["name"] = t.name;
	m["choose_text"] = t.choose_text;
	m["listed"] = t.listed;
	m["locked"] = t.locked;
	m["game_type"] = guid_hex(t.game_type);
	m["score_limit"] = guid_hex(t.score_limit);
	m["ai_difficulty"] = double(t.ai_difficulty);
	m["excluded_pilots"] = guids_json(t.excluded_pilots);
	m["player_pilots"] = guids_json(t.player_pilots);
	m["player_robots"] = guids_json(t.player_robots);
	json::Value ms = json::Value::array();
	for (auto &x : t.matches) {
		json::Value o = json::Value::object();
		o["key"] = x.key;
		o["name"] = x.name;
		o["description"] = x.description;
		o["intro"] = x.intro;
		json::Value ops = json::Value::array();
		for (auto &op : x.opponents) {
			json::Value e = json::Value::object();
			e["pilot"] = guid_hex(op.pilot);
			e["robot"] = guid_hex(op.robot);
			e["faction"] = guid_hex(op.faction);
			ops.push(e);
		}
		o["opponents"] = ops;
		o["player_faction"] = guid_hex(x.player_faction);
		o["enemy_faction"] = guid_hex(x.enemy_faction);
		o["arena"] = guid_hex(x.arena);
		o["pre_quotes"] = quotes_json(x.pre_quotes);
		o["win_quotes"] = quotes_json(x.win_quotes);
		o["lose_quotes"] = quotes_json(x.lose_quotes);
		o["win_commentary"] = x.win_commentary;
		o["lose_commentary"] = x.lose_commentary;
		json::Value drops = json::Value::array();
		for (auto &d : x.drop_ons) {
			json::Value e = json::Value::object();
			e["slot"] = d.slot;
			e["actor"] = guid_hex(d.actor);
			e["delay"] = double(d.delay);
			drops.push(e);
		}
		o["drop_ons"] = drops;
		o["on_win"] = x.on_win;
		o["on_lose"] = x.on_lose;
		o["ai_offset"] = x.ai_offset;
		ms.push(o);
	}
	m["matches"] = ms;
	m["ending_arena"] = guid_hex(t.ending_arena);
	m["ending_name"] = t.ending_name;
	m["ending_description"] = t.ending_description;
	json::Value eq = json::Value::array();
	for (auto &q : t.ending_quotes) eq.push(q);
	m["ending_quotes"] = eq;
	json::Value un = json::Value::array();
	for (auto &u : t.unlocks) {
		json::Value e = json::Value::object();
		e["description"] = guid_hex(u.description);
		e["text"] = u.text;
		un.push(e);
	}
	m["unlocks"] = un;
	json::Value ex = json::Value::array();
	for (auto &e : t.extra) {
		json::Value o = json::Value::object();
		o["name"] = e.name;
		o["source"] = e.source;
		o["state"] = e.state;
		o["virtual"] = e.virtual_;
		ex.push(o);
	}
	m["extra"] = ex;
	m["image_source"] = t.image_source;
	m["icon_source"] = t.icon_source;
	return m.dump();
}

Tournament model_from_json(const std::string &text) {
	json::Value m = json::Value::parse(text);
	Tournament t;
	t.name = str_at(m, "name");
	t.choose_text = str_at(m, "choose_text");
	if (auto *v = m.find("listed")) t.listed = v->as_bool();
	if (auto *v = m.find("locked")) t.locked = v->as_bool();
	t.game_type = guid_of(str_at(m, "game_type"));
	t.score_limit = guid_of(str_at(m, "score_limit"));
	if (auto *v = m.find("ai_difficulty")) t.ai_difficulty = float(v->as_real());
	t.excluded_pilots = guids_from(m.find("excluded_pilots"));
	t.player_pilots = guids_from(m.find("player_pilots"));
	t.player_robots = guids_from(m.find("player_robots"));
	if (auto *ms = m.find("matches")) {
		for (auto &o : ms->items()) {
			TMatch x;
			x.key = str_at(o, "key");
			x.name = str_at(o, "name");
			x.description = str_at(o, "description");
			x.intro = str_at(o, "intro");
			if (auto *ops = o.find("opponents")) {
				for (auto &e : ops->items()) x.opponents.push_back({guid_of(str_at(e, "pilot")), guid_of(str_at(e, "robot")), guid_of(str_at(e, "faction"))});
			}
			x.player_faction = guid_of(str_at(o, "player_faction"));
			x.enemy_faction = guid_of(str_at(o, "enemy_faction"));
			x.arena = guid_of(str_at(o, "arena"));
			x.pre_quotes = quotes_from(o.find("pre_quotes"));
			x.win_quotes = quotes_from(o.find("win_quotes"));
			x.lose_quotes = quotes_from(o.find("lose_quotes"));
			x.win_commentary = str_at(o, "win_commentary");
			x.lose_commentary = str_at(o, "lose_commentary");
			if (auto *ds = o.find("drop_ons")) {
				for (auto &e : ds->items()) {
					const json::Value *dl = e.find("delay");
					x.drop_ons.push_back({int_at(e, "slot", 0), guid_of(str_at(e, "actor")), dl ? float(dl->as_real()) : 0.0f});
				}
			}
			x.on_win = int_at(o, "on_win", kNextMatch);
			x.on_lose = int_at(o, "on_lose", kReplay);
			x.ai_offset = int_at(o, "ai_offset", 0);
			t.matches.push_back(std::move(x));
		}
	}
	t.ending_arena = guid_of(str_at(m, "ending_arena"));
	t.ending_name = str_at(m, "ending_name");
	t.ending_description = str_at(m, "ending_description");
	if (auto *v = m.find("ending_quotes")) {
		for (auto &q : v->items()) t.ending_quotes.push_back(q.as_string());
	}
	if (auto *v = m.find("unlocks")) {
		for (auto &e : v->items()) t.unlocks.push_back({guid_of(str_at(e, "description")), str_at(e, "text")});
	}
	if (auto *v = m.find("extra")) {
		for (auto &e : v->items()) {
			TMethod x;
			x.name = str_at(e, "name");
			x.source = str_at(e, "source");
			if (auto *s = e.find("state")) x.state = s->as_bool();
			if (auto *s = e.find("virtual")) x.virtual_ = s->as_bool();
			t.extra.push_back(x);
		}
	}
	t.image_source = str_at(m, "image_source");
	t.icon_source = str_at(m, "icon_source");
	return t;
}

// ---- globals the scripts can name ------------------------------------------------------------

struct GlobalMap {
	std::map<std::string, std::string> name_of;  // guid key -> global name
	std::map<std::string, Guid> target_of;      // global name -> target
	void add(const std::string &name, const Guid &g) {
		if (name.empty()) return;
		target_of.emplace(name, g);
		if (!guid_zero(g)) name_of.emplace(guid_key(g), name);
	}
};

const BaseObject *base_behavior(const BaseData &base, const std::string &stem, const std::string &name) {
	const auto *f = base.file(stem);
	if (!f) return nullptr;
	for (auto &o : f->objects) {
		if (o.class_path == "Behavior" && o.name == name) return &o;
	}
	return nullptr;
}

void add_base_globals(GlobalMap &m, const BaseData &base, const BaseObject *b) {
	if (!b) return;
	const deo::File &f = base.model(b->file);
	BehaviorGlobals g = behavior_globals_of(f.objects[b->index]);
	for (size_t k = 0; k < g.names.size(); ++k) m.add(g.names[k], k < g.refs.size() ? ref_guid(f, g.refs[k]) : Guid{});
}

// The globals BSubSession's code sees besides its own: BSinglePlayerSubSession's, then BMaster's.
GlobalMap visible_globals(const Project &p, const TournamentObjects &o) {
	GlobalMap m;
	add_base_globals(m, p.base(), base_behavior(p.base(), "SinglePlayer", "BSinglePlayerSubSession"));
	add_base_globals(m, p.base(), base_behavior(p.base(), "SinglePlayer", "BSinglePlayerSession"));
	if (o.bmaster >= 0) {
		BehaviorGlobals g = behavior_globals_of(p.file().objects[size_t(o.bmaster)]);
		for (size_t k = 0; k < g.names.size(); ++k) m.add(g.names[k], k < g.refs.size() ? p.target(g.refs[k]).guid : Guid{});
	}
	return m;
}

std::string identifier(const std::string &s) {
	std::string out;
	bool up = true;
	for (unsigned char c : s) {
		if (isalnum(c)) {
			out += up ? char(toupper(c)) : char(c);
			up = false;
		} else {
			up = true;
		}
	}
	if (out.empty() || isdigit(uint8_t(out[0]))) out = "X" + out;
	return out;
}

std::string literal(const std::string &text) {
	std::string out;
	for (char c : lines_unix(text)) {
		if (c == '\n') out += "<Return>";
		else if (c == '"') out += '\'';
		else if (c == '\\') out += '/';
		else out += c;
	}
	return out;
}

std::string unliteral(const std::string &text) {
	std::string out = text;
	for (size_t at; (at = out.find("<Return>")) != std::string::npos;) out.replace(at, 8, "\n");
	return out;
}

std::string fmt_float(float v) {
	char b[32];
	if (v == std::floor(v)) snprintf(b, sizeof b, "%d", int(v));
	else snprintf(b, sizeof b, "%g", double(v));
	return b;
}

// ---- reading base scripts into a model ------------------------------------------------------

// Statements of a method's source (one per ';' at depth 0, comments dropped), braces kept apart.
std::vector<std::string> statements(const std::string &source) {
	std::vector<std::string> out;
	std::string cur;
	const std::string s = lines_unix(source);
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
			while (i < s.size() && s[i] != '\n') ++i;
			continue;
		}
		if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
			size_t e = s.find("*/", i + 2);
			i = e == std::string::npos ? s.size() : e + 1;
			continue;
		}
		if (c == '"') {
			size_t e = s.find('"', i + 1);
			if (e == std::string::npos) e = s.size() - 1;
			cur += s.substr(i, e - i + 1);
			i = e;
			continue;
		}
		if (c == ';' || c == '{' || c == '}') {
			std::string t = trim(cur);
			if (c != ';' && !t.empty()) out.push_back(t);
			if (c == ';') out.push_back(t + ";");
			else out.push_back(std::string(1, c));
			cur.clear();
			continue;
		}
		cur += c;
	}
	if (!trim(cur).empty()) out.push_back(trim(cur));
	return out;
}

bool call(const std::string &st, const std::string &fn, std::vector<std::string> *args) {
	std::string s = st;
	s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n'; }), s.end());
	if (s.rfind(fn + "(", 0) != 0 || s.size() < fn.size() + 3 || s.substr(s.size() - 2) != ");") return false;
	std::string inner = s.substr(fn.size() + 1, s.size() - fn.size() - 3);
	args->clear();
	int depth = 0;
	std::string cur;
	for (char c : inner) {
		if (c == '(') ++depth;
		if (c == ')') --depth;
		if (c == ',' && depth == 0) {
			args->push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!inner.empty()) args->push_back(cur);
	return true;
}

bool quote_assignment(const std::string &st, std::string *text) {
	std::string s = trim(st);
	if (s.rfind("m_Quote", 0) != 0) return false;
	size_t q = s.find('"'), e = s.rfind('"');
	if (q == std::string::npos || e <= q) return false;
	*text = unliteral(s.substr(q + 1, e - q - 1));
	return true;
}

struct Importer {
	const Project &p;
	GlobalMap globals;
	Guid resolve(const std::string &name) const {
		auto it = globals.target_of.find(trim(name));
		return it == globals.target_of.end() ? Guid{} : it->second;
	}
};

// A match state's body: what the model expresses, and the rest as the user block's text.
void import_match(const Importer &im, const std::string &source, TMatch &m, std::string *rest) {
	std::string quote;
	bool have_quote = false;
	std::vector<std::string> a;
	for (auto &st : statements(source)) {
		if (st == "exKillCallers();" || st == "RegisterState();" || st == "exSleep();") continue;
		if (quote_assignment(st, &quote)) {
			have_quote = true;
			continue;
		}
		if (call(st, "SubSessionActivateSlot", &a) && (a.size() == 3 || a.size() == 4)) {
			TOpponent op;
			op.robot = a[1] == "CreatureListGetRandom()" ? Guid{} : im.resolve(a[1]);
			op.pilot = im.resolve(a[2]);
			if (a.size() == 4) op.faction = im.resolve(a[3]);
			size_t slot = size_t(std::max(1, atoi(a[0].c_str())));
			if (m.opponents.size() < slot) m.opponents.resize(slot);
			m.opponents[slot - 1] = op;
			continue;
		}
		if (call(st, "SubSessionSetFactions", &a) && a.size() == 2) {
			m.player_faction = im.resolve(a[0]), m.enemy_faction = im.resolve(a[1]);
			continue;
		}
		if (call(st, "SinglePlayerPickRandomEnvironment", &a)) {
			m.arena = {};
			continue;
		}
		if (call(st, "SubSessionPickEnvironment", &a) && a.size() == 1) {
			m.arena = im.resolve(a[0]);
			continue;
		}
		if (call(st, "AddDropActorOnSlotCreature", &a) && a.size() == 3) {
			m.drop_ons.push_back({atoi(a[0].c_str()), im.resolve(a[1]), float(atof(a[2].c_str()))});
			continue;
		}
		if (have_quote && call(st, "AddPilotQuote", &a) && a.size() == 2 && a[1] == "m_Quote") {
			m.pre_quotes.push_back({atoi(a[0].c_str()), quote});
			continue;
		}
		if (have_quote && call(st, "SetIntroMatchCommentary", &a)) {
			m.intro = quote;
			continue;
		}
		if (have_quote && call(st, "SetNextMatchDescription", &a)) {
			m.description = quote;
			continue;
		}
		if (have_quote && call(st, "SetNextMatchName", &a)) {
			m.name = quote;
			continue;
		}
		*rest += st + "\n";
	}
	if (m.opponents.empty()) m.opponents.resize(1);
}

// "if (m_PlayerLost) { m_Quote=...; SetMatchCommentary(m_Quote); } else { ... }"
bool import_match_complete(const std::string &source, TMatch &m) {
	auto st = statements(source);
	std::vector<std::string> t;
	for (auto &s : st) t.push_back(s);
	std::string lose, win, q;
	std::vector<std::string> a;
	if (t.size() == 10 && t[0] == "if (m_PlayerLost)" && t[1] == "{" && quote_assignment(t[2], &lose) && call(t[3], "SetMatchCommentary", &a) &&
	    t[4] == "}" && t[5] == "else" && t[6] == "{" && quote_assignment(t[7], &win) && call(t[8], "SetMatchCommentary", &a) && t[9] == "}") {
		m.lose_commentary = lose, m.win_commentary = win;
		return true;
	}
	return false;
}

void import_cinematic(const Importer &im, const std::string &source, Tournament &t, std::string *rest) {
	std::string quote;
	bool have = false;
	std::vector<std::string> a;
	for (auto &st : statements(source)) {
		if (st == "exKillCallers();" || st == "RegisterState();" || st == "exSleep();" || st == "instance CText Pilot;") continue;
		if (quote_assignment(st, &quote)) {
			have = true;
			continue;
		}
		if (call(st, "SubSessionPickEnvironment", &a) && a.size() == 1) {
			t.ending_arena = im.resolve(a[0]);
			continue;
		}
		if (have && call(st, "SetNextMatchDescription", &a)) {
			t.ending_description = quote;
			continue;
		}
		if (have && call(st, "SetNextMatchName", &a)) {
			t.ending_name = quote;
			continue;
		}
		if (have && call(st, "AddUnlock", &a) && a.size() == 2) {
			t.unlocks.push_back({im.resolve(a[0]), quote});
			continue;
		}
		if (have && call(st, "AddCinematicQuote", &a)) {
			t.ending_quotes.push_back(quote);
			continue;
		}
		*rest += st + "\n";
	}
}

std::vector<std::string> enum_matches(const std::string &members) {
	std::vector<std::string> out;
	std::string s = lines_unix(members);
	size_t e = s.find("enum EMatch");
	if (e == std::string::npos) return out;
	size_t a = s.find('{', e), b = s.find('}', e);
	if (a == std::string::npos || b == std::string::npos) return out;
	std::string body = s.substr(a + 1, b - a - 1);
	std::string clean;
	for (size_t i = 0; i < body.size(); ++i) {
		if (body[i] == '/' && i + 1 < body.size() && body[i + 1] == '/') {
			while (i < body.size() && body[i] != '\n') ++i;
			continue;
		}
		clean += body[i];
	}
	std::string cur;
	for (char c : clean + ",") {
		if (c == ',') {
			std::string n = trim(cur);
			if (!n.empty()) out.push_back(n);
			cur.clear();
		} else {
			cur += c;
		}
	}
	return out;
}

float final_float(const std::string &members, const std::string &name) {
	std::string s = lines_unix(members);
	size_t at = s.find(name + "=");
	if (at == std::string::npos) at = s.find(name + " =");
	if (at == std::string::npos) return 0;
	return float(atof(s.c_str() + s.find('=', at) + 1));
}

struct Imported {
	Tournament t;
	std::map<std::string, std::string> blocks;  // user blocks for the first generation
};

Imported import_scripts(const Project &p, const TournamentObjects &o) {
	Imported out;
	Tournament &t = out.t;
	Importer im{p, visible_globals(p, o)};
	de::PCode sub = read_pcode(p.file().objects.at(size_t(o.bsub)));
	de::PCode master = read_pcode(p.file().objects.at(size_t(o.bmaster)));
	{
		BehaviorGlobals g = behavior_globals_of(p.file().objects[size_t(o.bsub)]);
		for (size_t k = 0; k < g.names.size(); ++k) im.globals.add(g.names[k], k < g.refs.size() ? p.target(g.refs[k]).guid : Guid{});
	}
	t.ai_difficulty = final_float(master.members.source, "m_TournamentAIDifficulty");
	std::vector<std::string> order = enum_matches(master.members.source);
	// SetupMatch: "if (m_MatchID==matchX) MethodY();"
	std::map<std::string, std::string> method_of;
	int setup = -1;
	for (size_t i = 0; i < sub.elements.size(); ++i) {
		if (sub.elements[i].link < 0 && sub.elements[i].name == "SetupMatch") setup = int(i);
	}
	if (setup >= 0) {
		auto st = statements(sub.elements[size_t(setup)].source);
		for (size_t k = 0; k < st.size(); ++k) {
			const std::string &s = st[k];
			size_t eq = s.find("m_MatchID==");
			if (s.rfind("if", 0) != 0 || eq == std::string::npos) continue;
			size_t close = s.find(')', eq);
			if (close == std::string::npos) continue;
			std::string id = trim(s.substr(eq + 11, close - eq - 11));
			std::string next = trim(s.substr(close + 1));
			if (next.empty() && k + 1 < st.size()) next = trim(st[k + 1]);
			method_of[id] = trim(next.substr(0, next.find('(')));
		}
	}
	std::set<size_t> used;
	if (setup >= 0) used.insert(size_t(setup));
	std::map<std::string, int> match_index;  // method name -> match
	for (auto &id : order) {
		if (id == "matchCinematic" || id == "matchEnd" || !method_of.count(id)) continue;
		const std::string method = method_of[id];
		int el = -1;
		for (size_t i = 0; i < sub.elements.size(); ++i) {
			if (sub.elements[i].link < 0 && sub.elements[i].name == method) el = int(i);
		}
		if (el < 0) continue;
		TMatch m;
		m.key = id.size() > 5 ? id.substr(5) : id;
		std::string rest;
		import_match(im, sub.elements[size_t(el)].source, m, &rest);
		match_index[method] = int(t.matches.size());
		used.insert(size_t(el));
		t.matches.push_back(std::move(m));
		if (!trim(rest).empty()) out.blocks["Match" + t.matches.back().key] = rest;
		// Rename the method to Match<Key> when the base names it otherwise.
		if (method != "Match" + t.matches.back().key) match_index["Match" + t.matches.back().key] = match_index[method];
	}
	for (size_t i = 0; i < sub.elements.size(); ++i) {
		const auto &e = sub.elements[i];
		if (used.count(i) || e.type != de::PCodeElement::kMethod || e.excluded) continue;
		if (e.link < 0 && e.name == "Cinematic") {
			std::string rest;
			import_cinematic(im, e.source, t, &rest);
			if (!trim(rest).empty()) out.blocks["Cinematic"] = rest;
			continue;
		}
		std::string q = sub.qualified_name(i);
		std::string top = q.substr(0, q.find('.'));
		auto mi = match_index.find(top);
		if (mi != match_index.end() && e.link >= 0 && sub.elements[size_t(e.link)].link < 0) {
			TMatch &m = t.matches[size_t(mi->second)];
			std::string key = "Match" + m.key;
			if (e.name == "MatchComplete" && import_match_complete(e.source, m)) continue;
			if (e.name == "MatchComplete" || e.name == "InitialBeginAction") {
				out.blocks[key + "." + e.name] = lines_unix(e.source);
				continue;
			}
		}
		if (mi != match_index.end()) {
			std::string key = "Match" + t.matches[size_t(mi->second)].key;
			q = key + q.substr(top.size());
		}
		t.extra.push_back({q, lines_unix(e.source), e.method_id == 0, e.virtual_});
	}
	return out;
}

// ---- generating ------------------------------------------------------------------------------

struct Gen {
	const Project &p;
	GlobalMap visible;
	std::vector<std::string> own_names;
	std::vector<Guid> own_targets;
	std::set<std::string> taken;

	std::string global(const Guid &g, const std::string &kind) {
		if (guid_zero(g)) return "null";
		if (auto it = visible.name_of.find(guid_key(g)); it != visible.name_of.end()) return it->second;
		for (size_t k = 0; k < own_targets.size(); ++k) {
			if (own_targets[k] == g) return own_names[k];
		}
		std::string base = "g_" + kind + identifier(object_name(p, g));
		std::string n = base;
		for (int k = 2; taken.count(n) || visible.target_of.count(n); ++k) n = base + std::to_string(k);
		taken.insert(n);
		own_names.push_back(n);
		own_targets.push_back(g);
		return n;
	}
};

std::string block(const std::map<std::string, std::string> &blocks, const std::string &id) {
	std::string out = "// <user " + id + ">\n";
	auto it = blocks.find(id);
	if (it != blocks.end() && !it->second.empty()) {
		out += it->second;
		if (out.back() != '\n') out += "\n";
	}
	return out + "// </user>\n";
}

std::string quote_lines(const std::vector<TQuote> &qs, const char *indent = "") {
	std::string s;
	for (auto &q : qs) {
		s += std::string(indent) + "m_Quote=\"" + literal(q.text) + "\";\n";
		s += std::string(indent) + "AddPilotQuote(" + std::to_string(q.slot) + ",m_Quote);\n";
	}
	return s;
}

std::string match_target(const Tournament &t, int target) {
	if (target >= kEnding || target >= int(t.matches.size())) return "matchCinematic";
	return "match" + t.matches[size_t(std::max(0, target))].key;
}

}  // namespace

int TournamentObjects::value(const Project &p, const char *name) const {
	if (master < 0) return -1;
	for (uint32_t c : p.children(uint32_t(master))) {
		const auto &o = p.file().objects[c];
		if (o.info.class_text() == "Value\\ObjectReference" && o.info.name_text() == name) return int(c);
	}
	return -1;
}

std::vector<std::pair<std::string, std::string>> user_blocks(const std::string &source) {
	std::vector<std::pair<std::string, std::string>> out;
	std::string s = lines_unix(source);
	size_t at = 0;
	while ((at = s.find("// <user ", at)) != std::string::npos) {
		size_t close = s.find('>', at);
		size_t line_end = s.find('\n', at);
		if (close == std::string::npos || line_end == std::string::npos || close > line_end) break;
		std::string id = s.substr(at + 9, close - at - 9);
		size_t end = s.find("// </user>", line_end);
		if (end == std::string::npos) break;
		out.emplace_back(id, s.substr(line_end + 1, end - line_end - 1));
		at = end;
	}
	return out;
}

std::vector<const BaseObject *> base_tournaments(const BaseData &base) {
	std::vector<const BaseObject *> out;
	for (const char *stem : {"SinglePlayer", "SinglePlayerFull"}) {
		const auto *fi = base.file(stem);
		if (!fi) continue;
		size_t file = size_t(fi - &base.files()[0]);
		const deo::File &f = base.model(file);
		for (auto &o : fi->objects) {
			if (o.class_path != "Description\\SessionMaster") continue;
			TournamentObjects s = find_structure(f, o.index);
			if (s.bmaster >= 0 && s.bsub >= 0 && s.sub_class >= 0) out.push_back(&o);
		}
	}
	return out;
}

std::vector<uint32_t> addon_tournaments(const Project &p) {
	std::vector<uint32_t> out;
	const auto &objs = p.file().objects;
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (objs[i].info.class_text() == "Description\\SessionMaster" && tournament_objects(p, i).model >= 0) out.push_back(i);
	}
	return out;
}

TournamentObjects tournament_objects(const Project &p, uint32_t description) {
	TournamentObjects o = find_structure(p.file(), description);
	const auto &objs = p.file().objects;
	std::vector<int> parents = object_parents(p.file());
	for (int up = parents[description]; up >= 0; up = parents[size_t(up)]) {
		for (uint32_t c : p.children(uint32_t(up))) {
			if (objs[c].info.class_text() == "Value\\String" && objs[c].info.name_text() == kModelName) {
				o.folder = up, o.model = int(c);
			}
		}
		if (o.model >= 0) break;
	}
	return o;
}

Tournament read_tournament(const Project &p, const TournamentObjects &o) {
	Tournament t;
	if (o.model >= 0) {
		try {
			t = model_from_json(string_value(p.file().objects[size_t(o.model)]));
		} catch (const std::exception &) {
		}
	}
	if (o.description >= 0) {
		const auto &d = p.file().objects[size_t(o.description)];
		t.name = utf8(d.info.name);
		read_description_flags(d, &t.listed, &t.locked);
	}
	if (o.text >= 0) t.choose_text = string_value(p.file().objects[size_t(o.text)]);
	t.excluded_pilots = object_reference_value(p, o.value(p, "ExcludePilots"));
	t.player_pilots = object_reference_value(p, o.value(p, "PlayerPilots"));
	t.player_robots = object_reference_value(p, o.value(p, "PlayerCreatures"));
	if (o.bmaster >= 0) {
		BehaviorGlobals g = behavior_globals_of(p.file().objects[size_t(o.bmaster)]);
		for (size_t k = 0; k < g.names.size() && k < g.refs.size(); ++k) {
			Guid target = p.target(g.refs[k]).guid;
			if (g.names[k] == "g_GameDescription") t.game_type = target;
			if (g.names[k] == "g_ScoreLimit") t.score_limit = target;
			if (g.names[k] == "g_EndingEnvironment") t.ending_arena = target;
		}
	}
	return t;
}

GeneratedScripts generate_scripts(const Project &p, const TournamentObjects &o, const Tournament &t,
                                  const std::map<std::string, std::string> *initial_blocks) {
	GeneratedScripts out;
	std::map<std::string, std::string> blocks;
	if (initial_blocks) {
		blocks = *initial_blocks;
	} else {
		for (auto &[id, text] : user_blocks(script_text(read_pcode(p.file().objects.at(size_t(o.bsub)))).text)) blocks[id] = text;
	}
	std::map<std::string, std::string> master_blocks;
	const de::PCode base_master = read_pcode(p.file().objects.at(size_t(o.bmaster)));
	for (auto &[id, text] : user_blocks(script_text(base_master).text)) master_blocks[id] = text;

	// BMaster: the shipped template with this ladder's enum, and the lose branches.
	bool lose_branches = std::any_of(t.matches.begin(), t.matches.end(), [](const TMatch &m) { return m.on_lose != kReplay; });
	{
		de::PCode m = base_master;
		std::string members = "enum EMatch\n\t{\n";
		for (auto &x : t.matches) members += "\tmatch" + x.key + ",\n";
		members += "\tmatchCinematic,\n\tmatchEnd\n\t};\n\n\n";
		members += "final dword m_StartingMatch=" + (t.matches.empty() ? std::string("matchCinematic") : "match" + t.matches[0].key) + ";\n\n\n";
		members += "final float m_TournamentAIDifficulty=" + fmt_float(t.ai_difficulty) + ";\n";
		members += block(master_blocks, "Members");
		m.members.source = lines_dos(members);
		for (auto &e : m.elements) {
			if (e.link >= 0 || e.name != "StartNewMatch") continue;
			std::string s = lines_unix(e.source);
			const std::string marker = "/*PreDestroyLastSessionInsert*/";
			const std::string branch = "\nif (m_PlayerLost)\n\tm_MatchNumber=m_NextMatchNumber;  // lose branches: the sub-session set it\n";
			size_t at = s.find(marker);
			size_t had = s.find(branch);
			if (had != std::string::npos) s.erase(had, branch.size());
			if (lose_branches && at != std::string::npos) s.insert(at + marker.size(), branch);
			e.source = lines_dos(s);
		}
		std::vector<Guid> targets;
		BehaviorGlobals g = behavior_globals_of(p.file().objects[size_t(o.bmaster)]);
		for (size_t k = 0; k < g.names.size(); ++k) {
			Guid target = k < g.refs.size() ? p.target(g.refs[k]).guid : Guid{};
			if (g.names[k] == "g_GameDescription" && !guid_zero(t.game_type)) target = t.game_type;
			if (g.names[k] == "g_ScoreLimit" && !guid_zero(t.score_limit)) target = t.score_limit;
			if (g.names[k] == "g_EndingEnvironment" && !guid_zero(t.ending_arena)) target = t.ending_arena;
			targets.push_back(target);
		}
		m.global.globals = g.names;
		out.master = std::move(m);
		out.master_globals = std::move(targets);
	}

	// BSubSession: a state per match, SetupMatch, Cinematic, then the kept methods.
	Gen gen{p, visible_globals(p, o), {}, {}, {}};
	{
		BehaviorGlobals mg;
		mg.names = out.master.global.globals;
		for (size_t k = 0; k < mg.names.size(); ++k) gen.visible.add(mg.names[k], out.master_globals[k]);
	}
	de::PCode sub;
	sub.name = p.file().objects.at(size_t(o.bsub)).info.name_text();
	sub.members.source = lines_dos("instance CText m_Quote;\ninstance CText m_Pilot;\n" + block(blocks, "Members"));
	std::set<std::string> generated;
	auto add = [&](de::PCodeElement e) {
		sub.elements.push_back(std::move(e));
		generated.insert(sub.qualified_name(sub.elements.size() - 1));
		return int(sub.elements.size() - 1);
	};
	for (size_t mi = 0; mi < t.matches.size(); ++mi) {
		const TMatch &m = t.matches[mi];
		const std::string method = "Match" + m.key;
		std::string s = "exKillCallers();\nRegisterState();\n\n";
		if (!guid_zero(m.player_faction) || !guid_zero(m.enemy_faction)) {
			s += "SubSessionSetFactions(" + gen.global(m.player_faction, "Faction") + "," + gen.global(m.enemy_faction, "Faction") + ");\n\n";
		}
		for (size_t k = 0; k < m.opponents.size(); ++k) {
			const TOpponent &op = m.opponents[k];
			std::string robot = guid_zero(op.robot) ? "CreatureListGetRandom()" : gen.global(op.robot, "Fighter");
			std::string pilot = guid_zero(op.pilot) ? "PilotListPopRandom()" : gen.global(op.pilot, "Pilot");
			s += "SubSessionActivateSlot(" + std::to_string(k + 1) + "," + robot + "," + pilot;
			if (!guid_zero(op.faction)) s += "," + gen.global(op.faction, "Faction");
			s += ");\n";
		}
		s += "\n";
		s += guid_zero(m.arena) ? "SinglePlayerPickRandomEnvironment();\n\n" : "SubSessionPickEnvironment(" + gen.global(m.arena, "Environment") + ");\n\n";
		s += quote_lines(m.pre_quotes);
		if (!m.pre_quotes.empty()) s += "\n";
		if (!m.intro.empty()) s += "m_Quote=\"" + literal(m.intro) + "\";\nSetIntroMatchCommentary(m_Quote);\n\n";
		s += "m_Quote=\"" + literal(m.description) + "\";\nSetNextMatchDescription(m_Quote);\n\n";
		s += "m_Quote=\"" + literal(m.name) + "\";\nSetNextMatchName(m_Quote);\n\n";
		for (auto &d : m.drop_ons) {
			s += "AddDropActorOnSlotCreature(" + std::to_string(d.slot) + "," + gen.global(d.actor, "DropOn") + "," + fmt_float(d.delay) + ");\n";
		}
		if (!m.drop_ons.empty()) s += "\n";
		if (m.on_win != kNextMatch && m.on_win != int(mi) + 1) s += "SetNextMatch(" + match_target(t, m.on_win) + ");\n\n";
		s += block(blocks, method) + "\nexSleep();\n";
		int me = add(pcode_method(method, lines_dos(s), true));

		std::string c;
		bool commentary = !m.win_commentary.empty() || !m.lose_commentary.empty() || !m.win_quotes.empty() || !m.lose_quotes.empty();
		if (commentary || lose_branches) {
			c += "if (m_PlayerLost)\n\t{\n";
			if (!m.lose_commentary.empty()) c += "\tm_Quote=\"" + literal(m.lose_commentary) + "\";\n\tSetMatchCommentary(m_Quote);\n";
			c += quote_lines(m.lose_quotes, "\t");
			if (lose_branches) c += "\tSetNextMatch(" + (m.on_lose == kReplay ? std::string("m_MatchID") : match_target(t, m.on_lose)) + ");\n";
			c += "\t}\nelse\n\t{\n";
			if (!m.win_commentary.empty()) c += "\tm_Quote=\"" + literal(m.win_commentary) + "\";\n\tSetMatchCommentary(m_Quote);\n";
			c += quote_lines(m.win_quotes, "\t");
			if (lose_branches && m.on_win != kNextMatch && m.on_win != int(mi) + 1) c += "\tSetNextMatch(" + match_target(t, m.on_win) + ");\n";
			c += "\t}\n";
		}
		const std::string cid = method + ".MatchComplete";
		if (!c.empty() || blocks.count(cid)) add(pcode_method("MatchComplete", lines_dos(c + block(blocks, cid)), false, me, true));

		std::string ib;
		float level = t.ai_difficulty + float(m.ai_offset);
		if (level != 0) {
			for (size_t k = 0; k < m.opponents.size(); ++k) ib += "SlotPilot(" + std::to_string(k + 1) + ").SetAIDifficultyOffset(" + fmt_float(level) + ");\n";
		}
		const std::string iid = method + ".InitialBeginAction";
		if (!ib.empty() || blocks.count(iid)) add(pcode_method("InitialBeginAction", lines_dos(ib + block(blocks, iid)), false, me, true));
	}
	{
		std::string s = "//ERROR(\"Setup Match \",m_MatchID);\n\nBSinglePlayerSubSession::SetupMatch();\n\n";
		for (auto &m : t.matches) s += "if (m_MatchID==match" + m.key + ")\n\tMatch" + m.key + "();\n";
		s += "if (m_MatchID==matchCinematic)\n\tCinematic();\n";
		add(pcode_method("SetupMatch", lines_dos(s)));
	}
	{
		std::string s = "instance CText Pilot;\n\nexKillCallers();\nRegisterState();\n\n";
		s += "SubSessionPickEnvironment(g_EndingEnvironment);\n\n";
		s += "m_Quote=\"" + literal(t.ending_description) + "\";\nSetNextMatchDescription(m_Quote);\n\n";
		s += "m_Quote=\"" + literal(t.ending_name) + "\";\nSetNextMatchName(m_Quote);\n\n";
		for (auto &u : t.unlocks) {
			if (guid_zero(u.description)) continue;
			s += "m_Quote=\"" + literal(u.text) + "\";\nAddUnlock(" + gen.global(u.description, "Unlock") + ",m_Quote);\n\n";
		}
		for (auto &q : t.ending_quotes) s += "m_Quote=\"" + literal(q) + "\";\nAddCinematicQuote(m_Quote);\n\n";
		s += block(blocks, "Cinematic") + "\nexSleep();\n";
		add(pcode_method("Cinematic", lines_dos(s)));
	}
	std::vector<const TMethod *> extra;
	for (auto &e : t.extra) extra.push_back(&e);
	std::stable_sort(extra.begin(), extra.end(), [](const TMethod *a, const TMethod *b) {
		return std::count(a->name.begin(), a->name.end(), '.') < std::count(b->name.begin(), b->name.end(), '.');
	});
	for (const TMethod *e : extra) {
		if (generated.count(e->name)) continue;
		size_t dot = e->name.rfind('.');
		int parent = -1;
		if (dot != std::string::npos) {
			std::string up = e->name.substr(0, dot);
			for (size_t i = 0; i < sub.elements.size(); ++i) {
				if (sub.qualified_name(i) == up) parent = int(i);
			}
			if (parent < 0) continue;  // its match is gone
		}
		add(pcode_method(dot == std::string::npos ? e->name : e->name.substr(dot + 1), lines_dos(e->source), e->state, parent, e->virtual_));
	}
	sub.global.globals = gen.own_names;
	out.sub = std::move(sub);
	out.sub_globals = gen.own_targets;
	return out;
}

GeneratedScripts generate_scripts(const Project &p, const TournamentObjects &o, const Tournament &t) {
	return generate_scripts(p, o, t, nullptr);
}

namespace {

void write_all(Project &p, const TournamentObjects &o, const Tournament &t, const std::map<std::string, std::string> *blocks) {
	auto &objs = p.file().objects;
	if (o.description >= 0) set_description(objs[size_t(o.description)], t.name, t.listed, t.locked);
	if (o.text >= 0) set_string_value(objs[size_t(o.text)], t.choose_text);
	if (o.matches >= 0) {
		std::string list;
		for (auto &m : t.matches) list += (m.name.empty() ? m.key : m.name) + "\r\n";
		list += "Cinematic\r\n";
		set_string_value(objs[size_t(o.matches)], list);
	}
	set_object_reference_value(p, o.value(p, "ExcludePilots"), t.excluded_pilots);
	set_object_reference_value(p, o.value(p, "PlayerPilots"), t.player_pilots);
	set_object_reference_value(p, o.value(p, "PlayerCreatures"), t.player_robots);
	if (o.model >= 0) set_string_value(objs[size_t(o.model)], model_json(t));
	GeneratedScripts g = generate_scripts(p, o, t, blocks);
	write_behavior(p, uint32_t(o.bmaster), g.master, g.master_globals);
	write_behavior(p, uint32_t(o.bsub), g.sub, g.sub_globals);
	p.touch();
}

}  // namespace

void write_tournament(Project &p, const TournamentObjects &o, const Tournament &t, const std::map<std::string, std::string> *blocks) {
	if (o.bmaster < 0 || o.bsub < 0) throw deo::Error("the tournament's scripts are missing");
	write_all(p, o, t, blocks);
}

std::vector<std::pair<std::string, Guid>> drop_on_choices(const BaseData &base) {
	std::vector<std::pair<std::string, Guid>> out;
	std::set<std::string> seen;
	for (const BaseObject *d : base_tournaments(base)) {
		const deo::File &f = base.model(d->file);
		TournamentObjects s = find_structure(f, d->index);
		if (s.bsub < 0) continue;
		BehaviorGlobals g = behavior_globals_of(f.objects[size_t(s.bsub)]);
		for (auto &r : g.refs) {
			Guid t = ref_guid(f, r);
			const BaseObject *o = base.by_guid(t);
			if (!o || o->class_path.rfind("Actor", 0) != 0 || !seen.insert(guid_key(t)).second) continue;
			out.emplace_back(o->name, t);
		}
	}
	return out;
}

std::string without_user_blocks(const std::string &text) {
	std::string s = lines_unix(text), out;
	size_t at = 0;
	for (;;) {
		size_t open = s.find("// <user ", at);
		if (open == std::string::npos) break;
		size_t line_end = s.find('\n', open);
		size_t close = line_end == std::string::npos ? std::string::npos : s.find("// </user>", line_end);
		if (close == std::string::npos) break;
		out += s.substr(at, line_end + 1 - at);
		at = close;
	}
	return out + s.substr(at);
}

uint32_t create_tournament(Project &p, const BaseObject &base_description, const std::string &name, bool blank) {
	const deo::File &src = p.base().model(base_description.file);
	TournamentObjects s = find_structure(src, base_description.index);
	if (s.bmaster < 0 || s.bsub < 0 || s.sub_class < 0) throw deo::Error(base_description.name + " is not a tournament the editor can copy");
	std::set<uint32_t> roots;
	for (int i : {s.description, s.master, s.master_class, s.bmaster, s.handler_description, s.sub_master, s.sub_class, s.bsub, s.creator, s.icon}) {
		if (i >= 0) roots.insert(uint32_t(i));
	}
	for (int i : {s.master, s.sub_master}) {
		int exec = first_of_class(src, local_refs(src, i), "ClassExecutor");
		if (exec >= 0) roots.insert(uint32_t(exec));
	}
	std::function<void(int)> panel = [&](int i) {
		for (int k : local_refs(src, i)) {
			if (k < 0 || roots.count(uint32_t(k))) continue;
			roots.insert(uint32_t(k));
			if (cls(src, k) == "ControlCreator") panel(k);
		}
	};
	panel(s.creator);
	std::vector<uint32_t> list(roots.begin(), roots.end());
	std::map<uint32_t, uint32_t> copies;
	p.copy_from_base(base_description.file, list, &copies);

	// The folder holds the copies that no other copy holds.
	std::set<uint32_t> held;
	for (auto &[from, to] : copies) {
		for (uint32_t c : p.children(to)) held.insert(c);
	}
	uint32_t folder = add_folder(p, name);
	for (auto &[from, to] : copies) {
		if (!held.count(to)) add_child(p, folder, to);
	}
	uint32_t model = add_string_value(p, kModelName, "{}");
	add_child(p, folder, model);

	uint32_t description = copies.at(uint32_t(s.description));
	TournamentObjects o = tournament_objects(p, description);
	Imported im = import_scripts(p, o);
	Tournament t = read_tournament(p, o);
	t.matches = im.t.matches;
	t.extra = im.t.extra;
	t.ai_difficulty = im.t.ai_difficulty;
	t.ending_name = im.t.ending_name;
	t.ending_description = im.t.ending_description;
	t.ending_quotes = im.t.ending_quotes;
	t.unlocks = im.t.unlocks;
	t.name = name;
	t.listed = true, t.locked = false;
	std::map<std::string, std::string> blocks = im.blocks;
	if (blank) {
		TMatch m;
		m.key = "First";
		m.name = "First Match";
		m.description = "<Slot0Name> faces <Slot1Name> in <EnvironmentName>.";
		m.intro = "<Slot0Name> has entered the " + name + ".";
		m.opponents.resize(1);
		auto pilots = p.base().of_class("Description\\Pilot");
		for (auto *bp : pilots) {
			if (bp->name == "Lucas") m.opponents[0].pilot = bp->guid;
		}
		t.matches = {m};
		t.extra.clear();
		t.unlocks.clear();
		t.ending_quotes = {"<Slot0Name> has won the " + name + "!"};
		t.ending_name = "Champion";
		t.ending_description = "<Slot0Name> is the champion.";
		blocks.clear();
	}
	write_all(p, o, t, &blocks);
	return description;
}

std::vector<std::pair<std::string, Guid>> class_choices(const Project &p, const std::string &class_path) {
	std::vector<std::pair<std::string, Guid>> out;
	for (auto &o : p.file().objects) {
		if (o.info.class_text() == class_path) out.emplace_back(utf8(o.info.name) + "  (add-on)", o.info.guid);
	}
	std::vector<std::pair<std::string, Guid>> base;
	for (auto *b : p.base().of_class(class_path)) base.emplace_back(deo::latin1_to_utf8(Bytes(b->name.begin(), b->name.end())), b->guid);
	std::sort(base.begin(), base.end(), [](auto &a, auto &b) { return a.first < b.first; });
	out.insert(out.end(), base.begin(), base.end());
	return out;
}

std::string match_label(const Tournament &t, int target, bool lose) {
	if (lose && target == kReplay) return "Replay this match";
	if (!lose && target == kNextMatch) return "Next match";
	if (target >= kEnding || target >= int(t.matches.size())) return "Ending cinematic";
	const TMatch &m = t.matches[size_t(target)];
	return "M" + std::to_string(target + 1) + "  " + (m.name.empty() ? m.key : m.name);
}

}  // namespace ed
