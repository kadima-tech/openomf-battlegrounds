#include "game/replay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>

#include "de/contents.h"
#include "game/runtime.h"
#include "game/session.h"
#include "game/simple_physics.h"
#include "vm/executor.h"

namespace game {

namespace {

// DE_CStandardDataPacker: little-endian, unaligned.
struct Packer {
	std::vector<uint8_t> data;
	size_t pos = 0;
	// DE_CLocalExecutorPacker: references as the object's handle, repacked when recorded.
	bool local = false;
	std::vector<std::pair<size_t, uint32_t>> refs;
	void raw(const void *p, size_t n) { data.insert(data.end(), (const uint8_t *)p, (const uint8_t *)p + n); }
	void u8(uint8_t v) { raw(&v, 1); }
	void u16(uint16_t v) { raw(&v, 2); }
	void u32(uint32_t v) { raw(&v, 4); }
	void f32(float v) { raw(&v, 4); }
	void bytes(const std::vector<uint8_t> &b) {
		u32(uint32_t(b.size()));
		raw(b.data(), b.size());
	}
	// AddSqueezed (Core 0x70019100).
	void squeezed(uint32_t v) {
		if (v < 0xFE) {
			u8(uint8_t(v));
		} else if (v < 0xFFFF) {
			u8(0xFE);
			u16(uint16_t(v));
		} else {
			u8(0xFF);
			u32(v);
		}
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
	uint32_t rsqueezed() {
		uint8_t b = r8();
		return b < 0xFE ? b : b == 0xFE ? r16() : r32();
	}
	std::vector<uint8_t> rn(size_t n) {
		n = std::min(n, data.size() - pos);
		std::vector<uint8_t> b(data.begin() + long(pos), data.begin() + long(pos + n));
		pos += n;
		return b;
	}
	std::vector<uint8_t> rbytes() { return rn(r32()); }
	// A versioned chunk's end (u8 size, or 0xFF + i32).
	size_t chunk_end() {
		r8();
		uint8_t s = r8();
		size_t n = s == 0xFF ? r32() : s;
		return std::min(pos + n, data.size());
	}
};

// A state in a manager's history (DE_CStateManager::CState): packed with local references; the
// fighter physics' part is repacked through a scratch physics when recorded.
struct LocalState {
	uint32_t time = 0;
	std::vector<uint8_t> bytes;
	std::vector<std::pair<size_t, uint32_t>> refs;
	size_t phys_at = 0, phys_end = 0;
};
constexpr size_t kLocalRef = 7;  // v1 chunk, size 5, kind 3, u32 handle

struct StateManager {
	GameObject *owner = nullptr;
	std::string id;
	StateRecord *record = nullptr;
	bool replay = false;       // +0x78
	uint32_t applied = ~0u;    // +0x80
	uint64_t joined = 0;       // the session's managers are searched newest first
	bool got_hit = false;      // fighter +0x4a0: RegisterGetHitState since its last state
	uint32_t window = 1000;    // CleanOldStates: 1000 / +0xb8 (a tick-based creature's rate)
	uint32_t adds = 0;         // +0x64
	bool keep_clock = false;   // +0xd0
	bool connected = true;     // in the session's connector (ObjPrepare)
	bool local = false;        // kept for its own states only, outside any session
	std::vector<LocalState> history;                               // +0x3c, the owner's own states
	std::vector<std::pair<uint32_t, std::vector<uint8_t>>> lagged;  // +0x3c, while replaying
};

// A tick-based creature's clock in a replay (DE_CCreature +0x1d4 is GameObject::creature_ticks).
struct Clock {
	// +0x1d8: tick k runs at base + (k + 1) x rate; ours follows the ticks the creature ran (none
	// while out of the scene), which its recorded times count.
	uint32_t base = 0;
	int mode = 0;            // +0x2fc: 2 held since the clock went back
	uint32_t until = 0;      // +0x1e4
	uint32_t saved = 0;      // +0x1e8: the tick it was at when the hold began
	uint32_t unpacking = 0;  // +0x214: the state being restored
	bool ticked = false;     // +0x20c
};

struct ReplayWorld {
	std::map<uint32_t, StateManager> managers;  // by owner handle
	std::shared_ptr<SessionRecord> record;      // the session being recorded or replaying
	SessionState *session = nullptr;
	uint64_t joins = 0;
	uint32_t last_ms = 0;
	std::map<uint32_t, uint32_t> prepared;  // "ScenePrepareTime" of the actors met while replaying
	std::map<const StateRecord *, std::vector<uint32_t>> joined;  // DE_CStateManagerRecord +0x00
	std::map<uint32_t, Clock> clocks;  // by creature handle
	std::vector<uint32_t> deferred;    // DE_CScene +0x98: creatures whose SetTime waits
	int reticking = 0;
};

std::map<const World *, ReplayWorld> g_worlds;
ReplayWorld &rw(World &w) { return g_worlds[&w]; }
// DE_CCreature +0x1ec: SetInstanceNumber's suffix, by handle.
std::map<uint32_t, std::pair<const GameObject *, std::string>> g_instance_numbers;

GameObject *state_manager_object(const GameObject &o) {
	for (GameObject *c : o.children) {
		if (c->is("StateManager")) return c;
	}
	return nullptr;
}
bool has_state_manager(const GameObject &o) { return state_manager_object(o) != nullptr; }

// DE_CStateManager contents (ObjProcessContents 0x701b1600; v2: its name, then the BOOLs +0x84,
// +0x88, +0x8c, +0xbc, +0xc0, +0xa8 and +0xd0, defaults 0, 1, 1, 0, 1, 0, 0): the session takes a
// manager with any of the first three set (DE_CStateManager::ObjPrepare 0x701b1920), and its
// record only with +0x8c.
struct ManagerFlags {
	bool joins = true, record = true, keep_clock = false;  // keep_clock: +0xd0
};
ManagerFlags manager_flags(const GameObject &o) {
	bool up = false, down = true, record = true, d0 = false;
	GameObject *m = state_manager_object(o);
	if (m && m->file) {
		try {
			de::Contents c(m->file->object_data(m->file->objects()[m->index]).contents);
			de::Chunk ch = de::begin_chunk(c);
			if (ch.version == 1 || ch.version == 2) {
				c.block();
				up = c.boolean();
				down = c.boolean();
				if (ch.version == 2) {
					record = c.boolean();
					c.boolean(), c.boolean(), c.boolean();
					d0 = c.boolean();
				}
			}
		} catch (const std::exception &) {
		}
	}
	return {up || down || record, record, d0};
}

// DE_CCreature::LocalTime (Creature 0x15008300): a tick-based creature's ticks, else the scene's.
uint32_t local_time(World &w, const GameObject &o) {
	if (o.is("Actor\\Creature") && w.creature_rate(const_cast<GameObject *>(&o))) return o.creature_ticks;
	return uint32_t(w.scene_ms());
}

// DE_CObject::GUID (Core 0x7003df30): its data object's, which copies share; none for others.
de::Guid guid_of(const GameObject &o) { return o.file ? o.file->objects()[o.index].guid : de::Guid{}; }

// DE_CGUID::Get (Core 0x7001eee0): the 16 bytes in memory order, grouped 4-2-2-8.
std::string guid_text(const de::Guid &g) {
	std::string s;
	char hex[3];
	for (int i = 0; i < 16; ++i) {
		if (i == 4 || i == 6 || i == 8) s += '-';
		snprintf(hex, sizeof hex, "%02x", g.bytes[i]);
		s += hex;
	}
	return s;
}

// DE_CCreature +0x1d0 (ObjProcessContents, Creature 0x15014820): which parts make its ID; and
// +0x300 / +0x304, whether its states carry its position and orientation.
struct CreatureFlags {
	uint32_t id = 0x16;
	bool position = false, orientation = false;
	uint32_t defer = 0;  // +0x1e0: a restored state's SetTime waits for the next scene update
};
CreatureFlags creature_flags(const GameObject &o) {
	CreatureFlags f;
	if (!o.file) return f;
	try {
		de::Contents c(o.file->object_data(o.file->objects()[o.index]).contents);
		c.seek(de::begin_chunk(c).end);
		de::Chunk ch = de::begin_chunk(c);
		if (ch.version < 2 || ch.version > 4) return f;
		c.u32();
		f.defer = c.u32();
		c.u32();
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

// DE_CObject::ObjPath (Core 0x7003a880): the names from the root down, "\" between; a scene's
// root actor's parent is the scene, named "{No Name}" (DE_CScene::ObjInitContents 0x7006e8d0).
std::string path_of(World &w, const GameObject *o) {
	std::string p;
	const GameObject *root = o;
	for (; o; root = o, o = o->parent) p = o->name + (p.empty() ? "" : "\\" + p);
	if (root && !root->gui && std::find(w.scene_roots.begin(), w.scene_roots.end(), root) != w.scene_roots.end())
		p = "{No Name}\\" + p;
	return p;
}

// The index of the slot holding `o` in its session's slot list (DE_CStandardSessionSlot::SlotID).
int slot_index(World &w, const GameObject &o) {
	for (auto &s : w.sessions) {
		for (size_t i = 0; i < s->slots.size(); ++i) {
			for (const GameObject *a = &o; a; a = a->parent) {
				if (a == s->slots[i]->obj) return int(i);
			}
		}
	}
	return -1;
}

// DE_CStateManager::BuildID (Core 0x701b1e00): the owner's BuildID event. A creature
// (Creature 0x15014c20): its component owner's ID, its path (0x40) or name (8), its instance
// suffix, GUID (4) and creation time (2). A session modifier (Core 0x70200c80): its name and GUID
// text. A slot modifier (Session 0x4501ef50): its class path, GUID text, " Slot" and the slot's
// index. A session (Core 0x701f88a0): its class path. Anything else: empty.
std::string build_id(World &w, GameObject &o) {
	if (o.is("Actor\\Creature")) {
		std::string id;
		if (o.component_owner && o.component_owner != &o) id = build_id(w, *o.component_owner);
		CreatureFlags f = creature_flags(o);
		if (f.id & 0x40) id += path_of(w, &o) + '\0';
		else if (f.id & 8) id += o.name + '\0';
		auto it = g_instance_numbers.find(o.handle);
		if (it != g_instance_numbers.end() && it->second.first == &o) id += it->second.second;
		if (f.id & 4) id.append(reinterpret_cast<const char *>(guid_of(o).bytes), 16);
		if (f.id & 2) id.append(reinterpret_cast<const char *>(&o.creation_time), 4);
		return id;
	}
	if (o.class_path.rfind("SessionModifier", 0) == 0) return o.name + guid_text(guid_of(o)) + '\0';
	if (o.class_path == "StandardSessionSlotModifier")
		return o.class_path + guid_text(guid_of(o)) + " Slot" + std::to_string(slot_index(w, o)) + '\0';
	for (auto &s : w.sessions) {
		if (s->obj == &o) return o.class_path + '\0';
	}
	return {};
}

StateManager *manager_of(World &w, const GameObject *o) {
	if (!o) return nullptr;
	auto &m = rw(w).managers;
	auto it = m.find(o->handle);
	return it == m.end() || it->second.owner != o ? nullptr : &it->second;
}

// DE_CCreature::ObjPrepare (Creature 0x15016050): a tick-based creature's manager keeps a second
// of ticks.
void set_window(World &w, StateManager &sm) {
	if (uint32_t rate = sm.owner->is("Actor\\Creature") ? w.creature_rate(sm.owner) : 0) sm.window = uint32_t(1000.0f / float(rate));
}

// DE_CSession::Add + DE_CSessionRecord::Add (Core 0x701f9520 / 0x70203610); a manager that kept
// states outside the session keeps them.
StateManager &join(World &w, GameObject &o) {
	auto &R = rw(w);
	StateManager &sm = R.managers[o.handle];
	if (sm.owner == &o && !sm.local) return sm;
	std::vector<LocalState> history;
	uint32_t adds = 0;
	if (sm.owner == &o) history = std::move(sm.history), adds = sm.adds;
	sm = StateManager{};
	sm.owner = &o;
	sm.history = std::move(history);
	sm.adds = adds;
	ManagerFlags flags = manager_flags(o);
	sm.connected = flags.joins;
	sm.keep_clock = flags.keep_clock;
	set_window(w, sm);
	if (!sm.connected) return sm;
	sm.id = build_id(w, o);
	sm.joined = ++R.joins;
	// An empty ID matches the first empty-ID record, so all such managers share one.
	if (auto &rec = R.record; rec && flags.record) {
		sm.record = rec->find(sm.id);
		if (!sm.record && rec->recording) {
			sm.record = rec->managers.emplace_back(std::make_unique<StateRecord>()).get();
			sm.record->id = sm.id;
		}
		sm.replay = sm.record && !rec->recording;
		if (sm.record) R.joined[sm.record].push_back(o.handle);
	}
	if (getenv("OPENBG_DEBUG_REPLAY")) {
		std::string shown;
		for (char ch : sm.id) shown += ch >= 32 && ch < 127 ? ch : '.';
		fprintf(stderr, "[replay %u] join %s (%s): %s, window %u, defer %u\n", w.frame(), o.name.c_str(), shown.c_str(),
		        sm.record ? (sm.replay ? "replayed" : "recorded") : "-", sm.window, creature_flags(o).defer);
	}
	return sm;
}

// The manager of an object outside the recorded or replayed session: CreateCurrentState keeps its
// states all the same.
StateManager &own_manager(World &w, GameObject &o) {
	StateManager &sm = rw(w).managers[o.handle];
	if (sm.owner == &o) return sm;
	sm = StateManager{};
	sm.owner = &o;
	sm.local = true;
	sm.connected = false;
	sm.keep_clock = manager_flags(o).keep_clock;
	set_window(w, sm);
	return sm;
}

// DE_CRecordPacker::AddElementID / DE_CSession::AddObjectByGlobalID (Core 0x702049b0 / 0x701f9a90):
// a v1 chunk of a kind byte (0 an object no manager owns, 1 known, 2 null) and, when known, the
// ID of the newest manager it owns.
void add_element(World &w, Packer &p, uint32_t handle) {
	if (p.local) {
		p.refs.emplace_back(p.data.size(), handle);
		p.u8(1), p.u8(5), p.u8(3), p.u32(handle);
		return;
	}
	GameObject *o = handle ? w.object(handle) : nullptr;
	const StateManager *best = nullptr;
	if (o) {
		for (auto &[h, sm] : rw(w).managers) {
			if (sm.owner == o && sm.connected && (!best || sm.joined > best->joined)) best = &sm;
		}
	}
	Packer c;
	c.u8(!o ? 2 : best ? 1 : 0);
	if (best) {
		c.u32(uint32_t(best->id.size()));
		c.raw(best->id.data(), best->id.size());
	}
	p.u8(1);
	p.u8(uint8_t(c.data.size()));
	if (c.data.size() >= 0xFF) {
		p.data.back() = 0xFF;
		p.u32(uint32_t(c.data.size()));
	}
	p.raw(c.data.data(), c.data.size());
}
// ExtractObjectByGlobalID (Core 0x701f9940).
uint32_t extract_element(World &w, Packer &p) {
	size_t end = p.chunk_end();
	uint32_t found = 0;
	uint8_t kind = p.r8();
	if (kind == 3) {
		found = p.r32();
		if (!w.object(found)) found = 0;
	} else if (kind == 1) {
		auto b = p.rbytes();
		std::string id(b.begin(), b.end());
		uint64_t newest = 0;
		for (auto &[h, sm] : rw(w).managers) {
			if (sm.connected && sm.id == id && w.object(h) == sm.owner && sm.joined > newest) newest = sm.joined, found = h;
		}
	}
	p.pos = end;
	return found;
}

// Type 2 replicated values: u32 length, then what the value class's generate function (its class
// node +0xdc) writes.
enum class Value { Unknown, Text, Data, Guid, GuidPool, Vector, Coordinates };
Value value_kind(const std::string &cls) {
	auto is = [&](const char *n) { return cls == n || cls == std::string("CObject::") + n; };
	if (is("CText")) return Value::Text;
	if (is("CData")) return Value::Data;
	if (is("CGUIDPool")) return Value::GuidPool;
	if (is("CGUID")) return Value::Guid;
	if (is("CVector")) return Value::Vector;
	if (is("CCoordinateSystem")) return Value::Coordinates;
	return Value::Unknown;
}
void add_value(World &w, Packer &p, const std::string &cls, uint32_t inst) {
	vm::Memory &m = w.memory();
	Packer v;
	switch (inst ? value_kind(cls) : Value::Unknown) {
	case Value::Text: {  // squeezed length with the NUL (0 for none), the bytes
		uint32_t s = m.get<uint32_t>(inst);
		std::string t = s ? std::string(reinterpret_cast<const char *>(m.ptr(s))) : std::string();
		v.squeezed(t.empty() ? 0 : uint32_t(t.size() + 1));
		if (!t.empty()) v.raw(t.c_str(), t.size() + 1);
		break;
	}
	case Value::GuidPool: {  // DE_CGUIDPool::GenerateContents
		uint32_t data = m.get<uint32_t>(inst), n = m.get<uint32_t>(inst + 4);
		v.u8(1);
		v.u16(uint16_t(n));
		for (uint32_t i = 0; i < n && data; ++i) v.raw(m.ptr(data + i * 16, 16), 16);
		break;
	}
	case Value::Guid: v.raw(m.ptr(inst, 16), 16); break;
	case Value::Vector: v.raw(m.ptr(inst, 12), 12); break;
	case Value::Coordinates: v.raw(m.ptr(inst, 36), 36); break;
	case Value::Data: {  // DataGenerateContentsCaller (Core 0x7014b5f0)
		const auto &b = cdata_bytes(inst);
		v.squeezed(uint32_t(b.size()));
		v.raw(b.data(), b.size());
		break;
	}
	case Value::Unknown: v.squeezed(0); break;
	}
	p.bytes(v.data);
}
void extract_value(World &w, Packer &p, const std::string &cls, uint32_t inst) {
	Packer v;
	v.data = p.rbytes();
	vm::Memory &m = w.memory();
	if (!inst || v.data.empty()) return;
	switch (value_kind(cls)) {
	case Value::Text: {
		auto b = v.rn(v.rsqueezed());
		m.set<uint32_t>(inst, w.string(std::string(b.begin(), std::find(b.begin(), b.end(), uint8_t(0)))));
		break;
	}
	case Value::GuidPool: {
		v.r8();
		uint32_t n = v.r16(), data = m.alloc(std::max<uint32_t>(1, n) * 16);
		for (uint32_t i = 0; i < n; ++i) v.take(m.ptr(data + i * 16, 16), 16);
		m.set<uint32_t>(inst, data);
		m.set<uint32_t>(inst + 4, n);
		break;
	}
	case Value::Data: cdata_bytes(inst) = v.rn(v.rsqueezed()); break;
	case Value::Guid:
	case Value::Vector:
	case Value::Coordinates: v.take(m.ptr(inst, uint32_t(v.data.size())), v.data.size()); break;
	default: break;
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

// ProcessStatus (Core 0x70145b50): restores the frames and clears the sleep; runs nothing.
// False for the no-frames byte, which leaves the instance as it is.
bool process_status(World &w, Packer &p, Brain &b) {
	for (const auto &bc : b.cls.code.behaviors) extract_variables(w, p, b.cls, bc.replicated, b.inst.members, b.inst.ref_members);
	std::vector<vm::Executor::RestoreFrame> frames;
	std::vector<size_t> local_at;
	for (;;) {
		uint8_t flag = p.r8();
		uint32_t width = (flag >> 1) & 3;
		if (width == 3) return false;
		uint32_t mi = width == 0 ? p.r8() : width == 1 ? p.r16() : p.r32();
		uint32_t ip = (flag & 8) ? p.r16() : p.r8();
		vm::ScriptMethod *m = b.cls.method(mi);
		if (!m) return false;
		frames.push_back({m, ip});
		local_at.push_back(p.pos);
		// The locals go into the rebuilt frame (below), which does not exist yet.
		for (const Replicated &r : m->cm->replicated) {
			if (r.type == 0) p.pos += r.b;
			else if (r.type == 2) p.rbytes();
			else if (r.type == 3 || r.type == 4) p.pos = p.chunk_end();
		}
		if (!(flag & 1)) break;
	}
	if (!b.exec) return false;
	b.inst.sleep = 0;
	size_t k = 0;
	size_t end = p.pos;
	b.exec->restore(frames, [&](vm::ScriptMethod &m, uint32_t addr, std::vector<uint32_t> &refs) {
		p.pos = local_at[k++];
		extract_variables(w, p, b.cls, m.cm->replicated, addr, refs);
	});
	p.pos = end;
	return true;
}

std::vector<Brain *> brains_of(World &w, const GameObject &o) {
	std::vector<Brain *> out;
	for (Brain *b : w.brains()) {
		if (b->owner == &o) out.push_back(b);
	}
	return out;
}

// DE_CClassExecutor +0x4c (contents v4/v5, default 1; IsExecuteStatesFromAllInstant 0x70146440):
// the brain runs a restored state at once.
bool executes_states_instantly(World &w, const Brain &b) {
	if (!b.owner) return true;
	std::vector<GameObject *> around(b.owner->children);
	around.insert(around.end(), b.owner->components.begin(), b.owner->components.end());
	for (GameObject *e : around) {
		if (!e || !e->file || e->class_path != "ClassExecutor") continue;
		de::ObjectRef ref{e->file, e->index};
		bool ours = false;
		for (auto &lists : reference_lists(w.data(), ref)) {
			for (auto &l : lists) {
				for (auto &t : l) ours |= t && t.info().class_path == "Class" && t.info().name == b.cls.name;
			}
		}
		if (!ours) continue;
		try {
			de::Contents c(e->file->object_data(ref.info()).contents);
			de::Chunk ch = de::begin_chunk(c);
			if (ch.version < 4) return true;
			c.block(), c.block(), c.u32(), c.boolean();
			return c.boolean();
		} catch (const std::exception &) {
			return true;
		}
	}
	return true;
}

// DE_CCoordinateSystem::Decompose (Core 0x70027230) and its inverse (Reset, RotateX, Y, Z).
std::array<float, 3> decompose(const std::array<float, 9> &m) {
	if (m[0] == 0 && m[1] == 0) return {std::atan2(-m[3], -m[6]), float(M_PI / 2), 0};
	return {std::atan2(m[5], m[8]), std::asin(-m[2]), std::atan2(m[1], m[0])};
}
std::array<float, 9> compose(const std::array<float, 3> &a) {
	float cx = std::cos(a[0]), sx = std::sin(a[0]), cy = std::cos(a[1]), sy = std::sin(a[1]), cz = std::cos(a[2]),
	      sz = std::sin(a[2]);
	return {cy * cz, cy * sz, -sy, sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy, cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy};
}

float length3(const std::array<float, 3> &v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// DE_CFighterPhysics' packed fields (PackState / UnpackState, OMF 0x3503a5d0 / 0x3503a9d0).
struct FighterState {
	std::array<float, 3> momentum{}, accel{}, pos{};
	uint16_t heading = 0;
	float gravity = 1, recover = 1, turn_rate = 0, forward_speed = 0, guide_power = 0;
	bool skip_friction = false, skip_push = false, on_floor = false;
	uint32_t last_hit = 0, last_hit_by = 0;
	int guide_mode = 0, guide_frames = -1, invulnerable = 0;
};
constexpr double kPi = 3.14159265358979323846;
uint16_t heading_of(const std::array<float, 9> &r) {
	return uint16_t(int64_t((std::atan2(double(r[0]), double(r[2])) + kPi + kPi / 2) / (2 * kPi) * 65536.0));
}
std::array<float, 9> yaw_of(uint16_t h) {
	double th = h / 65536.0 * 2 * kPi + kPi / 65536.0;
	float c = float(std::cos(th)), s = float(std::sin(th));
	return {c, 0, -s, 0, 1, 0, s, 0, c};
}

// The live physics; momentum at or below 0.01 and acceleration at or below 0.001 are zeroed in it
// as PackState does.
FighterState capture_physics(World &w, GameObject &o) {
	auto &fs = w.forces->surfaces(o);
	if (length3(o.momentum) <= 0.01f) o.momentum = {0, 0, 0};
	if (length3(fs.accel) <= 0.001f) fs.accel = {0, 0, 0};
	FighterState s;
	s.momentum = o.momentum;
	s.accel = fs.accel;
	s.pos = o.local.t;
	s.heading = heading_of(o.local.r);
	s.gravity = o.gravity;
	s.recover = o.gravity_recover;
	s.turn_rate = o.turn_rate;
	s.forward_speed = o.forward_speed;
	s.skip_friction = o.skip_friction || o.propelled;
	s.skip_push = fs.skip_push;
	s.on_floor = o.on_floor;
	s.last_hit = o.last_hit ? o.last_hit->handle : 0;
	s.last_hit_by = o.last_hit_by ? o.last_hit_by->handle : 0;
	s.guide_mode = o.guide_mode;
	s.guide_frames = o.guide_frames;
	s.guide_power = o.guide_power;
	s.invulnerable = o.invulnerable_frames;
	return s;
}

// PackState: a u16 mask, then momentum (0x10), acceleration (0x08), the position, the heading as a
// u16 turn, gravity and its recovery (0x04), turn rate (0x100), forward speed (0x200), last hit
// (0x01) and hit by (0x02), the guide (0x0c00: its mode in the mask, power, frames) and the
// invulnerable frames (0x1000). `got_hit` is the fighter's +0x4a0.
void encode_physics(World &w, Packer &p, const FighterState &s, bool got_hit) {
	uint16_t m = 0;
	if (length3(s.momentum) > 0.01f) m |= 0x10;
	if (length3(s.accel) > 0.001f) m |= 0x08;
	if (s.skip_friction) m |= 0x20;
	if (s.skip_push) m |= 0x40;
	if (s.on_floor) m |= 0x80;
	if (s.gravity != 1.0f) m |= 0x04;
	bool guide = s.guide_frames >= 0;
	if (s.guide_mode == 1) {
		if (s.last_hit) m |= 0x01;
		else guide = false;
	}
	// As the binary tests it: mode 2 sends the hit-by only while there is none.
	if ((s.guide_mode == 2 && !s.last_hit_by) || got_hit) m |= 0x02;
	if (s.turn_rate != 0) m |= 0x100;
	if (s.forward_speed != 0) m |= 0x200;
	if (guide) m |= uint16_t((s.guide_mode & 3) << 10);
	if (s.invulnerable) m |= 0x1000;
	p.u16(m);
	if (m & 0x10) p.raw(s.momentum.data(), 12);
	if (m & 0x08) p.raw(s.accel.data(), 12);
	p.raw(s.pos.data(), 12);
	p.u16(s.heading);
	if (m & 0x04) {
		p.f32(s.gravity);
		p.f32(s.recover);
	}
	if (m & 0x100) p.f32(s.turn_rate);
	if (m & 0x200) p.f32(s.forward_speed);
	if (m & 0x01) add_element(w, p, s.last_hit);
	if (m & 0x02) add_element(w, p, s.last_hit_by);
	if (m & 0x0c00) {
		p.f32(s.guide_power);
		p.squeezed(uint32_t(s.guide_frames));
	}
	if (s.invulnerable) p.squeezed(uint32_t(s.invulnerable));
}

// UnpackState sets each part outright: absent, its default.
FighterState decode_physics(World &w, Packer &p) {
	FighterState s;
	uint16_t m = p.r16();
	if (m & 0x10) p.take(s.momentum.data(), 12);
	if (m & 0x08) p.take(s.accel.data(), 12);
	p.take(s.pos.data(), 12);
	s.heading = p.r16();
	if (m & 0x04) {
		s.gravity = p.rf();
		s.recover = p.rf();
	}
	s.skip_friction = (m & 0x20) != 0;
	s.skip_push = (m & 0x40) != 0;
	s.on_floor = (m & 0x80) != 0;
	if (m & 0x100) s.turn_rate = p.rf();
	if (m & 0x200) s.forward_speed = p.rf();
	auto actor = [&] {
		GameObject *a = w.object(extract_element(w, p));
		return a && a->is("Actor") ? a->handle : 0u;
	};
	if (m & 0x01) s.last_hit = actor();
	if (m & 0x02) s.last_hit_by = actor();
	if (m & 0x0c00) {
		s.guide_power = p.rf();
		s.guide_frames = int(p.rsqueezed());
	}
	s.guide_mode = (m >> 10) & 3;
	if (m & 0x1000) s.invulnerable = int(p.rsqueezed());
	return s;
}

// The orientation becomes a pure yaw; DE_COMF2Fighter::UnpackState (0x3502a500) then puts the actor
// where the physics is.
void apply_physics(World &w, GameObject &o, const FighterState &s) {
	auto &fs = w.forces->surfaces(o);
	o.momentum = s.momentum;
	fs.accel = s.accel;
	o.local.t = s.pos;
	o.local.r = yaw_of(s.heading);
	o.gravity = s.gravity;
	o.gravity_recover = s.recover;
	o.skip_friction = s.skip_friction;
	fs.skip_push = s.skip_push;
	o.on_floor = s.on_floor;
	o.turn_rate = s.turn_rate;
	o.forward_speed = s.forward_speed;
	o.last_hit = s.last_hit ? w.object(s.last_hit) : nullptr;
	o.last_hit_by = s.last_hit_by ? w.object(s.last_hit_by) : nullptr;
	o.guide_power = s.guide_power;
	o.guide_frames = s.guide_frames;
	o.guide_mode = s.guide_mode;
	o.invulnerable_frames = s.invulnerable;
}

// DE_CFighterPhysics::RepackState (OMF 0x3503aca0): unpacked into a scratch physics (+0x1ac) and
// packed again, so the mask is recomputed, and the hit bit follows the live fighter's +0x4a0.
void repack_physics(World &w, Packer &src, Packer &dst, bool got_hit) {
	FighterState s = decode_physics(w, src);
	s.heading = heading_of(yaw_of(s.heading));
	encode_physics(w, dst, s, got_hit);
}

// The creature's physics (+0x1cc): a fighter's, else its first simple physics.
GameObject *simple_physics_of(World &w, GameObject &o) {
	auto ps = w.simple_physics->physics_of(o);
	return ps.empty() ? nullptr : ps[0];
}

// DE_CObject::PackState (Core 0x7003de40) / DE_CCreature::PackState (Creature 0x150150b0): each
// executor instance's status back to back; for a creature then its position (+0x300), its
// orientation as three angles (+0x304) and its physics' own state (a fighter's from `phys_at`).
void pack_state(World &w, Packer &p, GameObject &o, StateManager &sm, size_t &phys_at, size_t &phys_end) {
	phys_at = phys_end = 0;
	for (Brain *b : brains_of(w, o)) generate_status(w, p, *b);
	if (!o.is("Actor\\Creature")) return;
	CreatureFlags f = creature_flags(o);
	if (f.position) p.raw(o.local.t.data(), 12);
	if (f.orientation) {
		auto a = decompose(o.local.r);
		p.raw(a.data(), 12);
	}
	if (o.fighter) {
		size_t refs = p.refs.size();
		phys_at = p.data.size();
		encode_physics(w, p, capture_physics(w, o), sm.got_hit);
		phys_end = p.data.size();
		p.refs.resize(refs);
		sm.got_hit = false;  // DE_COMF2Fighter::PackState (OMF 0x3502a4e0)
	} else if (GameObject *ph = simple_physics_of(w, o)) {
		// DE_CSimplePhysics::PackState (Core 0x701dc5b0).
		auto &s = w.simple_physics->state(*ph);
		if (s.pack_momentum) p.raw(s.momentum.data(), 12);
	}
}

void set_time(World &w, GameObject &o, uint32_t ms);

// DE_CObject::UnpackState (Core 0x7003de80) / DE_CCreature::UnpackState (Creature 0x15015160):
// every status first, then a creature's position and orientation; each restored instance whose
// executor runs states at once does so (a creature's with the scene clock at the state's time);
// then a creature's physics, and its clock is set to the scene's (SetTime), at once or, with
// +0x1e0, at the next scene update.
void unpack_state(World &w, Packer &p, GameObject &o, uint32_t t) {
	bool creature = o.is("Actor\\Creature");
	uint32_t rate = creature ? w.creature_rate(&o) : 0;
	Clock *clock = rate ? &rw(w).clocks[o.handle] : nullptr;
	if (clock) {
		o.creature_ticks = t;
		clock->unpacking = t;
	}
	std::vector<Brain *> run;
	for (Brain *b : brains_of(w, o)) {
		if (process_status(w, p, *b) && executes_states_instantly(w, *b)) run.push_back(b);
	}
	CreatureFlags f = creature ? creature_flags(o) : CreatureFlags{};
	if (f.position) p.take(o.local.t.data(), 12);
	if (f.orientation) {
		std::array<float, 3> a;
		p.take(a.data(), 12);
		o.local.r = compose(a);
	}
	auto pulse = [&](Brain *b) {
		if (!b->exec || !b->exec->running() || b->exec->executing() || b->inst.shut_down) return;
		try {
			b->exec->pulse();
		} catch (const std::exception &e) {
			w.log.push_back(o.name + ": " + e.what());
			b->inst.shut_down = true;
		}
	};
	for (Brain *b : run) {
		if (creature) w.at_scene_ms(clock ? clock->base + t * rate : t, [&] { pulse(b); });
		else pulse(b);
	}
	if (!creature) return;
	if (clock) o.creature_ticks = t;
	if (o.fighter) apply_physics(w, o, decode_physics(w, p));
	else if (GameObject *ph = simple_physics_of(w, o)) {
		auto &s = w.simple_physics->state(*ph);
		if (s.pack_momentum) p.take(s.momentum.data(), 12);
	}
	if (clock) {
		if (!f.defer) set_time(w, o, uint32_t(w.scene_ms()));
		else if (std::find(rw(w).deferred.begin(), rw(w).deferred.end(), o.handle) == rw(w).deferred.end())
			rw(w).deferred.push_back(o.handle);
		clock->unpacking = 0;
	}
}

// DE_CStateManagerRecord::Record (Core 0x70203fa0): a state at the same time as the last replaces
// it; the record's first manager's owner repacks it (DE_CCreature::RepackState, Creature
// 0x15015380): the bytes as they are, references as the newest manager ID of their object now,
// a fighter's physics through RepackState. With that owner gone, nothing is recorded.
void record_state(World &w, StateRecord &rec, const LocalState &ls) {
	auto &R = rw(w);
	if (!rec.states.empty() && rec.states.back().first == ls.time) rec.states.pop_back();
	auto &joined = R.joined[&rec];
	StateManager *first = nullptr;
	if (!joined.empty()) {
		auto it = R.managers.find(joined[0]);
		if (it != R.managers.end() && w.object(joined[0]) == it->second.owner) first = &it->second;
	}
	if (!first) {
		w.log.push_back("Object is dead while a state is still being recorded.");
		return;
	}
	Packer out, src;
	src.data = ls.bytes;
	size_t pos = 0;
	auto copy_to = [&](size_t end) {
		out.raw(ls.bytes.data() + pos, end - pos);
		pos = end;
	};
	for (auto &[at, h] : ls.refs) {
		copy_to(at);
		add_element(w, out, h);
		pos += kLocalRef;
	}
	if (ls.phys_end > ls.phys_at) {
		copy_to(ls.phys_at);
		src.pos = ls.phys_at;
		repack_physics(w, src, out, first->owner->fighter && first->got_hit);
		pos = ls.phys_end;
	}
	copy_to(ls.bytes.size());
	rec.states.emplace_back(ls.time, std::move(out.data));
}

// DE_CStateManager::CleanOldStates (Core 0x701b1c90): all of them, or those older than the
// window behind the newest, keeping at most 22; DE_CStandardStateManager::CleanState records each.
void clean_old_states(World &w, StateManager &sm, bool all) {
	auto &h = sm.history;
	if (h.empty()) return;
	uint32_t newest = h.back().time;
	int i = int(h.size()) - 1;
	if (!all) {
		if (newest <= sm.window) return;
		i = int(h.size()) - 2;
		if (i < 0) return;
		for (int k = 0; newest - sm.window < h[size_t(i)].time && k < 0x15; ++k) {
			if (--i < 0) return;
		}
	}
	for (int k = 0; k <= i; ++k) {
		if (sm.record && !sm.replay) record_state(w, *sm.record, h[size_t(k)]);
	}
	h.erase(h.begin(), h.begin() + i + 1);
}

// AddCurrentState (Core 0x70126250): AddToNewStateList and AddToStateList (0x701b1b40 /
// 0x701b19f0) each count one and clean the old states past 40; a state at the same time replaces
// the one in the history, which is not recorded.
void add_current_state(World &w, StateManager &sm, LocalState ls) {
	for (int k = 0; k < 2; ++k) {
		if (++sm.adds > 40) {
			clean_old_states(w, sm, false);
			sm.adds = 0;
		}
	}
	auto &h = sm.history;
	size_t at = h.size();
	while (at > 0 && h[at - 1].time >= ls.time) --at;
	if (at < h.size() && h[at].time == ls.time) h[at] = std::move(ls);
	else h.insert(h.begin() + long(at), std::move(ls));
}

// DE_CStateManager::UnjoinStateManagerRecord (Core 0x701b0e90).
void unjoin(World &w, StateManager &sm) {
	if (!sm.record) return;
	auto &v = rw(w).joined[sm.record];
	v.erase(std::remove(v.begin(), v.end(), sm.owner->handle), v.end());
	sm.record = nullptr;
}

// DE_CSessionRecord::ObjGenerateContents (Core 0x70202890, v5), each DE_CStateManagerRecord as
// GenerateContents (0x702042c0, v1) writes it.
std::vector<uint8_t> record_contents(const SessionRecord &r) {
	de::ContentsWriter c;
	size_t at = c.begin_chunk(5);
	c.string(r.name);
	c.guid(r.master);
	c.guid_pool(r.modifiers);
	c.i32(int32_t(r.managers.size()));
	for (auto &m : r.managers) {
		size_t mat = c.begin_chunk(1);
		c.block({reinterpret_cast<const uint8_t *>(m->id.data()), m->id.size()});
		c.i32(int32_t(m->states.size()));
		for (auto &[t, data] : m->states) {
			c.u32(t);
			c.block(data);
		}
		c.end_chunk(mat);
	}
	c.guid(r.factory);
	c.u32(r.end_ms);
	c.f32(r.length);
	c.u32(uint32_t(r.date));
	c.i32(r.auto_delete);
	c.string(r.player);
	c.string(r.server);
	c.filler(0x32);
	c.end_chunk(at);
	return c.take();
}

// ObjProcessContents (0x702029b0): v1 the name; v2 adds the master description, the modifiers,
// the managers' records, the factory GUID and the end time; v3 the date and AutoDelete; v4 the
// length (after the end time); v5 the player and server names.
void read_contents(SessionRecord &r, const std::vector<uint8_t> &data, bool states) {
	de::Contents c(data);
	de::Chunk ch = de::begin_chunk(c);
	auto text = [&] {
		auto b = c.block();
		return std::string(b.begin(), std::find(b.begin(), b.end(), uint8_t(0)));
	};
	r.name = text();
	if (ch.version < 2 || ch.version > 5) return;
	r.master = c.guid();
	if (c.u8() == 1) {
		uint16_t n = c.u16();
		for (uint16_t i = 0; i < n; ++i) r.modifiers.push_back(c.guid());
	}
	int32_t n = c.i32();
	for (int32_t i = 0; i < n; ++i) {
		// DE_CStateManagerRecord::ProcessContents (0x70204390).
		de::Chunk mc = de::begin_chunk(c);
		if (states) {
			auto &m = *r.managers.emplace_back(std::make_unique<StateRecord>());
			auto id = c.block();
			m.id.assign(id.begin(), id.end());
			int32_t k = c.i32();
			for (int32_t j = 0; j < k; ++j) {
				uint32_t t = c.u32();
				m.states.emplace_back(t, c.block());
			}
		}
		c.seek(mc.end);
	}
	r.factory = c.guid();
	r.end_ms = c.u32();
	if (ch.version >= 4) r.length = c.f32();
	if (ch.version >= 3) {
		r.date = c.u32();
		r.auto_delete = c.i32();
	}
	if (ch.version >= 5) {
		r.player = text();
		r.server = text();
	}
}

// The record as the master object of its own file (DE_CTransferAgentWinFile::NewUserFile +
// NewMasterObject, Core 0x70051e80 / 0x700564c0); references and hierarchy as DE_CObject leaves
// them (ObjGenerateReferences 0x70202eb0: v2 and v1, both empty).
bool write_record(const SessionRecord &r) {
	de::NewObject o;
	o.name = r.name;
	o.class_path = "SessionRecord";
	o.guid = r.guid;
	o.contents = record_contents(r);
	o.references = {2, 0, 1, 0};
	o.hierarchy = {1, 1, 0};
	auto bytes = de::write_objects_file(r.agent, r.agent_guid, {o});
	std::ofstream f(r.path, std::ios::binary);
	f.write(reinterpret_cast<const char *>(bytes.data()), long(bytes.size()));
	return bool(f);
}

std::vector<std::shared_ptr<SessionRecord>> records_in(const de::ObjectsFile &f, const std::string &path, bool states) {
	std::vector<std::shared_ptr<SessionRecord>> out;
	for (uint32_t i = 0; i < f.objects().size(); ++i) {
		const de::ObjectInfo &info = f.objects()[i];
		if (info.class_path != "SessionRecord") continue;
		auto r = std::make_shared<SessionRecord>();
		r->path = path;
		r->agent = f.master().name;
		r->agent_guid = f.master().guid;
		r->guid = info.guid;
		try {
			read_contents(*r, f.object_data(info).contents, states);
		} catch (const std::exception &) {
			continue;
		}
		out.push_back(r);
	}
	return out;
}

std::vector<std::shared_ptr<SessionRecord>> read_records(const std::string &path, bool states) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return {};
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
	try {
		return records_in(de::ObjectsFile(std::move(data)), path, states);
	} catch (const std::exception &) {
		return {};
	}
}

// A tick-based creature's clock; met first in a tick (`tick_ms`), its base is where that tick
// puts it, else the last step of the creature's rate.
Clock &clock_of(World &w, GameObject &o, uint32_t rate, int64_t tick_ms = -1) {
	auto [it, made] = rw(w).clocks.try_emplace(o.handle);
	if (made) {
		uint32_t now = uint32_t(w.scene_ms());
		uint32_t at = tick_ms >= 0 ? uint32_t(tick_ms) : now - now % rate;
		it->second.base = at - (o.creature_ticks + (tick_ms >= 0 ? 1 : 0)) * rate;
	}
	return it->second;
}

// DE_CStandardStateManager::AddLaggedState (Core 0x701aec10) for a replayed state: it goes after
// the newest state at or before its time, the newer ones discarded; the same state at the same
// time is kept and not restored again. Past 40 adds the old ones are cleaned (not recorded).
bool add_lagged_state(StateManager &sm, uint32_t t, const std::vector<uint8_t> &bytes) {
	auto &h = sm.lagged;
	size_t at = h.size();
	while (at > 0 && h[at - 1].first > t) --at;
	if (at > 0 && h[at - 1].first == t) {
		if (h[at - 1].second == bytes) return false;
		--at;
	}
	h.erase(h.begin() + long(at), h.end());
	h.emplace_back(t, bytes);
	if (++sm.adds > 40) {
		uint32_t newest = h.back().first;
		int i = int(h.size()) - 2;
		if (newest > sm.window && i >= 0) {
			bool keep = false;
			for (int k = 0; newest - sm.window < h[size_t(i)].first && k < 0x15; ++k) {
				if (--i < 0) {
					keep = true;
					break;
				}
			}
			if (!keep) h.erase(h.begin(), h.begin() + i + 1);
		}
		sm.adds = 0;
	}
	return true;
}

void restore(World &w, StateManager &sm, const std::pair<uint32_t, std::vector<uint8_t>> &st) {
	if (getenv("OPENBG_DEBUG_REPLAY") || (!sm.replay && getenv("OPENBG_DEBUG_STATES"))) fprintf(stderr, "[replay %u] %s takes its %u state\n", w.frame(), sm.owner->name.c_str(), st.first);
	Packer p;
	p.data = st.second;
	unpack_state(w, p, *sm.owner, st.first);
}

// DE_CStateManager::GetState / GetNewestStateInRange (Core 0x701b1f40, Creature 0x1501a630): the
// newest state in its list (+0x3c: the recorded ones a replay took, else the owner's own) at or
// before `to`, if not before `from`; copied, as restoring it may change the list.
std::optional<std::pair<uint32_t, std::vector<uint8_t>>> newest_state(StateManager &sm, uint32_t from, uint32_t to) {
	if (sm.replay) {
		for (auto it = sm.lagged.rbegin(); it != sm.lagged.rend(); ++it) {
			if (it->first <= to) return it->first >= from ? std::optional(*it) : std::nullopt;
		}
	} else {
		for (auto it = sm.history.rbegin(); it != sm.history.rend(); ++it) {
			if (it->time <= to) return it->time >= from ? std::optional(std::pair(it->time, it->bytes)) : std::nullopt;
		}
	}
	return std::nullopt;
}
std::optional<std::pair<uint32_t, std::vector<uint8_t>>> state_at(StateManager &sm, uint32_t t) { return newest_state(sm, t, t); }

// UpdateForReplay (Core 0x701af350): the newest recorded state at or before `t` (the owner's
// local time), when not the last one taken, joins the history; the session's fixup then restores
// it (SetupOwnerForState, 0x701af020).
void update_for_replay(World &w, StateManager &sm, uint32_t t) {
	if (!sm.record || sm.record->states.empty()) return;
	auto &st = sm.record->states;
	size_t &c = sm.record->cursor;
	if (c >= st.size()) c = st.size() - 1;
	while (c > 0 && st[c].first > t) --c;
	while (c + 1 < st.size() && st[c + 1].first <= t) ++c;
	if (st[c].first > t || st[c].first == sm.applied) return;
	sm.applied = st[c].first;
	if (add_lagged_state(sm, st[c].first, st[c].second)) restore(w, sm, sm.lagged.back());
}

void retick(World &w, GameObject &o, uint32_t at_ms) {
	++rw(w).reticking;
	w.retick(o, at_ms);
	--rw(w).reticking;
}

// DE_CCreature::AssureSequenced (Creature 0x15015be0), as a hold ends: short of the tick it was
// at, a replayed creature takes its record again and re-ticks from the newest state it holds;
// past it, it re-ticks from there.
void assure_sequenced(World &w, GameObject &o, Clock &c) {
	if (c.mode != 2) return;
	uint32_t now = o.creature_ticks;
	if (now < c.saved) {
		c.mode = 0;
		StateManager *sm = manager_of(w, &o);
		if (!sm) return;
		if (sm->replay) update_for_replay(w, *sm, now);
		uint32_t from = now;
		for (auto it = sm->lagged.rbegin(); it != sm->lagged.rend(); ++it) {
			if (it->first <= now) {
				auto st = *it;
				restore(w, *sm, st);
				if (!sm->keep_clock) from = st.first;
				break;
			}
		}
		o.creature_ticks = from;
		while (o.creature_ticks < now) retick(w, o, uint32_t(w.scene_ms()));
	} else if (c.saved < now) {
		o.creature_ticks = c.saved;
		c.saved = now;
		while (o.creature_ticks < c.saved) retick(w, o, uint32_t(w.scene_ms()));
	}
}

// DE_CCreature::RewindTime (Creature 0x15015eb0): back to tick `to`, the creature holds (no physics
// or brain) until its clock is back where it was.
void rewind_time(World &w, GameObject &o, Clock &c, uint32_t to) {
	uint32_t was = o.creature_ticks;
	if (to > was) return;
	if (was == to) {
		if (c.mode) assure_sequenced(w, o, c);
		c.mode = 0;
		c.until = to;
	} else {
		if (c.mode != 2) c.saved = was, c.mode = 2;
		c.until = was;
	}
	o.creature_ticks = to;
}

// DE_CCreature::SetTime (Creature 0x15015d20): to the tick of scene time `ms`; behind it the
// creature re-ticks up to it, at most 100 ticks; ahead of it, it rewinds.
void set_time(World &w, GameObject &o, uint32_t ms) {
	uint32_t rate = w.creature_rate(&o);
	if (!rate) return;
	Clock &c = clock_of(w, o, rate);
	if (ms < c.base) {
		o.creature_ticks = 0;
		return;
	}
	uint32_t tick = (ms - c.base) / rate, at = c.base + o.creature_ticks * rate;
	if (ms <= at) {
		if (tick < o.creature_ticks) rewind_time(w, o, c, tick);
		return;
	}
	uint32_t n = (ms - at) / rate;
	if (n > 100) {
		n = 100;
		o.creature_ticks = tick - 100;
	}
	for (; n; --n) retick(w, o, c.base + o.creature_ticks * rate);
	o.creature_ticks = tick;
	c.until = tick;
}

// The recorded or replayed session's: under it, or in its scene; outside both, anyone's.
bool in_session(World &w, const GameObject *o) {
	GameObject *own = rw(w).session ? rw(w).session->obj : nullptr;
	const GameObject *root = o;
	for (const GameObject *a = o; a; root = a, a = a->parent) {
		for (auto &s : w.sessions) {
			if (s->obj == a) return a == own;
		}
	}
	if (std::find(w.scene_roots.begin(), w.scene_roots.end(), root) != w.scene_roots.end()) return w.scene_session_of(root) == own;
	return true;
}

void forget(World &w) {
	auto &R = rw(w);
	if (R.record && !R.record->recording) {
		w.time_scale = 1;  // the VCR's scale goes with the replay
		w.passed_ms = 0;
		for (Brain *b : w.brains()) b->inst.controlled = true;
	}
	R.prepared.clear();
	R.joined.clear();
	R.clocks.clear();
	R.deferred.clear();
	R.last_ms = 0;
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

// DE_CRecordingListUIManager::Begin (OMF 0x35062040) finds every SessionRecord in the loaded
// files: ours are the data folder's and the Recordings folder's.
std::vector<std::shared_ptr<SessionRecord>> list_recordings(const de::World *data) {
	std::vector<std::shared_ptr<SessionRecord>> out;
	if (data) {
		for (auto &[stem, f] : data->files()) {
			auto rs = records_in(*f, (data->dir() / (stem + ".DEObjects")).string(), false);
			out.insert(out.end(), rs.begin(), rs.end());
		}
	}
	std::error_code ec;
	for (auto &e : std::filesystem::directory_iterator(recordings_dir(), ec)) {
		if (e.path().extension() != ".DEObjects") continue;
		auto rs = read_records(e.path().string(), false);
		out.insert(out.end(), rs.begin(), rs.end());
	}
	std::sort(out.begin(), out.end(), [](auto &a, auto &b) { return a->date > b->date; });
	return out;
}

std::shared_ptr<SessionRecord> load_recording(const std::string &path) {
	auto rs = read_records(path, true);
	return rs.empty() ? nullptr : rs[0];
}

// DeleteMasterObject; the file holds nothing else, so DestroyResource deletes it.
bool delete_recording(const SessionRecord &r) {
	std::error_code ec;
	return std::filesystem::remove(r.path, ec);
}

// BeginEdit, ObjSetName, EndEdit.
bool rename_recording(SessionRecord &r, const std::string &name) {
	auto full = load_recording(r.path);
	if (!full) return false;
	full->name = name;
	if (!write_record(*full)) return false;
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
		// Each record's first manager flushes its history into it and leaves it.
		for (auto &m : r.managers) {
			auto &joined = R.joined[m.get()];
			if (joined.empty()) continue;
			StateManager *sm = manager_of(w, w.object(joined[0]));
			if (!sm) continue;
			clean_old_states(w, *sm, true);
			unjoin(w, *sm);
		}
		// +0x58: the session's class (its factory, a script class here).
		if (s.obj && s.obj->file) r.factory = s.obj->file->objects()[s.obj->index].guid;
		r.end_ms = uint32_t(w.scene_ms());
		r.length = float(r.end_ms) / 1000.0f;
		r.date = int64_t(time(nullptr));
		r.server = w.registry_string("Network\\Server Name");
		if (r.server.empty()) r.server = "Server";
		r.name = r.server;
		r.player = w.registry_string("Network\\Player Name");
		char stamp[96];
		time_t now = time_t(r.date);
		strftime(stamp, sizeof stamp, "Session Recording, %a %B %d %Y, %I-%M-%S%p", localtime(&now));
		r.agent = stamp;
		r.agent_guid = de::new_guid();
		r.guid = de::new_guid();
		std::error_code ec;
		std::filesystem::create_directories(recordings_dir(), ec);
		r.path = recordings_dir() + "/" + stamp + ".DEObjects";
		size_t states = 0;
		for (auto &m : r.managers) {
			states += m->states.size();
			if (!getenv("OPENBG_DEBUG_REPLAY")) continue;
			std::string shown;
			for (char ch : m->id) shown += ch >= 32 && ch < 127 ? ch : '.';
			fprintf(stderr, "[record] %s:", shown.c_str());
			for (auto &st : m->states) fprintf(stderr, " %u/%zu", st.first, st.second.size());
			fprintf(stderr, "\n");
		}
		if (write_record(r)) printf("recorded %s: %zu managers, %zu states\n", r.path.c_str(), r.managers.size(), states);
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
	// DE_CStateManager::ObjPrepare adds a manager to its scene's session (DE_CScene +0xa8): the
	// recorded session's scene objects and its own (its modifiers and slots are not in the scene).
	GameObject *own = R.session->obj;
	auto consider = [&](GameObject *o) {
		if (!o->is("StateManager") && !in_gui(*o) && has_state_manager(*o) && in_session(w, o)) join(w, *o);
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
	if (own && w.object(own->handle)) walk(own);
	uint32_t now = uint32_t(w.scene_ms());
	if (getenv("OPENBG_DEBUG_REPLAY") && w.frame() % 30 == 0) {
		for (GameObject *o : w.scene_objects()) {
			if (o->fighter) fprintf(stderr, "[fighter %u ms] %s %.1f %.1f %.1f tick %u\n", now, o->name.c_str(), o->local.t[0], o->local.t[1], o->local.t[2], o->creature_ticks);
		}
	}
	// OPENBG_DEBUG_REWIND=<frame>:<ms>: a replay's clock goes back that far then (testing).
	if (const char *rw = getenv("OPENBG_DEBUG_REWIND"); rw && !R.record->recording && w.frame() == uint32_t(atoi(rw))) {
		if (const char *ms = strchr(rw, ':')) w.passed_ms = -atof(ms + 1);
	}
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
	// DE_CStandardSessionSlot::SetupCreatureControl (Session 0x4501a9b0) makes no creature locally
	// controlled in a replay, so their `master` code (IsMaster, Core 0x70145830) never runs.
	for (Brain *b : w.brains()) {
		if (b->owner && b->owner->is("Actor\\Creature")) b->inst.controlled = false;
	}
	// DE_CScene::PassTime back (Core 0x70070040): its TimeRewound event deletes the run-time actors
	// prepared after the new time (DE_CActor::ProcessEvent 0x70090ed0, "ScenePrepareTime").
	if (now < R.last_ms) {
		for (GameObject *o : w.scene_objects()) {
			auto it = R.prepared.find(o->handle);
			if (it != R.prepared.end() && it->second > now && o->copied && !in_gui(*o)) w.clean_later(o);
		}
	}
	R.last_ms = now;
	for (GameObject *o : w.scene_objects()) R.prepared.emplace(o->handle, now);
	for (auto &[h, sm] : R.managers) {
		if (sm.replay) update_for_replay(w, sm, local_time(w, *sm.owner));
	}
}

void register_state(World &w, GameObject *owner, bool got_hit) {
	auto &R = rw(w);
	if (!owner) return;
	// The state manager of the object (or of the creature it is a component of).
	GameObject *o = owner;
	while (o && !has_state_manager(*o)) o = o->component_owner && o->component_owner != o ? o->component_owner : o->parent;
	if (!o || in_gui(*o)) return;
	StateManager &sm = R.record && R.record->recording && in_session(w, o) ? join(w, *o) : own_manager(w, *o);
	if (sm.replay) return;
	// CreateCurrentState (Core 0x701aeea0) at the owner's local time (RegisterExecutorState,
	// Creature 0x15015650), recording or not; the state is recorded when it ages out of the history.
	Packer p;
	p.local = true;
	LocalState ls;
	ls.time = local_time(w, *o);
	pack_state(w, p, *o, sm, ls.phys_at, ls.phys_end);
	ls.bytes = std::move(p.data);
	ls.refs = std::move(p.refs);
	add_current_state(w, sm, std::move(ls));
	// RegisterGetHitState (OMF 0x350296b0) flags the fighter after registering.
	sm.got_hit |= got_hit;
}

// DE_CStateManager::ProcessEvent (Core 0x701b18c0): on its owner's pre-destruction event a
// manager flushes its history into the record; it then leaves the record.
void replay_destroying(World &w, GameObject &o) {
	auto &R = rw(w);
	if (R.managers.empty() && R.clocks.empty()) return;
	std::vector<GameObject *> branch = {&o};
	for (size_t i = 0; i < branch.size(); ++i) branch.insert(branch.end(), branch[i]->children.begin(), branch[i]->children.end());
	for (GameObject *b : branch) {
		R.clocks.erase(b->handle);
		R.deferred.erase(std::remove(R.deferred.begin(), R.deferred.end(), b->handle), R.deferred.end());
		StateManager *sm = manager_of(w, b);
		if (!sm) continue;
		if (R.record && R.record->recording) clean_old_states(w, *sm, true);
		unjoin(w, *sm);
		R.managers.erase(b->handle);
	}
}

// DE_CCreature::Tick (Creature 0x15016870), its state side: a held creature skips physics and
// brain; one whose history holds a state for this very tick (not the one being restored) takes it
// and skips them too, live or replayed: a live creature's own state registered since its last
// tick comes back. At the end of the tick a hold ends once the clock is back where it was
// (AssureSequenced).
bool creature_state_tick(World &w, GameObject &o) {
	uint32_t rate = w.creature_rate(&o);
	if (!rate) return false;
	Clock &c = clock_of(w, o, rate, int64_t(w.scene_ms()));
	if (o.creation_time) c.base = o.creation_time;
	else if (!rw(w).reticking) c.base = uint32_t(w.scene_ms()) - (o.creature_ticks + 1) * rate;
	bool skip = c.mode == 2;
	if (!skip && c.mode == 0 && c.ticked && c.unpacking != o.creature_ticks) {
		if (StateManager *sm = manager_of(w, &o)) {
			if (auto st = state_at(*sm, o.creature_ticks)) {
				restore(w, *sm, *st);
				skip = true;
			}
		}
	}
	if (c.mode != 0) {
		if (o.creature_ticks >= c.until) {
			assure_sequenced(w, o, c);
			c.mode = 0;
			c.until = o.creature_ticks;
		}
	} else {
		c.until = o.creature_ticks;
	}
	c.ticked = true;
	return skip;
}

bool creature_delta_ticked(const GameObject &o) { return creature_flags(o).defer != 0; }

// Once it has ticked, the newest state its manager holds since its last delta tick comes back;
// +0x1e4 and +0x1d4 then follow the scene.
void creature_delta_tick(World &w, GameObject &o) {
	Clock &c = rw(w).clocks[o.handle];
	uint32_t now = uint32_t(w.scene_ms());
	if (now == c.until && !c.ticked) return;
	if (StateManager *sm = manager_of(w, &o); sm && c.ticked) {
		if (sm->replay) update_for_replay(w, *sm, now);
		if (auto st = newest_state(*sm, c.until + 1, now)) restore(w, *sm, *st);
	}
	c.until = now;
	o.creature_ticks = now;
	c.ticked = true;
}

bool replay_held(World &w, const GameObject &o) {
	auto &R = rw(w);
	auto it = R.clocks.find(o.handle);
	return it != R.clocks.end() && it->second.mode == 2;
}

void replay_set_time(World &w, GameObject &o, uint32_t ms) {
	if (replaying(w)) set_time(w, o, ms);
}

// DE_CScene::TemporalUpdate (Core 0x70070710): the deferred SetTime of restored creatures.
void replay_temporal_update(World &w) {
	auto &R = rw(w);
	auto list = std::move(R.deferred);
	R.deferred.clear();
	w.temporal_updating = true;
	for (uint32_t h : list) {
		if (GameObject *o = w.object(h)) set_time(w, *o, uint32_t(w.scene_ms()));
	}
	w.temporal_updating = false;
}

// SetInstanceNumberCaller (Creature 0x15014100): the number's digits join the ID suffix, which
// keeps one NUL at its end.
void set_instance_number(GameObject &o, int32_t n) {
	auto &[who, s] = g_instance_numbers[o.handle];
	if (who != &o) who = &o, s.clear();
	if (!s.empty()) s.pop_back();
	s += std::to_string(n) + '\0';
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
