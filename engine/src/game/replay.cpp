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

struct StateManager {
	GameObject *owner = nullptr;
	std::string id;
	StateRecord *record = nullptr;
	bool replay = false;       // +0x78
	uint32_t applied = ~0u;    // +0x80
	uint64_t joined = 0;       // the session's managers are searched newest first
	bool got_hit = false;      // fighter +0x4a0: RegisterGetHitState since its last state
	bool rewound = false;      // creature +0x2fc == 2: held since the clock went back
};

struct ReplayWorld {
	std::map<uint32_t, StateManager> managers;  // by owner handle
	std::shared_ptr<SessionRecord> record;      // the session being recorded or replaying
	SessionState *session = nullptr;
	uint64_t joins = 0;
	uint32_t last_ms = 0;
	std::map<uint32_t, uint32_t> prepared;  // "ScenePrepareTime" of the actors met while replaying
};

std::map<const World *, ReplayWorld> g_worlds;
ReplayWorld &rw(World &w) { return g_worlds[&w]; }
// DE_CCreature +0x1ec: SetInstanceNumber's suffix, by handle.
std::map<uint32_t, std::pair<const GameObject *, std::string>> g_instance_numbers;

bool has_state_manager(const GameObject &o) {
	return std::any_of(o.children.begin(), o.children.end(), [](const GameObject *c) { return c->is("StateManager"); });
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

// DE_CObject::ObjPath (Core 0x7003a880): the names from the root down, "\" between.
std::string path_of(const GameObject *o) {
	std::string p;
	for (; o; o = o->parent) p = o->name + (p.empty() ? "" : "\\" + p);
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
		if (f.id & 0x40) id += path_of(&o) + '\0';
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

// DE_CSession::Add + DE_CSessionRecord::Add (Core 0x701f9520 / 0x70203610).
StateManager &join(World &w, GameObject &o) {
	auto &R = rw(w);
	StateManager &sm = R.managers[o.handle];
	if (sm.owner == &o) return sm;
	sm = StateManager{};
	sm.owner = &o;
	sm.id = build_id(w, o);
	sm.joined = ++R.joins;
	if (auto &rec = R.record; rec && !sm.id.empty()) {
		sm.record = rec->find(sm.id);
		if (!sm.record && rec->recording) {
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
// a v1 chunk of a kind byte (0 an object no manager owns, 1 known, 2 null) and, when known, the
// ID of the newest manager it owns.
void add_element(World &w, Packer &p, uint32_t handle) {
	GameObject *o = handle ? w.object(handle) : nullptr;
	const StateManager *best = nullptr;
	if (o) {
		for (auto &[h, sm] : rw(w).managers) {
			if (sm.owner == o && (!best || sm.joined > best->joined)) best = &sm;
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
	if (p.r8() == 1) {
		auto b = p.rbytes();
		std::string id(b.begin(), b.end());
		uint64_t newest = 0;
		for (auto &[h, sm] : rw(w).managers) {
			if (sm.id == id && w.object(h) == sm.owner && sm.joined > newest) newest = sm.joined, found = h;
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
	case Value::Data:  // ours holds a packed hit outside script memory: not carried
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

// DE_CFighterPhysics::PackState (OMF 0x3503a5d0): a u16 mask, then momentum (0x10), acceleration
// (0x08), the position, the heading as a u16 turn, gravity and its recovery (0x04), turn rate
// (0x100), forward speed (0x200), last hit (0x01) and hit by (0x02), the guide (0x0c00: its mode in
// the mask, power, frames) and the invulnerable frames (0x1000).
void pack_physics(World &w, Packer &p, GameObject &o, bool got_hit) {
	auto &fs = w.forces->surfaces(o);
	uint16_t m = 0;
	// Below these it zeroes the live values too.
	if (length3(o.momentum) > 0.01f) m |= 0x10;
	else o.momentum = {0, 0, 0};
	if (length3(fs.accel) > 0.001f) m |= 0x08;
	else fs.accel = {0, 0, 0};
	if (o.skip_friction || o.propelled) m |= 0x20;
	if (fs.skip_push) m |= 0x40;
	if (o.on_floor) m |= 0x80;
	if (o.gravity != 1.0f) m |= 0x04;
	bool guide = o.guide_frames >= 0;
	if (o.guide_mode == 1) {
		if (o.last_hit) m |= 0x01;
		else guide = false;
	}
	// As the binary tests it: mode 2 sends the hit-by only while there is none.
	if ((o.guide_mode == 2 && !o.last_hit_by) || got_hit) m |= 0x02;
	if (o.turn_rate != 0) m |= 0x100;
	if (o.forward_speed != 0) m |= 0x200;
	if (guide) m |= uint16_t((o.guide_mode & 3) << 10);
	if (o.invulnerable_frames) m |= 0x1000;
	p.u16(m);
	if (m & 0x10) p.raw(o.momentum.data(), 12);
	if (m & 0x08) p.raw(fs.accel.data(), 12);
	p.raw(o.local.t.data(), 12);
	const auto &r = o.local.r;
	constexpr double kPi = 3.14159265358979323846;
	double turn = (std::atan2(double(r[0]), double(r[2])) + kPi + kPi / 2) / (2 * kPi) * 65536.0;
	p.u16(uint16_t(int64_t(turn)));
	if (m & 0x04) {
		p.f32(o.gravity);
		p.f32(o.gravity_recover);
	}
	if (m & 0x100) p.f32(o.turn_rate);
	if (m & 0x200) p.f32(o.forward_speed);
	if (m & 0x01) add_element(w, p, o.last_hit ? o.last_hit->handle : 0);
	if (m & 0x02) add_element(w, p, o.last_hit_by ? o.last_hit_by->handle : 0);
	if (m & 0x0c00) {
		p.f32(o.guide_power);
		p.squeezed(uint32_t(o.guide_frames));
	}
	if (o.invulnerable_frames) p.squeezed(uint32_t(o.invulnerable_frames));
}

// UnpackState (OMF 0x3503a9d0) sets each part outright (absent: its default; the heading as a pure
// yaw); DE_COMF2Fighter::UnpackState (0x3502a500) then puts the actor where the physics is.
void unpack_physics(World &w, Packer &p, GameObject &o) {
	auto &fs = w.forces->surfaces(o);
	uint16_t m = p.r16();
	o.momentum = {0, 0, 0};
	fs.accel = {0, 0, 0};
	if (m & 0x10) p.take(o.momentum.data(), 12);
	if (m & 0x08) p.take(fs.accel.data(), 12);
	p.take(o.local.t.data(), 12);
	constexpr double kPi = 3.14159265358979323846;
	double th = p.r16() / 65536.0 * 2 * kPi + kPi / 65536.0;
	float c = float(std::cos(th)), s = float(std::sin(th));
	o.local.r = {c, 0, -s, 0, 1, 0, s, 0, c};
	o.gravity = o.gravity_recover = 1.0f;
	if (m & 0x04) {
		o.gravity = p.rf();
		o.gravity_recover = p.rf();
	}
	o.skip_friction = (m & 0x20) != 0;
	fs.skip_push = (m & 0x40) != 0;
	o.on_floor = (m & 0x80) != 0;
	o.turn_rate = m & 0x100 ? p.rf() : 0;
	o.forward_speed = m & 0x200 ? p.rf() : 0;
	auto actor = [&](GameObject *&ref) {
		GameObject *a = w.object(extract_element(w, p));
		ref = a && a->is("Actor") ? a : nullptr;
	};
	if (m & 0x01) actor(o.last_hit);
	if (m & 0x02) actor(o.last_hit_by);
	o.guide_power = 0;
	o.guide_frames = -1;
	if (m & 0x0c00) {
		o.guide_power = p.rf();
		o.guide_frames = int(p.rsqueezed());
	}
	o.guide_mode = (m >> 10) & 3;
	o.invulnerable_frames = m & 0x1000 ? int(p.rsqueezed()) : 0;
}
// The creature's physics (+0x1cc): a fighter's, else its first simple physics.
GameObject *simple_physics_of(World &w, GameObject &o) {
	auto ps = w.simple_physics->physics_of(o);
	return ps.empty() ? nullptr : ps[0];
}

// DE_CObject::PackState (Core 0x7003de40) / DE_CCreature::PackState (Creature 0x150150b0): each
// executor instance's status back to back; for a creature then its position (+0x300), its
// orientation as three angles (+0x304) and its physics' own state.
void pack_state(World &w, Packer &p, GameObject &o, StateManager &sm) {
	for (Brain *b : brains_of(w, o)) generate_status(w, p, *b);
	if (!o.is("Actor\\Creature")) return;
	CreatureFlags f = creature_flags(o);
	if (f.position) p.raw(o.local.t.data(), 12);
	if (f.orientation) {
		auto a = decompose(o.local.r);
		p.raw(a.data(), 12);
	}
	if (o.fighter) {
		pack_physics(w, p, o, sm.got_hit);
		sm.got_hit = false;  // DE_COMF2Fighter::PackState (OMF 0x3502a4e0)
	} else if (GameObject *ph = simple_physics_of(w, o)) {
		// DE_CSimplePhysics::PackState (Core 0x701dc5b0).
		auto &s = w.simple_physics->state(*ph);
		if (s.pack_momentum) p.raw(s.momentum.data(), 12);
	}
}

// DE_CObject::UnpackState (Core 0x7003de80) / DE_CCreature::UnpackState (Creature 0x15015160):
// every status first, then each restored instance whose executor runs states at once does so.
void unpack_state(World &w, Packer &p, GameObject &o) {
	std::vector<Brain *> run;
	for (Brain *b : brains_of(w, o)) {
		if (process_status(w, p, *b) && executes_states_instantly(w, *b)) run.push_back(b);
	}
	for (Brain *b : run) {
		if (!b->exec || !b->exec->running() || b->exec->executing() || b->inst.shut_down) continue;
		try {
			b->exec->pulse();
		} catch (const std::exception &e) {
			w.log.push_back(o.name + ": " + e.what());
			b->inst.shut_down = true;
		}
	}
	if (!o.is("Actor\\Creature")) return;
	CreatureFlags f = creature_flags(o);
	if (f.position) p.take(o.local.t.data(), 12);
	if (f.orientation) {
		std::array<float, 3> a;
		p.take(a.data(), 12);
		o.local.r = compose(a);
	}
	if (o.fighter) {
		unpack_physics(w, p, o);
	} else if (GameObject *ph = simple_physics_of(w, o)) {
		auto &s = w.simple_physics->state(*ph);
		if (s.pack_momentum) p.take(s.momentum.data(), 12);
	}
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

void forget(World &w) {
	auto &R = rw(w);
	if (R.record && !R.record->recording) {
		w.time_scale = 1;  // the VCR's scale goes with the replay
		w.passed_ms = 0;
		for (Brain *b : w.brains()) b->inst.controlled = true;
	}
	R.prepared.clear();
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
		for (auto &m : r.managers) states += m->states.size();
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
	if (getenv("OPENBG_DEBUG_REPLAY") && w.frame() % 30 == 0) {
		for (GameObject *o : w.scene_objects()) {
			if (o->fighter) fprintf(stderr, "[fighter %u ms] %s %.1f %.1f %.1f\n", now, o->name.c_str(), o->local.t[0], o->local.t[1], o->local.t[2]);
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
		// DE_CCreature::RewindTime (Creature 0x15015eb0): a creature holds still until a state comes.
		for (auto &[h, sm] : R.managers) sm.rewound = sm.replay && sm.owner->is("Actor\\Creature");
	}
	R.last_ms = now;
	for (GameObject *o : w.scene_objects()) R.prepared.emplace(o->handle, now);
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
		sm.rewound = false;
		if (getenv("OPENBG_DEBUG_REPLAY")) fprintf(stderr, "[replay %u] %s takes its %u ms state\n", w.frame(), sm.owner->name.c_str(), st[c].first);
		Packer p;
		p.data = st[c].second;
		unpack_state(w, p, *sm.owner);
	}
}

void register_state(World &w, GameObject *owner, bool got_hit) {
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
	pack_state(w, p, *o, sm);
	// RegisterGetHitState (OMF 0x350296b0) flags the fighter after registering.
	sm.got_hit |= got_hit;
	auto &st = sm.record->states;
	// AddToStateList / Record: a state at the same time replaces the last.
	if (!st.empty() && st.back().first == t) st.back().second = std::move(p.data);
	else st.emplace_back(t, std::move(p.data));
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

bool replay_held(World &w, const GameObject &o) {
	StateManager *sm = manager_of(w, &o);
	return sm && sm->rewound;
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
