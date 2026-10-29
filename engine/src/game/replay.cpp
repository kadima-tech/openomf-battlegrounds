#include "game/replay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>

#include "de/contents.h"
#include "game/runtime.h"
#include "game/session.h"
#include "game/simple_physics.h"
#include "vm/executor.h"

namespace game {

namespace {

uint16_t input_bits(const Input &i) {
	bool b[11] = {i.up, i.down, i.left, i.right, i.left_punch, i.right_punch, i.left_kick, i.right_kick, i.jump, i.evade, i.duck};
	uint16_t v = 0;
	for (int k = 0; k < 11; ++k) v |= uint16_t(b[k] ? 1 << k : 0);
	return v;
}
Input input_of(uint16_t v) {
	Input i;
	bool *b[11] = {&i.up, &i.down, &i.left, &i.right, &i.left_punch, &i.right_punch, &i.left_kick, &i.right_kick, &i.jump, &i.evade, &i.duck};
	for (int k = 0; k < 11; ++k) *b[k] = (v >> k) & 1;
	return i;
}

// DE_CStandardDataPacker: little-endian, unaligned.
struct Packer {
	std::vector<uint8_t> data;
	size_t pos = 0;
	void raw(const void *p, size_t n) { data.insert(data.end(), (const uint8_t *)p, (const uint8_t *)p + n); }
	void u8(uint8_t v) { raw(&v, 1); }
	void u16(uint16_t v) { raw(&v, 2); }
	void u32(uint32_t v) { raw(&v, 4); }
	void f32(float v) { raw(&v, 4); }
	void bytes(const std::vector<uint8_t> &b) {
		u32(uint32_t(b.size()));
		raw(b.data(), b.size());
	}
	void str(const std::string &s) {
		u32(uint32_t(s.size()));
		raw(s.data(), s.size());
	}
	bool take(void *p, size_t n) {
		if (pos + n > data.size()) {
			pos = data.size();
			memset(p, 0, n);
			return false;
		}
		memcpy(p, data.data() + pos, n);
		pos += n;
		return true;
	}
	uint8_t r8() { uint8_t v; take(&v, 1); return v; }
	uint16_t r16() { uint16_t v; take(&v, 2); return v; }
	uint32_t r32() { uint32_t v; take(&v, 4); return v; }
	float rf() { float v; take(&v, 4); return v; }
	std::vector<uint8_t> rbytes() {
		uint32_t n = r32();
		if (pos + n > data.size()) n = uint32_t(data.size() - pos);
		std::vector<uint8_t> b(data.begin() + long(pos), data.begin() + long(pos + n));
		pos += n;
		return b;
	}
	std::string rstr() {
		auto b = rbytes();
		return std::string(b.begin(), b.end());
	}
	bool done() const { return pos >= data.size(); }
};

struct StateManager {
	GameObject *owner = nullptr;
	std::string id;
	StateRecord *record = nullptr;
	bool replay = false;       // +0x78
	uint32_t applied = ~0u;    // +0x80
};

struct ReplayWorld {
	std::map<uint32_t, StateManager> managers;  // by owner handle
	std::shared_ptr<SessionRecord> record;      // the session being recorded or replayed
	SessionState *session = nullptr;
};

std::map<const World *, ReplayWorld> g_worlds;
ReplayWorld &rw(World &w) { return g_worlds[&w]; }

std::string path_of(const GameObject *o) {
	std::string p;
	for (; o; o = o->parent) p = o->name + (p.empty() ? "" : "\\" + p);
	return p;
}

bool has_state_manager(const GameObject &o) {
	return std::any_of(o.children.begin(), o.children.end(), [](const GameObject *c) { return c->is("StateManager"); });
}

// DE_CCreature +0x1d0 (ObjProcessContents, Creature 0x15014820): which parts make its ID; and
// +0x300 / +0x304, whether its states carry its position and orientation.
struct CreatureFlags {
	uint32_t id = 0x16;
	bool position = false, orientation = false;
};
CreatureFlags creature_flags(const GameObject &o) {
	CreatureFlags f;
	if (!o.file) return f;
	try {
		de::Contents c(o.file->object_data(o.file->objects()[o.index]).contents);
		c.seek(de::begin_chunk(c).end);
		de::Chunk ch = de::begin_chunk(c);
		if (ch.version < 2 || ch.version > 4) return f;
		c.u32(), c.u32(), c.u32();
		if (ch.version == 4) {
			f.id = c.u32();
		} else {
			f.id = 0;
			uint32_t bits[2][4] = {{2, 2, 4, 8}, {2, 4, 8, 0x10}};
			for (int k = 0; k < 4; ++k) f.id |= c.boolean() ? bits[ch.version - 2][k] : 0;
		}
		if (ch.version >= 3) {
			f.position = c.squeezed() == 1;
			f.orientation = c.squeezed() == 1;
		}
	} catch (const std::exception &) {
	}
	return f;
}

// DE_CStateManager::BuildID (Core 0x701b1e00). A creature's: DE_CCreature::BuildID (Creature
// 0x15014c20), its component owner's first, then its path or name, its GUID and creation time as
// its flags say. Session nodes: their path, with the ordinal of that path among the nodes seen.
std::string build_id(World &w, GameObject &o) {
	if (o.is("Actor\\Creature")) {
		std::string id;
		if (o.component_owner && o.component_owner != &o) id = build_id(w, *o.component_owner);
		CreatureFlags f = creature_flags(o);
		if (f.id & 0x40) id += path_of(&o) + '\0';
		else if (f.id & 8) id += o.name + '\0';
		if ((f.id & 4) && o.file) id.append(reinterpret_cast<const char *>(o.file->objects()[o.index].guid.bytes), 16);
		if (f.id & 2) id.append(reinterpret_cast<const char *>(&o.creation_time), 4);
		return id;
	}
	// Relative to the session (a sub-session's object is named after its game type); each level
	// numbered among its same-named siblings.
	std::string p;
	for (const GameObject *a = &o; a && a->parent; a = a->parent) {
		int k = 0;
		for (const GameObject *sib : a->parent->children) {
			if (sib == a) break;
			k += sib->name == a->name;
		}
		p = a->name + '#' + std::to_string(k) + (p.empty() ? "" : "\\" + p);
	}
	return p;
}

StateManager *manager_of(World &w, const GameObject *o) {
	if (!o) return nullptr;
	auto &m = rw(w).managers;
	auto it = m.find(o->handle);
	return it == m.end() || it->second.owner != o ? nullptr : &it->second;
}

// DE_CSession::Add + DE_CSessionRecord::Add (Core 0x701f9520 / 0x70203610).
StateManager &join(World &w, GameObject &o) {
	auto &R = rw(w);
	StateManager &sm = R.managers[o.handle];
	if (sm.owner == &o) return sm;
	sm = StateManager{};
	sm.owner = &o;
	sm.id = build_id(w, o);
	if (auto &rec = R.record) {
		sm.record = rec->find(sm.id);
		if (!sm.record && rec->recording && o.class_path != "StateManager") {
			sm.record = rec->managers.emplace_back(std::make_unique<StateRecord>()).get();
			sm.record->id = sm.id;
		}
		sm.replay = sm.record && !rec->recording;
	}
	if (getenv("OPENBG_DEBUG_REPLAY")) {
		std::string shown;
		for (char ch : sm.id) shown += ch >= 32 && ch < 127 ? ch : '.';
		fprintf(stderr, "[replay %u] join %s (%s): %s\n", w.frame(), o.name.c_str(), shown.c_str(), sm.record ? (sm.replay ? "replayed" : "recorded") : "-");
	}
	return sm;
}

// DE_CRecordPacker::AddElementID / DE_CSession::AddObjectByGlobalID (Core 0x702049b0 / 0x701f9a90):
// 2 null, 1 the ID of the manager its object owns, 0 an object no manager owns.
void add_element(World &w, Packer &p, uint32_t handle) {
	GameObject *o = handle ? w.object(handle) : nullptr;
	if (!o) {
		p.u8(2);
		return;
	}
	if (StateManager *sm = manager_of(w, o)) {
		p.u8(1);
		p.str(sm->id);
		return;
	}
	p.u8(0);
}
uint32_t extract_element(World &w, Packer &p) {
	uint8_t kind = p.r8();
	if (kind != 1) return 0;
	std::string id = p.rstr();
	for (auto &[h, sm] : rw(w).managers) {
		if (sm.id == id && w.object(h) == sm.owner) return h;
	}
	return 0;
}

// Type 2 replicated values: the value class's own serialisation (its type object's +0xdc).
void add_value(World &w, Packer &p, const std::string &cls, uint32_t inst) {
	vm::Memory &m = w.memory();
	if (!inst) {
		p.u32(0);
		return;
	}
	Packer v;
	if (cls.find("CText") != std::string::npos || cls.find("CString") != std::string::npos) {
		uint32_t s = m.get<uint32_t>(inst);
		v.str(s ? std::string(reinterpret_cast<const char *>(m.ptr(s))) : std::string());
	} else if (cls.find("CGUIDPool") != std::string::npos) {
		uint32_t data = m.get<uint32_t>(inst), n = m.get<uint32_t>(inst + 4);
		v.u32(n);
		for (uint32_t i = 0; i < n && data; ++i) v.raw(m.ptr(data + i * 16, 16), 16);
	} else if (cls.find("CGUID") != std::string::npos) {
		v.raw(m.ptr(inst, 16), 16);
	} else if (cls.find("CVector") != std::string::npos) {
		v.raw(m.ptr(inst, 12), 12);
	} else {
		v.raw(m.ptr(inst, 48), 48);
	}
	p.bytes(v.data);
}
void extract_value(World &w, Packer &p, const std::string &cls, uint32_t inst) {
	Packer v;
	v.data = p.rbytes();
	vm::Memory &m = w.memory();
	if (!inst || v.data.empty()) return;
	if (cls.find("CText") != std::string::npos || cls.find("CString") != std::string::npos) {
		m.set<uint32_t>(inst, w.string(v.rstr()));
	} else if (cls.find("CGUIDPool") != std::string::npos) {
		uint32_t n = v.r32(), data = m.alloc(std::max<uint32_t>(1, n) * 16);
		for (uint32_t i = 0; i < n; ++i) v.take(m.ptr(data + i * 16, 16), 16);
		m.set<uint32_t>(inst, data);
		m.set<uint32_t>(inst + 4, n);
	} else {
		v.take(m.ptr(inst, uint32_t(v.data.size())), v.data.size());
	}
}

using Replicated = vm::CompiledMethod::Replicated;
const std::string &class_name(const vm::ScriptClass &cls, uint32_t index) {
	static const std::string none;
	return index < cls.code.classes.size() ? cls.code.classes[index].name : none;
}

// AddReplicatedMemberVariables / AddReplicatedVariables (Core 0x7013e680 / 0x7013cf40): 0 raw bytes,
// 2 a value instance, 3 / 4 a reference (a frame's local reference / the instance's reference
// member).
void add_variables(World &w, Packer &p, const vm::ScriptClass &cls, const std::vector<Replicated> &list, uint32_t base,
                   const std::vector<uint32_t> &refs) {
	vm::Memory &m = w.memory();
	for (const Replicated &r : list) {
		if (r.type == 0) p.raw(m.ptr(base + r.a, r.b ? r.b : 1), r.b);
		else if (r.type == 2) add_value(w, p, class_name(cls, r.b), m.get<uint32_t>(base + r.a));
		else if (r.type == 3 || r.type == 4) add_element(w, p, r.b < refs.size() ? refs[r.b] : 0);
	}
}
void extract_variables(World &w, Packer &p, const vm::ScriptClass &cls, const std::vector<Replicated> &list, uint32_t base,
                       std::vector<uint32_t> &refs) {
	vm::Memory &m = w.memory();
	for (const Replicated &r : list) {
		if (r.type == 0) {
			p.take(m.ptr(base + r.a, r.b ? r.b : 1), r.b);
		} else if (r.type == 2) {
			extract_value(w, p, class_name(cls, r.b), m.get<uint32_t>(base + r.a));
		} else if (r.type == 3 || r.type == 4) {
			uint32_t h = extract_element(w, p);
			if (refs.size() <= r.b) refs.resize(r.b + 1, 0);
			refs[r.b] = h;
		}
	}
}

// GenerateStatus (Core 0x70145930): the replicated members, then per frame a flag byte (bit 0 more
// frames, bits 1-2 the method index width, bit 3 the IP width; 6 alone: no frames), the method
// index, the IP offset and the frame's replicated locals.
void generate_status(World &w, Packer &p, Brain &b) {
	for (const auto &bc : b.cls.code.behaviors) add_variables(w, p, b.cls, bc.replicated, b.inst.members, b.inst.ref_members);
	auto frames = b.exec ? b.exec->status() : std::vector<vm::Executor::StatusFrame>{};
	if (frames.empty()) {
		p.u8(6);
		return;
	}
	for (size_t i = 0; i < frames.size(); ++i) {
		const auto &f = frames[i];
		uint32_t mi = f.method->index, ip = f.ip;
		uint8_t flag = i + 1 < frames.size() ? 1 : 0;
		flag |= mi < 0x100 ? 0 : mi < 0x10000 ? 2 : 4;
		flag |= ip < 0x100 ? 0 : 8;
		p.u8(flag);
		if (mi < 0x100) p.u8(uint8_t(mi));
		else if (mi < 0x10000) p.u16(uint16_t(mi));
		else p.u32(mi);
		if (ip < 0x100) p.u8(uint8_t(ip));
		else p.u16(uint16_t(ip));
		add_variables(w, p, b.cls, f.method->cm->replicated, f.addr, *f.local_refs);
	}
}

// ProcessStatus (Core 0x70145b50).
void process_status(World &w, Packer &p, Brain &b) {
	for (const auto &bc : b.cls.code.behaviors) extract_variables(w, p, b.cls, bc.replicated, b.inst.members, b.inst.ref_members);
	std::vector<vm::Executor::RestoreFrame> frames;
	std::vector<size_t> local_at;  // where each frame's locals start
	for (;;) {
		uint8_t flag = p.r8();
		uint32_t width = (flag >> 1) & 3;
		if (width == 3) return;
		uint32_t mi = width == 0 ? p.r8() : width == 1 ? p.r16() : p.r32();
		uint32_t ip = (flag & 8) ? p.r16() : p.r8();
		vm::ScriptMethod *m = b.cls.method(mi);
		if (!m) return;
		frames.push_back({m, ip});
		local_at.push_back(p.pos);
		// The locals go into the rebuilt frame (below), which does not exist yet.
		for (const Replicated &r : m->cm->replicated) {
			if (r.type == 0) p.pos += r.b;
			else if (r.type == 2) p.rbytes();
			else if ((r.type == 3 || r.type == 4) && p.r8() == 1) p.rstr();
		}
		if (!(flag & 1)) break;
	}
	if (!b.exec) return;
	b.inst.sleep = 0;  // ProcessStatus clears +0x104
	size_t k = 0;
	size_t end = p.pos;
	b.exec->restore(frames, [&](vm::ScriptMethod &m, uint32_t addr, std::vector<uint32_t> &refs) {
		p.pos = local_at[k++];
		extract_variables(w, p, b.cls, m.cm->replicated, addr, refs);
	});
	p.pos = end;
}

std::vector<Brain *> brains_of(World &w, const GameObject &o) {
	std::vector<Brain *> out;
	for (Brain *b : w.brains()) {
		if (b->owner == &o) out.push_back(b);
	}
	return out;
}

// DE_CObject::PackState (Core 0x7003de40) / DE_CCreature::PackState (Creature 0x150150b0): each
// executor instance's status; for a creature then its position and orientation (as its flags say)
// and its physics.
void pack_state(World &w, Packer &p, GameObject &o) {
	auto brains = brains_of(w, o);
	p.u8(uint8_t(brains.size()));
	for (Brain *b : brains) generate_status(w, p, *b);
	if (!o.is("Actor\\Creature")) return;
	CreatureFlags f = creature_flags(o);
	if (f.position) p.raw(o.local.t.data(), 12);
	if (f.orientation) p.raw(o.local.r.data(), 36);
	// The physics' own state: ours, not DE_CFighterPhysics::PackState's layout (FINDINGS 137).
	p.u8(o.fighter ? 1 : 0);
	if (o.fighter) {
		p.raw(o.local.t.data(), 12);
		p.raw(o.local.r.data(), 36);
		p.raw(o.momentum.data(), 12);
		p.f32(o.turn_rate);
		p.f32(o.forward_speed);
		p.u8(o.on_floor);
		p.u16(input_bits(o.user_controlled ? w.input : o.ai_input));
	}
}

// DE_CCreature::UnpackState (Creature 0x15015160).
void unpack_state(World &w, Packer &p, GameObject &o) {
	auto brains = brains_of(w, o);
	uint8_t n = p.r8();
	for (uint8_t i = 0; i < n; ++i) {
		if (i >= brains.size()) return;
		process_status(w, p, *brains[i]);
		// As a state switch settles at once (runtime.cpp settle): a session node's brain is only
		// pulsed by its hooks, so its restored state runs now; creatures are ticked anyway.
		Brain &b = *brains[i];
		if (!o.is("Actor\\Creature") && b.exec && b.exec->running() && !b.exec->executing() && !b.inst.shut_down) {
			try {
				b.exec->pulse();
			} catch (const std::exception &e) {
				w.log.push_back(o.name + ": " + e.what());
				b.inst.shut_down = true;
			}
		}
	}
	if (!o.is("Actor\\Creature")) return;
	CreatureFlags f = creature_flags(o);
	if (f.position) p.take(o.local.t.data(), 12);
	if (f.orientation) p.take(o.local.r.data(), 36);
	if (p.r8() && o.fighter) {
		p.take(o.local.t.data(), 12);
		p.take(o.local.r.data(), 36);
		p.take(o.momentum.data(), 12);
		o.turn_rate = p.rf();
		o.forward_speed = p.rf();
		o.on_floor = p.r8() != 0;
		// The controller's defaults, which a fighter with no controlling client reads.
		o.ai_input = input_of(p.r16());
	}
}

// The recordings' file: our own container for DE_CSessionRecord's contents (the original saves a
// .DEObjects file with the record as its master object).
constexpr char kMagic[8] = {'O', 'B', 'G', 'R', 'E', 'C', '0', '1'};

bool write_record(const SessionRecord &r, const std::string &path) {
	Packer p;
	p.raw(kMagic, 8);
	p.str(r.name);
	p.raw(r.master.bytes, 16);
	p.u32(uint32_t(r.modifiers.size()));
	for (auto &g : r.modifiers) p.raw(g.bytes, 16);
	p.u32(r.end_ms);
	int64_t d = r.date;
	p.raw(&d, 8);
	p.f32(r.length);
	p.str(r.player);
	p.str(r.server);
	p.u32(uint32_t(r.managers.size()));
	for (auto &m : r.managers) {
		p.str(m->id);
		p.u32(uint32_t(m->states.size()));
		for (auto &[t, data] : m->states) {
			p.u32(t);
			p.bytes(data);
		}
	}
	std::ofstream f(path, std::ios::binary);
	f.write(reinterpret_cast<const char *>(p.data.data()), long(p.data.size()));
	return bool(f);
}

std::shared_ptr<SessionRecord> read_record(const std::string &path, bool states) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return nullptr;
	Packer p;
	p.data.assign(std::istreambuf_iterator<char>(f), {});
	char magic[8];
	if (!p.take(magic, 8) || memcmp(magic, kMagic, 8) != 0) return nullptr;
	auto r = std::make_shared<SessionRecord>();
	r->path = path;
	r->name = p.rstr();
	p.take(r->master.bytes, 16);
	uint32_t nm = p.r32();
	for (uint32_t i = 0; i < nm && !p.done(); ++i) p.take(r->modifiers.emplace_back().bytes, 16);
	r->end_ms = p.r32();
	p.take(&r->date, 8);
	r->length = p.rf();
	r->player = p.rstr();
	r->server = p.rstr();
	if (!states) return r;
	uint32_t n = p.r32();
	for (uint32_t i = 0; i < n && !p.done(); ++i) {
		auto &m = *r->managers.emplace_back(std::make_unique<StateRecord>());
		m.id = p.rstr();
		uint32_t k = p.r32();
		for (uint32_t j = 0; j < k && !p.done(); ++j) {
			uint32_t t = p.r32();
			m.states.emplace_back(t, p.rbytes());
		}
	}
	return r;
}

void forget(World &w) {
	auto &R = rw(w);
	R.managers.clear();
	R.record.reset();
	R.session = nullptr;
}

}  // namespace

StateRecord *SessionRecord::find(const std::string &id) const {
	for (auto &m : managers) {
		if (m->id == id) return m.get();
	}
	return nullptr;
}

std::string recordings_dir() {
	const char *home = getenv("HOME");
	return std::string(home ? home : ".") + "/Library/Application Support/openbg/Recordings";
}

std::vector<std::shared_ptr<SessionRecord>> list_recordings() {
	std::vector<std::shared_ptr<SessionRecord>> out;
	std::error_code ec;
	for (auto &e : std::filesystem::directory_iterator(recordings_dir(), ec)) {
		if (e.path().extension() != ".obgrec") continue;
		if (auto r = read_record(e.path().string(), false)) out.push_back(r);
	}
	std::sort(out.begin(), out.end(), [](auto &a, auto &b) { return a->date > b->date; });
	return out;
}

std::shared_ptr<SessionRecord> load_recording(const std::string &path) { return read_record(path, true); }

bool delete_recording(const SessionRecord &r) {
	std::error_code ec;
	return std::filesystem::remove(r.path, ec);
}

bool rename_recording(SessionRecord &r, const std::string &name) {
	auto full = load_recording(r.path);
	if (!full) return false;
	full->name = name;
	if (!write_record(*full, r.path)) return false;
	r.name = name;
	return true;
}

bool recordable(const de::ObjectRef &d) {
	if (!d) return false;
	try {
		de::Contents c(d.file->object_data(d.info()).contents);
		c.seek(de::begin_chunk(c).end);
		de::Chunk ch = de::begin_chunk(c);
		return ch.version == 1 && c.boolean();
	} catch (const std::exception &) {
		return false;
	}
}

void replay_session_begun(World &w, SessionState &s) {
	auto &R = rw(w);
	if (s.replaying || R.record) return;
	if (w.registry_string("Gamepley\\Record Session") != "1") return;
	de::ObjectRef d = s.master_description && s.master_description->file
	                      ? de::ObjectRef{s.master_description->file, s.master_description->index}
	                      : de::ObjectRef{};
	if (!recordable(d)) return;
	auto rec = std::make_shared<SessionRecord>();
	rec->recording = true;
	rec->master = d.info().guid;
	rec->modifiers = s.attached_guids;
	R.record = rec;
	R.session = &s;
	s.record = rec;
	for (auto &[h, sm] : R.managers) sm = StateManager{};
	R.managers.clear();
	printf("recording %s\n", d.info().name.c_str());
}

// DE_CSessionRecord::Close (Core 0x70203810).
void replay_session_ended(World &w, SessionState &s) {
	auto &R = rw(w);
	if (R.session != &s) return;
	if (R.record && R.record->recording) {
		SessionRecord &r = *R.record;
		r.end_ms = uint32_t(w.scene_ms());
		r.length = float(r.end_ms) / 1000.0f;
		r.date = int64_t(time(nullptr));
		r.server = w.registry_string("Network\\Server Name");
		if (r.server.empty()) r.server = "Server";
		r.name = r.server;
		r.player = w.registry_string("Network\\Player Name");
		// "Session Recording, %a %B %d %Y, %I-%M-%S%p"
		char stamp[96];
		time_t now = time_t(r.date);
		strftime(stamp, sizeof stamp, "Session Recording, %a %B %d %Y, %I-%M-%S%p", localtime(&now));
		std::error_code ec;
		std::filesystem::create_directories(recordings_dir(), ec);
		r.path = recordings_dir() + "/" + stamp + ".obgrec";
		size_t states = 0;
		for (auto &m : r.managers) states += m->states.size();
		if (write_record(r, r.path)) printf("recorded %s: %zu managers, %zu states\n", r.path.c_str(), r.managers.size(), states);
	}
	s.record.reset();
	forget(w);
}

bool start_replay(World &w, const std::shared_ptr<SessionRecord> &rec) {
	if (!rec) return false;
	de::ObjectRef desc = w.data().by_guid(rec->master);
	if (!desc) return false;
	auto res = passive_references(w.data(), desc);
	if (res.empty()) return false;
	forget(w);
	auto &R = rw(w);
	rec->recording = false;
	for (auto &m : rec->managers) m->cursor = 0;
	R.record = rec;
	auto &ss = start_session(w, {res[0]});
	ss.master_description = w.global_object(desc);
	ss.attached_guids = rec->modifiers;
	ss.single_player = false;
	ss.max_player_clients = 4;
	ss.replaying = true;
	ss.record = rec;
	R.session = &ss;
	printf("replaying %s (%s)\n", desc.info().name.c_str(), rec->name.c_str());
	begin_session(w);
	return true;
}

void replay_update(World &w) {
	auto &R = rw(w);
	if (!R.record) return;
	if (!R.session || std::none_of(w.sessions.begin(), w.sessions.end(), [&](auto &s) { return s.get() == R.session && !s->ended; })) {
		forget(w);
		return;
	}
	for (auto it = R.managers.begin(); it != R.managers.end();) {
		if (w.object(it->first) != it->second.owner) it = R.managers.erase(it);
		else ++it;
	}
	// The scene's objects and the sessions' own (their modifiers and slots are not in the scene).
	auto consider = [&](GameObject *o) {
		if (!o->is("StateManager") && !in_gui(*o) && has_state_manager(*o)) join(w, *o);
	};
	for (GameObject *o : w.scene_objects()) consider(o);
	// Pilots and other objects that run scripts outside the scene.
	for (Brain *b : w.brains()) {
		if (b->owner && w.object(b->owner->handle) == b->owner) consider(b->owner);
	}
	std::function<void(GameObject *)> walk = [&](GameObject *o) {
		consider(o);
		for (GameObject *c : o->children) walk(c);
	};
	for (auto &s : w.sessions) {
		if (s->obj && w.object(s->obj->handle)) walk(s->obj);
	}
	uint32_t now = uint32_t(w.scene_ms());
	SessionRecord &rec = *R.record;
	if (rec.recording) {
		rec.end_ms = now;
		return;
	}
	// DE_CSession::Update while replaying: past the recorded end the session goes.
	if (rec.end_ms < now) {
		printf("replay ended\n");
		destroy_all_sessions(w);
		forget(w);
		return;
	}
	// UpdateForReplay (Core 0x701af350) + SetupOwnerForState: the newest state at or before now.
	for (auto &[h, sm] : R.managers) {
		if (!sm.replay || !sm.record || sm.record->states.empty()) continue;
		auto &st = sm.record->states;
		size_t &c = sm.record->cursor;
		if (c >= st.size()) c = st.size() - 1;
		while (c > 0 && st[c].first > now) --c;
		while (c + 1 < st.size() && st[c + 1].first <= now) ++c;
		if (st[c].first > now || st[c].first == sm.applied) continue;
		sm.applied = st[c].first;
		if (getenv("OPENBG_DEBUG_REPLAY")) fprintf(stderr, "[replay %u] %s takes its %u ms state\n", w.frame(), sm.owner->name.c_str(), st[c].first);
		Packer p;
		p.data = st[c].second;
		unpack_state(w, p, *sm.owner);
	}
}

void register_state(World &w, GameObject *owner) {
	auto &R = rw(w);
	if (!owner || !R.record || !R.record->recording) return;
	// The state manager of the object (or of the creature it is a component of).
	GameObject *o = owner;
	while (o && !has_state_manager(*o)) o = o->component_owner && o->component_owner != o ? o->component_owner : o->parent;
	if (!o || in_gui(*o)) return;
	StateManager &sm = join(w, *o);
	if (!sm.record) return;
	uint32_t t = uint32_t(w.scene_ms());
	Packer p;
	pack_state(w, p, *o);
	auto &st = sm.record->states;
	// AddToStateList / Record: a state at the same time replaces the last.
	if (!st.empty() && st.back().first == t) st.back().second = std::move(p.data);
	else st.emplace_back(t, std::move(p.data));
}

bool replay_driven(World &w, const GameObject *o) {
	auto &R = rw(w);
	if (!R.record || R.record->recording) return false;
	for (; o; o = o->component_owner && o->component_owner != o ? o->component_owner : o->parent) {
		if (StateManager *sm = manager_of(w, o)) return sm->replay;
	}
	return false;
}

bool replaying(World &w) {
	auto &R = rw(w);
	return R.record && !R.record->recording;
}

uint32_t replay_end_ms(World &w) {
	auto &R = rw(w);
	return replaying(w) ? R.record->end_ms : 0;
}

void forget_replay_objects(World &w) { forget(w); }

}  // namespace game
