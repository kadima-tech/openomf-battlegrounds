// Script value classes the session scripts lean on: generic events, GUIDs and GUID pools,
// reference/primitive arrays, object values (Set/Get*Value and Value\* children), text.
//
// Native value-class instances are kNativeInstanceSize blocks (World::make_instance). Layouts
// here are this engine's own (scripts only see them through these natives):
//   CGenericEvent  +0 handle (interned name), +4 INT1, +8..+20 Object1..4, +24 INT2, +28 INT3,
//                  +32..+40 FLOAT1..3, +44 text (string)
//   CGUID          +0 16 bytes
//   CGUIDPool      +0 data, +4 count
//   arrays         +0 data, +4 length, +8 capacity (4-byte elements)
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "game/session.h"
#include "game/impulses.h"
#include "game/descriptions.h"

#include "de/value.h"

namespace game {

namespace {

constexpr uint32_t kHandle = 0, kInt1 = 4, kObject1 = 8, kInt2 = 24, kInt3 = 28, kFloat1 = 32, kText = 44;

uint32_t interned(World &w, uint32_t handle_arg) {
	// A CNamedHandle is a value instance holding its name ("final CNamedHandle g_X = "...""); a
	// handle can also arrive as the interned name an event already holds.
	if (!handle_arg) return 0;
	uint32_t p = w.memory().get<uint32_t>(handle_arg);
	std::string s = p ? w.read_string(p) : std::string();
	return s.empty() ? 0 : w.string(s);
}

// Arrays: grow to `n` elements (zero-filled), reallocating in VM memory.
void ensure_length(vm::Memory &m, uint32_t a, uint32_t n) {
	uint32_t len = m.get<uint32_t>(a + 4), cap = m.get<uint32_t>(a + 8);
	if (n > cap) {
		uint32_t ncap = std::max<uint32_t>(n, std::max<uint32_t>(8, cap * 2));
		uint32_t nd = m.alloc(ncap * 4);
		if (len) memcpy(m.ptr(nd, len * 4), m.ptr(m.get<uint32_t>(a), len * 4), len * 4);
		m.set<uint32_t>(a, nd);
		m.set<uint32_t>(a + 8, ncap);
	}
	if (n > len) {
		memset(m.ptr(m.get<uint32_t>(a) + len * 4, (n - len) * 4), 0, (n - len) * 4);
		m.set<uint32_t>(a + 4, n);
	}
}
std::vector<uint32_t> elements(vm::Memory &m, uint32_t a) {
	std::vector<uint32_t> v(m.get<uint32_t>(a + 4));
	for (size_t i = 0; i < v.size(); ++i) v[i] = m.get<uint32_t>(m.get<uint32_t>(a) + uint32_t(i) * 4);
	return v;
}
void assign(vm::Memory &m, uint32_t a, const std::vector<uint32_t> &v) {
	m.set<uint32_t>(a + 4, 0);
	ensure_length(m, a, uint32_t(v.size()));
	for (size_t i = 0; i < v.size(); ++i) m.set<uint32_t>(m.get<uint32_t>(a) + uint32_t(i) * 4, v[i]);
}

// Value\* data children: [name chunk][payload chunk]; FLOAT/INT/BOOL payload 4 bytes, String a
// length-prefixed string; ObjectReference its references.
GameObject *value_child(GameObject *o, const std::string &name) {
	if (!o) return nullptr;
	for (auto *ch : o->children) {
		if (ch->name == name && ch->is("Value")) return ch;
	}
	return nullptr;
}
std::optional<std::vector<uint8_t>> value_payload(const GameObject *v) {
	if (!v || !v->file) return std::nullopt;
	try {
		de::Contents c(v->file->object_data(v->file->objects()[v->index]).contents);
		de::Chunk a = de::begin_chunk(c);
		c.seek(a.end);
		de::Chunk b = de::begin_chunk(c);
		auto bytes = c.bytes(b.end - c.pos());
		return std::vector<uint8_t>(bytes.begin(), bytes.end());
	} catch (const std::exception &) {
		return std::nullopt;
	}
}

}  // namespace

// Object values: Set*Value at run time, else the Value\\* data child of that name. A session
// looks in its info first (DE_CSession::ObjFindValueByName, Core 0x20202bd0, 2.112).
World::ObjectValue *World::object_value(GameObject *o, const std::string &name) {
	if (!o) return nullptr;
	if (SessionState *s = session_of(*this, o); s && s->obj == o && s->description) {
		if (ObjectValue *v = object_value(s->description, name)) return v;
	}
	auto &m = values[o];
	auto it = m.find(name);
	if (it != m.end()) return &it->second;
	if (GameObject *v = value_child(o, name)) {
		if (auto p = value_payload(v)) {
			World::ObjectValue val;
			if (v->is("Value\\String")) {
				de::Contents cc(*p);
				uint32_t n = cc.u32();
				auto b = cc.bytes(n);
				val.text.assign(b.begin(), b.end());
				while (!val.text.empty() && val.text.back() == 0) val.text.pop_back();
			} else if (v->is("Value\\ObjectReference")) {
				// references of the value object
				for (auto &lists : reference_lists(data(), de::ObjectRef{v->file, v->index})) {
					for (auto &l : lists) {
						for (auto &r : l) {
							if (GameObject *t = global_object(r)) val.objects.push_back(t->handle);
						}
					}
				}
			} else if (p->size() >= 4) {
				memcpy(&val.bits, p->data(), 4);
			}
			return &(m[name] = val);
		}
	}
	return nullptr;
}

// The application's music player: Music.DEObjects' "MusicPlayer" actor (a speaker and the
// BMusicPlayer behaviour), put in the scene once.
GameObject *World::music_player_actor() {
	if (!music_player) {
		if (const de::ObjectsFile *f = data().file("Music")) {
			for (uint32_t i = 0; i < f->objects().size(); ++i) {
				const auto &o = f->objects()[i];
				if (o.name != "MusicPlayer" || o.class_path.rfind("Actor", 0) != 0) continue;
				if (GameObject *tpl = global_object({f, i})) {
					music_player = copy_tree(*tpl);
					root_session[music_player] = nullptr;
					enter_scene(music_player);
				}
				break;
			}
		}
		if (!music_player) music_player = create("MusicPlayer", "Actor");
	}
	return music_player;
}

void register_script_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };
	// FindMasterObject(GUID): the data's object with that GUID (not a copy).
	reg("CObject::class CObject FindMasterObject(class CGUID)", [](const NativeContext &c) {
		uint32_t g = c.arg<uint32_t>(0);
		de::Guid guid;
		if (g) memcpy(guid.bytes, c.mem().ptr(g, 16), 16);
		c.ret_object(g ? c.world.global_object(c.world.data().by_guid(guid)) : nullptr);
	});
	reg("CEnvironmentDescription::int MinimumRecommendedPlayers()", [](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_u32(uint32_t(o && o->file ? environment_players({o->file, o->index}).first : 0));
	});
	reg("CEnvironmentDescription::int MaximumRecommendedPlayers()", [](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_u32(uint32_t(o && o->file ? environment_players({o->file, o->index}).second : 0));
	});
	reg("CObject::float MouseScreenPositionX()", [](const NativeContext &c) { c.ret_f32(c.world.mouse_x); });
	reg("CObject::float MouseScreenPositionY()", [](const NativeContext &c) { c.ret_f32(c.world.mouse_y); });
	reg("CObject::float ScreenAspectRatio()", [](const NativeContext &c) { c.ret_f32(c.world.screen_aspect); });
	reg("CDescription::Unlock(dword)", [](const NativeContext &c) {
		if (GameObject *o = c.self(); o && o->file) unlock_description(c.world, {o->file, o->index}, c.arg<uint32_t>(0));
	});
	// UnlockSinglePlayerMatchCaller (Core 0x201f8ae0, 2.112): Unlock(0x10063 + n), the id the Choose
	// Cast page's Starting Match list checks for match n.
	reg("CDescription::UnlockSinglePlayerMatch(dword)", [](const NativeContext &c) {
		if (GameObject *o = c.self(); o && o->file) unlock_description(c.world, {o->file, o->index}, 0x10063 + c.arg<uint32_t>(0));
	});
	// DE_CPilotDescription::UnlockStatDescriptions (OMF 0x35050cf0): shuffles the 22 stat ids with
	// the CRT's rand() and unlocks the first `n` still locked, walking from the end.
	reg("CPilotDescription::UnlockStatDescriptions(int)", [](const NativeContext &c) {
		GameObject *o = c.self();
		if (!o || !o->file) return;
		int ids[22];
		for (int k = 0; k < 22; ++k) ids[k] = k;
		for (int k = 21; k >= 0; --k) std::swap(ids[crt_rand() % 22], ids[k]);
		int n = int(c.arg<uint32_t>(0));
		de::ObjectRef d{o->file, o->index};
		for (int k = 21; k >= 0 && n > 0; --k) {
			if (!description_locked(c.world, d, uint32_t(ids[k] + 100))) continue;
			unlock_description(c.world, d, uint32_t(ids[k] + 100));
			--n;
		}
	});
	// IsUserSelectionValidCaller (Core 0x701f11d0): description vtable +0x130, true but for match
	// descriptions (DE_CMatchDescription::IsUserSelectionValid), which no shipped list gathers.
	reg("CDescription::bool IsUserSelectable()", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CDescription::bool IsLocked()", [](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_bool(o && o->file && description_locked(c.world, {o->file, o->index}));
	});
	auto stub = [&](const char *key) { w.register_native(key, [](const NativeContext &) {}); };

	// --- Generic events ---------------------------------------------------------------------------
	reg("CObject::bool IsGenericEvent(class CObject)", [](const NativeContext &c) {
		uint32_t e = c.arg<uint32_t>(0);
		c.ret_bool(e && c.world.instance_class(e) == "CGenericEvent");
	});
	reg("CGenericEvent::SetHandle(class CNamedHandle)", [](const NativeContext &c) {
		if (uint32_t e = c.self_handle()) c.mem().set<uint32_t>(e + kHandle, interned(c.world, c.arg<uint32_t>(0)));
	});
	// SetAsRegisteredEventCaller (Core): the handle registered under that name (here the interned
	// name, as CNamedHandle holds it).
	reg("CGenericEvent::SetAsRegisteredEvent(string)", [](const NativeContext &c) {
		std::string n = c.arg_string(0);
		if (uint32_t e = c.self_handle()) c.mem().set<uint32_t>(e + kHandle, n.empty() ? 0 : c.world.string(n));
	});
	reg("CGenericEvent::bool IsHandle(class CNamedHandle)", [](const NativeContext &c) {
		uint32_t e = c.self_handle();
		static const bool debug = getenv("OPENBG_DEBUG_EVENTS") != nullptr;
		if (debug) {
			uint32_t h = e ? c.mem().get<uint32_t>(e + kHandle) : 0, q = interned(c.world, c.arg<uint32_t>(0));
			fprintf(stderr, "[event] IsHandle %s == %s\n", h ? c.world.read_string(h).c_str() : "-", q ? c.world.read_string(q).c_str() : "-");
		}
		c.ret_bool(e && c.mem().get<uint32_t>(e + kHandle) && c.mem().get<uint32_t>(e + kHandle) == interned(c.world, c.arg<uint32_t>(0)));
	});
	reg("CGenericEvent::CNamedHandle::(opp =)(class CNamedHandle)", [](const NativeContext &c) {
		if (uint32_t t = c.self_handle()) c.mem().set<uint32_t>(t, c.arg<uint32_t>(0) ? c.mem().get<uint32_t>(c.arg<uint32_t>(0)) : 0);
	});
	auto field = [&](const std::string &name, uint32_t off, const char *type) {
		std::string t = type;
		reg("CGenericEvent::Set" + name + "(" + t + ")", [off](const NativeContext &c) {
			if (uint32_t e = c.self_handle()) c.mem().set<uint32_t>(e + off, c.arg<uint32_t>(0));
		});
		std::string ret = t == "class CReferenceTarget" ? "class CReferenceTarget " : (t + " ");
		reg("CGenericEvent::" + ret + name + "()", [off](const NativeContext &c) {
			uint32_t e = c.self_handle();
			c.ret_u32(e ? c.mem().get<uint32_t>(e + off) : 0);
		});
	};
	field("INT1", kInt1, "int");
	field("INT2", kInt2, "int");
	field("INT3", kInt3, "int");
	for (int i = 0; i < 4; ++i) field("Object" + std::to_string(i + 1), kObject1 + uint32_t(i) * 4, "class CReferenceTarget");
	for (int i = 0; i < 3; ++i) field("FLOAT" + std::to_string(i + 1), kFloat1 + uint32_t(i) * 4, "float");
	field("Text", kText, "string");
	// CEvent::SendToNow: deliver now to the target's relays. Without a session, requests a
	// fighter sends the "Session" object are granted (INT1 = 1) as before.
	reg("CEvent::SendToNow(class CObject)", [](const NativeContext &c) {
		uint32_t e = c.self_handle();
		GameObject *t = c.arg_object(0);
		if (!e || !t) return;
		if (!c.world.session && t->name == "Session") {
			c.mem().set<int32_t>(e + kInt1, 1);
			return;
		}
		static const char *dbg = getenv("OPENBG_DEBUG_SENDS");
		int32_t before = c.mem().get<int32_t>(e + kInt1);
		c.world.send_event(t, e);
		if (dbg) {
			std::string h = c.world.read_string(c.mem().get<uint32_t>(e + kHandle));
			if (h.find(dbg) != std::string::npos) fprintf(stderr, "[send %u] %s -> %s/%s INT1 %d -> %d\n", c.world.frame(), h.c_str(), t->parent ? t->parent->name.c_str() : "-", t->name.c_str(), before, c.mem().get<int32_t>(e + kInt1));
		}
	});
	// ProcessEventCaller (Core 0x70046230): the object's own ProcessEvent (vtable +0x70). For an
	// actor (DE_CActor::ProcessEvent 0x70090ed0) that is each property's in turn (the script
	// relays among them), then DE_CObject::ProcessEvent, which only handles the editor's drag and
	// drop events. DE_COMF2Fighter::ProcessEvent's own switch forwards OMF2 event types 1-10 to its
	// recipient lists; the arena surface hit (type 0xb) the projectiles send is not among them.
	reg("CObject::ProcessEvent(class CEvent)", [](const NativeContext &c) {
		if (GameObject *o = c.self()) c.world.send_event(o, c.arg<uint32_t>(0));
	});

	// --- GUIDs ------------------------------------------------------------------------------------
	reg("CObject::class CGUID GUID()", [](const NativeContext &c) {
		uint32_t g = c.world.make_instance("CObject::CGUID", 1);
		if (auto *o = c.self(); o && o->file) memcpy(c.mem().ptr(g, 16), o->file->objects()[o->index].guid.bytes, 16);
		c.ret_u32(g);
	});
	reg("CObject::CGUID::(opp =)(class CGUID)", [](const NativeContext &c) {
		uint32_t d = c.self_handle(), s = c.arg<uint32_t>(0);
		if (d) {
			if (s) memcpy(c.mem().ptr(d, 16), c.mem().ptr(s, 16), 16);
			else memset(c.mem().ptr(d, 16), 0, 16);
		}
	});
	reg("CObject::CGUID::Clear()", [](const NativeContext &c) { if (uint32_t d = c.self_handle()) memset(c.mem().ptr(d, 16), 0, 16); });
	reg("CObject::CGUID::bool IsSet()", [](const NativeContext &c) {
		uint32_t d = c.self_handle();
		bool set = false;
		for (uint32_t i = 0; d && i < 16; ++i) set |= c.mem().get<uint8_t>(d + i) != 0;
		c.ret_bool(set);
	});
	auto same = [](const NativeContext &c) {
		uint32_t a = c.self_handle(), b = c.arg<uint32_t>(0);
		if (!a || !b) return a == b;
		return memcmp(c.mem().ptr(a, 16), c.mem().ptr(b, 16), 16) == 0;
	};
	reg("CObject::CGUID::bool IsSame(class CGUID)", [same](const NativeContext &c) { c.ret_bool(same(c)); });
	reg("CObject::CGUID::bool (opp ==)(class CGUID)", [same](const NativeContext &c) { c.ret_bool(same(c)); });
	reg("CObject::CGUID::bool (opp !=)(class CGUID)", [same](const NativeContext &c) { c.ret_bool(!same(c)); });
	// GUID pools: 16-byte entries.
	auto pool = [](const NativeContext &c) {
		std::vector<std::array<uint8_t, 16>> v;
		uint32_t p = c.self_handle();
		if (!p) return v;
		uint32_t data = c.mem().get<uint32_t>(p), n = c.mem().get<uint32_t>(p + 4);
		for (uint32_t i = 0; i < n; ++i) {
			std::array<uint8_t, 16> g;
			memcpy(g.data(), c.mem().ptr(data + i * 16, 16), 16);
			v.push_back(g);
		}
		return v;
	};
	auto set_pool = [](const NativeContext &c, const std::vector<std::array<uint8_t, 16>> &v) {
		uint32_t p = c.self_handle();
		if (!p) return;
		uint32_t data = c.mem().alloc(uint32_t(std::max<size_t>(1, v.size())) * 16);
		for (size_t i = 0; i < v.size(); ++i) memcpy(c.mem().ptr(data + uint32_t(i) * 16, 16), v[i].data(), 16);
		c.mem().set<uint32_t>(p, data);
		c.mem().set<uint32_t>(p + 4, uint32_t(v.size()));
	};
	reg("CObject::CGUIDPool::(opp +=)(class CGUID)", [pool, set_pool](const NativeContext &c) {
		auto v = pool(c);
		std::array<uint8_t, 16> g{};
		if (uint32_t a = c.arg<uint32_t>(0)) memcpy(g.data(), c.mem().ptr(a, 16), 16);
		v.push_back(g);
		set_pool(c, v);
	});
	reg("CObject::CGUIDPool::(opp -)(class CGUID)", [pool, set_pool](const NativeContext &c) {
		auto v = pool(c);
		std::array<uint8_t, 16> g{};
		if (uint32_t a = c.arg<uint32_t>(0)) memcpy(g.data(), c.mem().ptr(a, 16), 16);
		v.erase(std::remove(v.begin(), v.end(), g), v.end());
		set_pool(c, v);
	});
	reg("CObject::CGUIDPool::(opp =)(class CGUIDPool)", [set_pool](const NativeContext &c) {
		std::vector<std::array<uint8_t, 16>> v;
		if (uint32_t o = c.arg<uint32_t>(0)) {
			uint32_t data = c.mem().get<uint32_t>(o), n = c.mem().get<uint32_t>(o + 4);
			for (uint32_t i = 0; i < n; ++i) {
				std::array<uint8_t, 16> g;
				memcpy(g.data(), c.mem().ptr(data + i * 16, 16), 16);
				v.push_back(g);
			}
		}
		set_pool(c, v);
	});
	reg("CObject::CGUIDPool::int Num()", [pool](const NativeContext &c) { c.ret_u32(uint32_t(pool(c).size())); });

	// --- Arrays -----------------------------------------------------------------------------------
	for (const char *cls : {"CObject::CSingleDimensionPrimitiveArray::", "CObject::CSingleDimensionReferenceArray::"}) {
		std::string k = cls;
		reg(k + "dword <ArrayRead>(int)", [](const NativeContext &c) {
			uint32_t a = c.self_handle();
			int i = c.arg<int32_t>(0);
			if (!a || i < 0 || uint32_t(i) >= c.mem().get<uint32_t>(a + 4)) {
				c.ret_u32(0);
				return;
			}
			c.ret_u32(c.mem().get<uint32_t>(c.mem().get<uint32_t>(a) + uint32_t(i) * 4));
		});
		reg(k + "dword <ArrayElementPosition>(int)", [](const NativeContext &c) {
			uint32_t a = c.self_handle();
			int i = c.arg<int32_t>(0);
			if (!a || i < 0) {
				c.ret_u32(0);
				return;
			}
			ensure_length(c.mem(), a, uint32_t(i) + 1);
			c.ret_u32(c.mem().get<uint32_t>(a) + uint32_t(i) * 4);
		});
		reg(k + "AssureLength(int)", [](const NativeContext &c) {
			if (uint32_t a = c.self_handle(); a && c.arg<int32_t>(0) > 0) ensure_length(c.mem(), a, uint32_t(c.arg<int32_t>(0)));
		});
		reg(k + "int Length()", [](const NativeContext &c) {
			uint32_t a = c.self_handle();
			c.ret_u32(a ? c.mem().get<uint32_t>(a + 4) : 0);
		});
		reg(k + "Reset()", [](const NativeContext &c) { if (uint32_t a = c.self_handle()) c.mem().set<uint32_t>(a + 4, 0); });
		reg(k + "<ArrayElementSet>(int,class CReferenceTarget)", [](const NativeContext &c) {
			uint32_t a = c.self_handle();
			int i = c.arg<int32_t>(0);
			if (!a || i < 0) return;
			ensure_length(c.mem(), a, uint32_t(i) + 1);
			c.mem().set<uint32_t>(c.mem().get<uint32_t>(a) + uint32_t(i) * 4, c.arg<uint32_t>(1));
		});
	}
	const std::string ra = "CObject::CSingleDimensionReferenceArray::";
	auto ref_add = [](const NativeContext &c, bool if_absent) {
		uint32_t a = c.self_handle(), v = c.arg<uint32_t>(0);
		if (!a) return;
		auto e = elements(c.mem(), a);
		if (if_absent && std::find(e.begin(), e.end(), v) != e.end()) return;
		e.push_back(v);
		assign(c.mem(), a, e);
	};
	reg(ra + "(opp +=)(class CReferenceTarget)", [ref_add](const NativeContext &c) { ref_add(c, false); });
	reg(ra + "AppendIfAbsent(class CReferenceTarget)", [ref_add](const NativeContext &c) { ref_add(c, true); });
	reg(ra + "(opp -)(class CReferenceTarget)", [](const NativeContext &c) {
		uint32_t a = c.self_handle(), v = c.arg<uint32_t>(0);
		if (!a) return;
		auto e = elements(c.mem(), a);
		e.erase(std::remove(e.begin(), e.end(), v), e.end());
		assign(c.mem(), a, e);
	});
	// SingleDimensionReferenceShuffleCaller (Core 0x7014d220): from the last element down, each
	// swapped with one picked by the CRT rand() stream from the whole array.
	reg(ra + "Shuffle()", [](const NativeContext &c) {
		uint32_t a = c.self_handle();
		if (!a) return;
		auto e = elements(c.mem(), a);
		for (int i = int(e.size()) - 1; i >= 0; --i) std::swap(e[size_t(i)], e[size_t(crt_rand() % int(e.size()))]);
		assign(c.mem(), a, e);
	});
	reg(ra + "bool IsPresent(class CReferenceTarget)", [](const NativeContext &c) {
		uint32_t a = c.self_handle(), v = c.arg<uint32_t>(0);
		auto e = a ? elements(c.mem(), a) : std::vector<uint32_t>{};
		c.ret_bool(v && std::find(e.begin(), e.end(), v) != e.end());
	});
	// ClearInactive: drop references to objects that no longer exist.
	reg(ra + "ClearInactive()", [](const NativeContext &c) {
		uint32_t a = c.self_handle();
		if (!a) return;
		auto e = elements(c.mem(), a);
		e.erase(std::remove_if(e.begin(), e.end(), [&](uint32_t h) { return !c.world.object(h); }), e.end());
		assign(c.mem(), a, e);
	});
	reg(ra + "SortByName()", [](const NativeContext &c) {
		uint32_t a = c.self_handle();
		if (!a) return;
		auto e = elements(c.mem(), a);
		std::stable_sort(e.begin(), e.end(), [&](uint32_t x, uint32_t y) {
			auto *ox = c.world.object(x), *oy = c.world.object(y);
			return (ox ? ox->name : "") < (oy ? oy->name : "");
		});
		assign(c.mem(), a, e);
	});
	// Gather*: every in-scene object of a class (classtype argument: the class handle's path).
	// GatherAllObjectsDerivedFromCaller (Core 0x7014d350): DE_CTransferAgent::FindObjectsDerivedFrom
	// (0x7005b880) walks the agent list from its last agent back, each agent's object infos in
	// table order (FindDerivedFrom, ProcessEvent case 9), the GUIDs once each; then every master
	// object. The file agents join the list as DE_CTransferAgentWinFileFactory::Init finds them,
	// in the directory's order (taken as NTFS's: names compared upper-cased), after the modules'.
	reg(ra + "GatherAllObjectsDerivedFrom(classtype )", [](const NativeContext &c) {
		uint32_t a = c.self_handle();
		std::string path = c.world.class_path_of_handle(c.arg<uint32_t>(0));
		if (!a || path.empty()) return;
		auto e = elements(c.mem(), a);
		std::vector<const de::ObjectsFile *> agents;
		{
			std::vector<std::pair<std::string, const de::ObjectsFile *>> on_disk;
			for (const auto &[stem, f] : c.world.data().files()) {
				std::string name = stem + ".DEObjects";
				if (!std::filesystem::exists(c.world.data().dir() / name)) {
					agents.push_back(f.get());
					continue;
				}
				for (char &ch : name) ch = char(toupper(uint8_t(ch)));
				on_disk.push_back({name, f.get()});
			}
			std::sort(on_disk.begin(), on_disk.end());
			for (auto &[name, f] : on_disk) agents.push_back(f);
		}
		for (auto it = agents.rbegin(); it != agents.rend(); ++it) {
			const de::ObjectsFile *f = *it;
			for (uint32_t i = 0; i < f->objects().size(); ++i) {
				const std::string &cp = f->objects()[i].class_path;
				if (cp != path && cp.rfind(path + "\\", 0) != 0) continue;
				GameObject *o = c.world.global_object({f, i});
				if (o && std::find(e.begin(), e.end(), o->handle) == e.end()) e.push_back(o->handle);
			}
		}
		assign(c.mem(), a, e);
	});
	reg(ra + "GatherAllActorsWithProperty(class CScene,classtype )", [](const NativeContext &c) {
		uint32_t a = c.self_handle();
		uint32_t cls = c.arg<uint32_t>(1);
		if (!a) return;
		auto e = elements(c.mem(), a);
		int view = World::gui_scene_view(c.arg_object(0));
		for (GameObject *o : c.world.scene_objects()) {
			if (view >= 0 ? !in_gui(*o) || gui_view(*o) != view : in_gui(*o)) continue;
			if (o->is_actor && c.world.has_behavior_property(o, cls)) e.push_back(o->handle);
		}
		assign(c.mem(), a, e);
	});

	// --- Object values (Set*Value at run time; Value\* children from the data) ------------------
	auto value = [](const NativeContext &c, const std::string &name) { return c.world.object_value(c.self(), name); };
	// DE_CObject::SetValue (Core 0x200406d0): the value ObjFindValueByName finds is set, a new one
	// is made on the object itself.
	auto holder = [](const NativeContext &c) -> GameObject * {
		GameObject *o = c.self();
		SessionState *s = o ? session_of(c.world, o) : nullptr;
		if (s && s->obj == o && s->description && c.world.object_value(s->description, c.arg_string(0))) return s->description;
		return o;
	};
	auto set_bits = [holder](const NativeContext &c, uint32_t bits) {
		if (GameObject *o = holder(c)) c.world.values[o][c.arg_string(0)].bits = bits;
	};
	reg("CObject::SetBOOLValue(string,bool)", [set_bits](const NativeContext &c) { set_bits(c, c.arg<uint32_t>(1) ? 1 : 0); });
	reg("CObject::SetINTValue(string,int)", [set_bits](const NativeContext &c) { set_bits(c, c.arg<uint32_t>(1)); });
	reg("CObject::SetFLOATValue(string,float)", [set_bits](const NativeContext &c) { set_bits(c, c.arg<uint32_t>(1)); });
	reg("CObject::SetValue(string,string)", [holder](const NativeContext &c) {
		if (GameObject *o = holder(c)) c.world.values[o][c.arg_string(0)].text = c.arg_string(1);
	});
	reg("CObject::SetValue(string,class CText)", [holder](const NativeContext &c) {
		uint32_t t = c.arg<uint32_t>(1);
		if (GameObject *o = holder(c)) c.world.values[o][c.arg_string(0)].text = t ? c.world.read_string(c.mem().get<uint32_t>(t)) : std::string();
	});
	reg("CObject::SetObjectReferenceValue(string,class CObject)", [holder](const NativeContext &c) {
		if (GameObject *o = holder(c)) c.world.values[o][c.arg_string(0)].objects.push_back(c.arg<uint32_t>(1));
	});
	reg("CObject::ClearAllObjectReferenceValues(string)", [holder](const NativeContext &c) {
		if (GameObject *o = holder(c)) c.world.values[o][c.arg_string(0)].objects.clear();
	});
	reg("CObject::bool GetBOOLValue(string)", [value](const NativeContext &c) {
		auto *v = value(c, c.arg_string(0));
		c.ret_bool(v && v->bits);
	});
	reg("CObject::int GetINTValue(string)", [value](const NativeContext &c) {
		auto *v = value(c, c.arg_string(0));
		c.ret_u32(v ? v->bits : 0);
	});
	reg("CObject::float GetFLOATValue(string)", [value](const NativeContext &c) {
		auto *v = value(c, c.arg_string(0));
		c.ret_u32(v ? v->bits : 0);
	});
	// GetFLOATOrINTValue / GetINTOrFLOATValue: a FLOAT or INT value, converted.
	auto is_int = [](const NativeContext &c, const std::string &name) {
		GameObject *v = value_child(c.self(), name);
		return v && v->is("Value\\INT");
	};
	reg("CObject::float GetFLOATOrINTValue(string)", [value, is_int](const NativeContext &c) {
		std::string name = c.arg_string(0);
		auto *v = value(c, name);
		if (!v) {
			c.ret_f32(0);
		} else if (is_int(c, name)) {
			c.ret_f32(float(int32_t(v->bits)));
		} else {
			c.ret_u32(v->bits);
		}
	});
	reg("CObject::int GetINTOrFLOATValue(string)", [value, is_int](const NativeContext &c) {
		std::string name = c.arg_string(0);
		auto *v = value(c, name);
		if (!v) {
			c.ret_u32(0);
		} else if (is_int(c, name)) {
			c.ret_u32(v->bits);
		} else {
			float f;
			memcpy(&f, &v->bits, 4);
			c.ret_u32(uint32_t(int32_t(f)));
		}
	});
	// CValueFLOAT::GetValue(t): the value object's curve (DE_CValueInterFLOAT::GetValue) at t.
	reg("CValueFLOAT::float GetValue(float)", [](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_f32(o && o->file ? de::read_float_value(c.world.data(), {o->file, o->index}).eval(c.arg<float>(0)) : 0);
	});
	// GetNormalizedLoopingValueCaller (0x7010f2e0): the same at t - trunc(t).
	reg("CValueFLOAT::float GetNormalizedLoopingValue(float)", [](const NativeContext &c) {
		GameObject *o = c.self();
		float t = c.arg<float>(0);
		t -= float(int32_t(t));
		c.ret_f32(o && o->file ? de::read_float_value(c.world.data(), {o->file, o->index}).eval(t) : 0);
	});
	reg("CValueFLOAT::float GetValue()", [](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_f32(o && o->file ? de::read_float_value(c.world.data(), {o->file, o->index}).eval(0) : 0);
	});
	reg("CObject::bool GetValue(string,class CText)", [value](const NativeContext &c) {
		auto *v = value(c, c.arg_string(0));
		if (v && c.arg<uint32_t>(1)) c.mem().set<uint32_t>(c.arg<uint32_t>(1), c.world.string(v->text));
		c.ret_bool(v != nullptr);
	});
	reg("CObject::int GetNumObjectReferenceValue(string)", [value](const NativeContext &c) {
		auto *v = value(c, c.arg_string(0));
		c.ret_u32(v ? uint32_t(v->objects.size()) : 0);
	});
	reg("CObject::class CObject GetObjectReferenceValue(string)", [value](const NativeContext &c) {
		auto *v = value(c, c.arg_string(0));
		c.ret_u32(v && !v->objects.empty() ? v->objects[0] : 0);
	});
	reg("CObject::class CObject GetObjectReferenceValue(string,int)", [value](const NativeContext &c) {
		auto *v = value(c, c.arg_string(0));
		int i = c.arg<int32_t>(1);
		c.ret_u32(v && i >= 0 && size_t(i) < v->objects.size() ? v->objects[size_t(i)] : 0);
	});

	// --- Text -----------------------------------------------------------------------------------
	auto text_of = [](const NativeContext &c) {
		uint32_t t = c.self_handle();
		return t ? c.world.read_string(c.mem().get<uint32_t>(t)) : std::string();
	};
	auto set_text = [](const NativeContext &c, const std::string &s) {
		if (uint32_t t = c.self_handle()) c.mem().set<uint32_t>(t, c.world.string(s));
	};
	// ReplaceCaller (Core 0x7014bd40): DE_CString::Replace, every occurrence.
	reg("CObject::CText::Replace(string,string)", [text_of, set_text](const NativeContext &c) {
		std::string t = text_of(c), from = c.arg_string(0), to = c.arg_string(1);
		if (from.empty()) return;
		for (size_t p = t.find(from); p != std::string::npos; p = t.find(from, p + to.size())) t.replace(p, from.size(), to);
		set_text(c, t);
	});
	reg("CObject::CText::int Length()", [text_of](const NativeContext &c) { c.ret_u32(uint32_t(text_of(c).size())); });
	// GetValue(name, text): a String value's text into the CText (left alone without one).
	reg("CObject::GetValue(string,class CText)", [](const NativeContext &c) {
		auto *v = c.world.object_value(c.self(), c.arg_string(0));
		uint32_t t = c.arg<uint32_t>(1);
		if (v && t) c.mem().set<uint32_t>(t, c.world.string(v->text));
	});
	reg("CObject::CText::(opp =)(int)", [set_text](const NativeContext &c) { set_text(c, std::to_string(c.arg<int32_t>(0))); });
	reg("CObject::CText::(opp +=)(int)", [text_of, set_text](const NativeContext &c) { set_text(c, text_of(c) + std::to_string(c.arg<int32_t>(0))); });
	reg("CObject::CText::bool (opp ==)(string)", [text_of](const NativeContext &c) { c.ret_bool(text_of(c) == c.arg_string(0)); });
	reg("CObject::CText::bool (opp !=)(string)", [text_of](const NativeContext &c) { c.ret_bool(text_of(c) != c.arg_string(0)); });
	reg("CObject::CText::bool CaseInsensitiveIsEqual(string)", [text_of](const NativeContext &c) {
		std::string a = text_of(c), b = c.arg_string(0);
		c.ret_bool(a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower(x) == tolower(y); }));
	});

	// --- Objects --------------------------------------------------------------------------------
	reg("CObject::SetName(string)", [](const NativeContext &c) { if (GameObject *o = c.self()) o->name = c.arg_string(0); });
	reg("CObject::class CObject TopParent()", [](const NativeContext &c) {
		GameObject *o = c.self();
		while (o && o->parent) o = o->parent;
		c.ret_object(o);
	});
	// ActorParentCaller (Core 0x7008e390): the actor's parent actor (+0xf8).
	reg("CActor::class CActor ActorParent()", [](const NativeContext &c) {
		GameObject *p = c.self() ? c.self()->parent : nullptr;
		while (p && !p->is_actor) p = p->parent;
		c.ret_object(p);
	});
	reg("CActor::class CActor TopActorParent()", [](const NativeContext &c) {
		GameObject *o = c.self(), *top = o;
		for (GameObject *p = o; p; p = p->parent) {
			if (p->is_actor) top = p;
		}
		c.ret_object(top);
	});
	reg("CObject::dword SystemTime()", [](const NativeContext &c) { c.ret_u32(c.world.system_ms()); });
	// DeleteThis: detach from the tree; references to it read as inactive.
	auto remove = [](const NativeContext &c) {
		if (GameObject *o = c.self()) c.world.destroy(o);
	};
	reg("CObject::DeleteThis()", remove);
	// DeleteThisLater and Die (Core registers both on DeleteThisLaterCaller): queued for the next
	// housekeeping (DE_GLOBAL::AddObjectToClean).
	auto later = [](const NativeContext &c) { c.world.clean_later(c.self()); };
	reg("CObject::DeleteThisLater()", later);
	reg("CObject::Die()", later);
	reg("CImpulseRobot::SetDefaultHotkey(string,byte,bool,bool,bool)", [](const NativeContext &c) {
		c.world.impulses->set_default_hotkey(c.arg_string(0), c.arg<uint8_t>(1), c.arg<uint8_t>(2), c.arg<uint8_t>(3), c.arg<uint8_t>(4));
	});
	reg("CImpulseRobot::SetDefaultMousekey(string,int,bool,bool,bool)", [](const NativeContext &c) {
		c.world.impulses->set_default_mousekey(c.arg_string(0), c.arg<int32_t>(1), c.arg<uint8_t>(2), c.arg<uint8_t>(3), c.arg<uint8_t>(4));
	});
	reg("CImpulseRobot::AttachToImpulse(string,class CReferenceTarget)", [](const NativeContext &c) {
		c.world.impulses->attach(c.arg_string(0), c.arg_object(1));
	});
	reg("CImpulseRobot::AttachToImpulse(string)", [](const NativeContext &c) { c.world.impulses->assure(c.arg_string(0)); });
	reg("CImpulseRobot::SetFocusRequirement(string,class CObject)", [](const NativeContext &c) {
		c.world.impulses->set_focus(c.arg_string(0), c.arg_object(1));
	});
	// IsImpulseEvent (0x700207e0 / 0x70020850): a generic event whose Text is the name, INT1 0 for
	// down (the event's handle is not checked).
	auto impulse_event = [](const NativeContext &c, int up) {
		uint32_t e = c.arg<uint32_t>(0);
		if (!e || c.world.instance_class(e) != "CGenericEvent") return false;
		uint32_t t = c.mem().get<uint32_t>(e + kText);
		if (strcasecmp(t ? c.world.read_string(t).c_str() : "", c.arg_string(1).c_str())) return false;
		return c.mem().get<int32_t>(e + kInt1) == up;
	};
	reg("CImpulseRobot::bool IsImpulseEvent(class CEvent,string)", [impulse_event](const NativeContext &c) { c.ret_bool(impulse_event(c, 0)); });
	reg("CImpulseRobot::bool IsImpulseEvent(class CEvent,string,bool)", [impulse_event](const NativeContext &c) {
		c.ret_bool(impulse_event(c, c.arg<uint8_t>(2) ? 1 : 0));
	});
	reg("CObject::class CObject GameDialog()", [](const NativeContext &c) { c.ret_u32(0); });
	reg("CObject::class CActor MusicPlayerActor()", [](const NativeContext &c) { c.ret_object(c.world.music_player_actor()); });
	reg("CObject::bool IsStandardSession(class CObject)", [](const NativeContext &c) {
		GameObject *o = c.arg_object(0);
		c.ret_bool(o && o->is("Session\\Standard"));
	});
	// --- Sounds (Core DE_CScene::PlayNoiseSphere 0x70072b70, PlayNoiseAmbient 0x70072c20) -----------
	for (const char *k : {"CScene::PlayNoiseSphere(class CNoise,class CVector,float,float,float)",
	                      "CScene::PlayNoiseSphere(class CNoise,class CVector,float,float,float,float)",
	                      "CScene::PlayNoiseSphereVolumetric(class CNoise,class CVector,float,float,float)",
	                      "CScene::PlayNoiseSphereVolumetric(class CNoise,class CVector,float,float,float,float)"}) {
		bool with_frequency = std::string(k).find(",float,float,float,float)") != std::string::npos;
		reg(k, [with_frequency](const NativeContext &c) {
			World::SoundRequest r;
			r.noise = c.arg_object(0);
			r.pos = read_vec(c.mem(), c.arg<uint32_t>(1));
			r.hotspot = c.arg<float>(2);
			r.falloff = c.arg<float>(3);
			r.volume = c.arg<float>(4);
			if (with_frequency) r.frequency = c.arg<float>(5);
			if (r.noise && scene_audible(c.world, c.owner())) c.world.sound_requests.push_back(r);
			c.ret_u32(0);  // the speaker sphere is not modelled
		});
	}
	reg("CScene::PlayNoiseAmbient(class CNoise,float)", [](const NativeContext &c) {
		World::SoundRequest r;
		r.noise = c.arg_object(0);
		r.volume = c.arg<float>(1);
		r.positional = false;
		if (r.noise && scene_audible(c.world, c.owner())) c.world.sound_requests.push_back(r);
		c.ret_u32(0);
	});
	stub("CStreamNoise::PlaySoundEvenOutOfFocus()");
	// --- Incident recorders (Core DE_CScene::CIncidentRecorder 0x70072cf0..0x70072f10) ----------
	reg("CScene::class CIncidentRecorder NewIncidentRecorder()", [](const NativeContext &c) {
		GameObject *r = c.world.create_child(nullptr, "IncidentRecorder", "Scene\\IncidentRecorder");
		c.world.incident_recorders[r];
		c.ret_object(r);
	});
	auto recorder = [](const NativeContext &c) -> World::IncidentRecorder * {
		auto it = c.world.incident_recorders.find(c.self());
		return it == c.world.incident_recorders.end() ? nullptr : &it->second;
	};
	auto now_ms = [](const NativeContext &c) { return uint32_t(c.world.scene_ms()); };
	auto present = [](const World::IncidentRecorder &r, uint32_t now, uint32_t type) {
		for (auto &[t, k] : r.incidents) {
			uint32_t d = t < now ? now - t : t - now;
			if (d <= r.threshold && k == type) return true;
		}
		return false;
	};
	auto add = [](World::IncidentRecorder &r, uint32_t now, uint32_t type) {
		r.incidents.erase(std::remove_if(r.incidents.begin(), r.incidents.end(), [&](auto &i) { return now - i.first > r.max_age; }), r.incidents.end());
		r.incidents.push_back({now, type});
	};
	const std::string ir = "CScene::CIncidentRecorder::";
	reg(ir + "SetTimeThreshold(dword)", [recorder](const NativeContext &c) { if (auto *r = recorder(c)) r->threshold = c.arg<uint32_t>(0); });
	reg(ir + "AddIncidentCurrentTime(class CReferenceTarget,dword)", [recorder, now_ms, add](const NativeContext &c) {
		if (auto *r = recorder(c)) add(*r, now_ms(c), c.arg<uint32_t>(1));
	});
	reg(ir + "bool AddIncidentCurrentTimeIfAbsent(class CReferenceTarget,dword)", [recorder, now_ms, present, add](const NativeContext &c) {
		auto *r = recorder(c);
		bool added = r && !present(*r, now_ms(c), c.arg<uint32_t>(1));
		if (added) add(*r, now_ms(c), c.arg<uint32_t>(1));
		c.ret_bool(added);
	});
	reg(ir + "bool IsIncidentCurrentTime(dword)", [recorder, now_ms, present](const NativeContext &c) {
		auto *r = recorder(c);
		c.ret_bool(r && present(*r, now_ms(c), c.arg<uint32_t>(0)));
	});
	// IsRenderedCaller (Core 0x7008e640): the actor's own Renderable flag (+0x108).
	reg("CActor::bool IsRendered()", [](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_bool(o && o->visible);
	});
	reg("CActor::class CScene TemporalMasterScene()", [](const NativeContext &c) { c.ret_object(c.world.scene_object(c.self())); });
	// The scene stands for its temporal master, which shares its pause (Genesis passes it on).
	reg("CScene::class CScene TemporalMaster()", [](const NativeContext &c) { c.ret_object(c.self()); });
	reg("CScene::AssureTemporalMaster()", [](const NativeContext &) {});
	reg("CScene::RegisterRenderTickHook(class CClassExecutorInstance,Method(float))", [](const NativeContext &c) {
		c.world.render_tick_hooks.push_back({c.world.brain_of(c.ex), c.arg<uint32_t>(1)});
	});
	// Delta-tick hooks run every scene tick with the time it covers (ms).
	reg("CScene::RegisterDeltaTickHook(class CClassExecutorInstance,Method(float))", [](const NativeContext &c) {
		c.world.delta_tick_hooks.push_back({c.world.brain_of(c.ex), c.arg<uint32_t>(1)});
	});
	reg("CScene::UnregisterDeltaTickHook(class CClassExecutorInstance,Method(float))", [](const NativeContext &c) {
		Brain *b = c.world.brain_of(c.ex);
		uint32_t m = c.arg<uint32_t>(1);
		auto &v = c.world.delta_tick_hooks;
		v.erase(std::remove_if(v.begin(), v.end(), [b, m](auto &h) { return h.first == b && h.second == m; }), v.end());
	});
	reg("CScene::UnregisterRenderTickHook(class CClassExecutorInstance,Method(float))", [](const NativeContext &c) {
		Brain *b = c.world.brain_of(c.ex);
		uint32_t m = c.arg<uint32_t>(1);
		auto &v = c.world.render_tick_hooks;
		v.erase(std::remove_if(v.begin(), v.end(), [b, m](auto &h) { return h.first == b && h.second == m; }), v.end());
	});
	reg("CScene::UnregisterRenderTickHook(class CClassExecutorInstance)", [](const NativeContext &c) {
		Brain *b = c.world.brain_of(c.ex);
		auto &v = c.world.render_tick_hooks;
		v.erase(std::remove_if(v.begin(), v.end(), [b](auto &h) { return h.first == b; }), v.end());
	});

	// --- Creature control (DE_CCreature::SetAsUserControlled / SetAsAIControlled) ----------------
	reg("CCreature::SetAsUserControlled(int)", [](const NativeContext &c) { if (GameObject *o = c.self()) c.world.set_user_controlled(*o, c.arg<int32_t>(0)); });
	reg("CCreature::SetAsAIControlled()", [](const NativeContext &c) { if (GameObject *o = c.self()) c.world.set_user_controlled(*o, -1); });
	stub("CCreature::SetCreationTime(int)");

	// --- Rules display (the "how to play / loading" screen before a match): shown and dismissed
	// at once until there is a front end.
	reg("CObject::class CRulesDisplay NewRulesDisplay()", [](const NativeContext &c) {
		c.ret_object(c.world.create_child(nullptr, "RulesDisplay", "UIManager\\RulesDisplay"));
	});
	// DE_CRulesDisplay::Begin (OMF 0x350630f0): it shows itself; the front end draws it.
	reg("CRulesDisplay::Begin(class CScene)", [](const NativeContext &c) {
		GameObject *o = c.self();
		if (!o) return;
		auto &r = c.world.rules_displays[o];
		r.session = c.world.session ? c.world.session->obj : nullptr;
		if (c.world.show_rules) {
			r.shown = true;
			c.world.show_rules(o);
		}
	});
	reg("CRulesDisplay::SetCurrentTask(string)", [](const NativeContext &c) {
		if (GameObject *o = c.self()) c.world.rules_displays[o].task = c.arg_string(0);
	});
	// IsAuthorizedToContinueCaller (OMF 0x35062ec0): asking while unauthorized brings up the prompt.
	reg("CRulesDisplay::bool IsAuthorizedToContinue()", [](const NativeContext &c) {
		GameObject *o = c.self();
		if (!o) {
			c.ret_bool(true);
			return;
		}
		auto &r = c.world.rules_displays[o];
		if (!r.authorized) r.prompted = true;
		c.ret_bool(r.authorized);
	});

	// --- Colours and text primitives ------------------------------------------------------------
	reg("CObject::CColorRGBA::Set(float,float,float,float)", [](const NativeContext &c) {
		if (uint32_t a = c.self_handle()) {
			for (int i = 0; i < 4; ++i) c.mem().set<float>(a + uint32_t(i) * 4, c.arg<float>(size_t(i)));
		}
	});
	// ColorRGBAEqualsOpperatorCaller: copy; from null, rgb go to 0 (alpha kept).
	reg("CObject::CColorRGBA::(opp =)(class CColorRGBA)", [](const NativeContext &c) {
		uint32_t d = c.self_handle(), s = c.arg<uint32_t>(0);
		if (!d) return;
		for (uint32_t i = 0; i < 4; ++i) {
			if (s) c.mem().set<float>(d + i * 4, c.mem().get<float>(s + i * 4));
			else if (i < 3) c.mem().set<float>(d + i * 4, 0);
		}
	});
	reg("CObject::CColorRGBA::Set(float,float,float)", [](const NativeContext &c) {
		if (uint32_t a = c.self_handle()) {
			for (int i = 0; i < 3; ++i) c.mem().set<float>(a + uint32_t(i) * 4, c.arg<float>(size_t(i)));
		}
	});
	auto new_primitive = [&](const char *sig, World::Primitive::Kind kind, const char *path) {
		reg(sig, [kind, path](const NativeContext &c) {
			GameObject *mesh = c.self();
			if (!mesh) {
				c.ret_u32(0);
				return;
			}
			GameObject *t = c.world.create_child(mesh, "Primitive", path);
			auto &p = c.world.texts[t];
			p.mesh = mesh;
			p.kind = kind;
			c.ret_object(t);
		});
	};
	new_primitive("CPrimitiveGeneratorMesh::class CTextPrimitive NewText()", World::Primitive::Text, "Primitive\\Text");
	new_primitive("CPrimitiveGeneratorMesh::class CQuadPrimitive NewQuad()", World::Primitive::Quad, "Primitive\\Quad");
	new_primitive("CPrimitiveGeneratorMesh::class CStatusBarPrimitive NewStatusBar()", World::Primitive::StatusBar, "Primitive\\StatusBar");
	reg("CPrimitiveGeneratorMesh::SetAsScreenSpace()", [](const NativeContext &c) { if (c.self()) c.world.screen_space_meshes.insert(c.self()); });
	auto text = [](const NativeContext &c) -> World::Primitive * {
		auto it = c.world.texts.find(c.self());
		return it == c.world.texts.end() ? nullptr : &it->second;
	};
	const std::string tp = "CPrimitiveGeneratorMesh::CTextPrimitive::";
	reg(tp + "SetText(string)", [text](const NativeContext &c) { if (auto *t = text(c)) t->text = c.arg_string(0); });
	reg(tp + "SetPosition(float,float,float)", [text](const NativeContext &c) {
		if (auto *t = text(c)) t->x = c.arg<float>(0), t->y = c.arg<float>(1), t->z = c.arg<float>(2);
	});
	reg(tp + "SetHeight(float)", [text](const NativeContext &c) { if (auto *t = text(c)) t->height = c.arg<float>(0); });
	reg(tp + "SetAsOverlay()", [text](const NativeContext &c) { if (auto *t = text(c)) t->overlay = true; });
	reg(tp + "SetShadowOn()", [text](const NativeContext &c) { if (auto *t = text(c)) t->shadow = true; });
	reg(tp + "SetInGameFont()", [text](const NativeContext &c) { if (auto *t = text(c)) t->in_game_font = true; });
	reg(tp + "CenterHorizontal()", [text](const NativeContext &c) { if (auto *t = text(c)) t->centered = true; });
	reg(tp + "SetColor(class CColorRGBA)", [text](const NativeContext &c) {
		auto *t = text(c);
		uint32_t a = c.arg<uint32_t>(0);
		if (t && a) {
			for (int i = 0; i < 4; ++i) t->rgba[i] = c.mem().get<float>(a + uint32_t(i) * 4);
		}
	});
	reg(tp + "SnapToRight()", [text](const NativeContext &c) { if (auto *t = text(c)) t->snap_right = true; });
	// CPrimitive (all kinds).
	const std::string pr = "CPrimitiveGeneratorMesh::CPrimitive::";
	reg(pr + "SetSortValue(int)", [text](const NativeContext &c) { if (auto *t = text(c)) t->sort = c.arg<int32_t>(0); });
	reg(pr + "SetTexture(class CImage)", [text](const NativeContext &c) { if (auto *t = text(c)) t->texture = c.arg_object(0); });
	reg(pr + "SetAsOverlay()", [text](const NativeContext &c) { if (auto *t = text(c)) t->overlay = true; });
	reg(pr + "SetBlendUnsortedTranslucentAlpha()", [text](const NativeContext &c) { if (auto *t = text(c)) t->additive = false; });
	reg(pr + "SetBlendAddAlpha()", [text](const NativeContext &c) { if (auto *t = text(c)) t->additive = true, t->modulate = false; });
	reg(pr + "SetBlendModulate()", [text](const NativeContext &c) { if (auto *t = text(c)) t->modulate = true, t->additive = false; });
	auto set_color = [text](const NativeContext &c) {
		auto *t = text(c);
		uint32_t a = c.arg<uint32_t>(0);
		if (t && a) {
			for (int i = 0; i < 4; ++i) t->rgba[i] = c.mem().get<float>(a + uint32_t(i) * 4);
		}
	};
	for (const char *cls : {"CPrimitiveGeneratorMesh::CQuadPrimitive::", "CPrimitiveGeneratorMesh::CStatusBarPrimitive::"}) {
		reg(std::string(cls) + "SetColor(class CColorRGBA)", set_color);
		reg(std::string(cls) + "SetTexture(class CImage)", [text](const NativeContext &c) { if (auto *t = text(c)) t->texture = c.arg_object(0); });
		reg(std::string(cls) + "SetAsOverlay()", [text](const NativeContext &c) { if (auto *t = text(c)) t->overlay = true; });
		reg(std::string(cls) + "SetBlendAddAlpha()", [text](const NativeContext &c) { if (auto *t = text(c)) t->additive = true, t->modulate = false; });
		reg(std::string(cls) + "SetBlendModulate()", [text](const NativeContext &c) { if (auto *t = text(c)) t->modulate = true, t->additive = false; });
		reg(std::string(cls) + "SetBlendUnsortedTranslucentAlpha()", [text](const NativeContext &c) { if (auto *t = text(c)) t->additive = false; });
		reg(std::string(cls) + "SetSortValue(int)", [text](const NativeContext &c) { if (auto *t = text(c)) t->sort = c.arg<int32_t>(0); });
	}
	const std::string qp = "CPrimitiveGeneratorMesh::CQuadPrimitive::";
	reg(qp + "UVAsBox(float,float,float,float)", [text](const NativeContext &c) {
		if (auto *t = text(c)) {
			for (int i = 0; i < 4; ++i) t->uv[i] = c.arg<float>(size_t(i));
		}
	});
	reg(qp + "PositionAsBox(float,float,float,float,float)", [text](const NativeContext &c) {
		if (auto *t = text(c)) {
			for (int i = 0; i < 4; ++i) t->box[i] = c.arg<float>(size_t(i));
			t->z = c.arg<float>(4);
		}
	});
	const std::string sb = "CPrimitiveGeneratorMesh::CStatusBarPrimitive::";
	reg(sb + "SetPosition(float,float,float,float,float)", [text](const NativeContext &c) {
		if (auto *t = text(c)) {
			for (int i = 0; i < 4; ++i) t->box[i] = c.arg<float>(size_t(i));
			t->z = c.arg<float>(4);
		}
	});
	reg(sb + "SetValue(float)", [text](const NativeContext &c) { if (auto *t = text(c)) t->value = std::clamp(c.arg<float>(0), 0.0f, 1.0f); });
	reg(sb + "float Value()", [text](const NativeContext &c) { auto *t = text(c); c.ret_f32(t ? t->value : 0); });
	reg(sb + "SetAsBottomToTop()", [text](const NativeContext &c) { if (auto *t = text(c)) t->bottom_to_top = true; });
	reg(sb + "SetAsLeftToRight()", [text](const NativeContext &c) { if (auto *t = text(c)) t->bottom_to_top = false; });

	reg("CObject::class CImpulseRobot GlobalImpulseRobot()", [](const NativeContext &c) { c.ret_object(c.world.impulses->object()); });
}

}  // namespace game
