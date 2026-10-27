// Creature bookkeeping the pickup and effect scripts rely on (Creature DE_CCreature
// SceneTicksPerUpdate / CreationTime, Core DE_CPassiveActorReference::Create, DE_CActor::AddProperty,
// DE_CScene::CIncidentRecorder::AddIncidentIfAbsent, single-dimension arrays).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "game/runtime.h"

namespace game {

void register_creature_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };

	// DE_CCreature::DeltaTick (Creature 0x15016700): once per scene time, restores a replayed state
	// (network replication), ticks the creature's command manager (+0x118) unless the scene is
	// paused, and calls its surface-change hooks when the surface under it changed. Only the
	// observer camera calls it (BObserverCamera::Tick); its command executors (mouse-axis
	// navigation, digital-orientation propulsion, teleport: its autopilot) are not ported, so
	// there is nothing to run.
	reg("CCreature::DeltaTick(float)", [](const NativeContext &) {});
	reg("CCreature::int SceneTicksPerUpdate()", [](const NativeContext &c) { c.ret_u32(c.world.creature_rate(c.self())); });
	reg("CCreature::dword CreationTime()", [](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_u32(o ? o->creation_time : 0);
	});
	// SetCreationTime: aligned down to the creature's update step (the spawner re-reads it).
	for (const char *k : {"CCreature::SetCreationTime(dword)", "CCreature::SetCreationTime(int)"}) {
		reg(k, [](const NativeContext &c) {
			GameObject *o = c.self();
			if (!o) return;
			uint32_t t = c.arg<uint32_t>(0), rate = c.world.creature_rate(o);
			o->creation_time = rate ? t - t % rate : t;
		});
	}
	reg("CCreature::int CreationTime()", [](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_u32(o ? o->creation_time : 0);
	});
	// SetOwner / Owner (OMF SetOwnerCaller 0x35012570, OwnerCaller 0x35012590): the creature that
	// made this one -- a projectile's fighter, which its collision tests then skip.
	reg("CCreature::SetOwner(class CActor)", [](const NativeContext &c) { if (auto *o = c.self()) o->creature_owner = c.arg_object(0); });
	reg("CCreature::class CActor Owner()", [](const NativeContext &c) { auto *o = c.self(); c.ret_object(o ? o->creature_owner : nullptr); });
	// SetAsNewProjectileOwner (0x350213b0): a projectile handed to this fighter becomes its own.
	reg("COMF2Fighter::SetAsNewProjectileOwner(class CActor)", [](const NativeContext &c) {
		GameObject *p = c.arg_object(0);
		if (p && p->is("Actor\\Creature\\OMF2Projectile")) p->creature_owner = c.self();
	});
	// SetAsProjectileCreator (0x35024fd0): a projectile's creator (+0x314) and owner (+0x308)
	// become this fighter. Only the state packing reads the creator, so only the owner is kept.
	reg("COMF2Fighter::SetAsProjectileCreator(class CActor)", [](const NativeContext &c) {
		GameObject *p = c.arg_object(0);
		if (p && p->is("Actor\\Creature\\OMF2Projectile")) p->creature_owner = c.self();
	});
	// SetOrigionalCaller (0x3503dc00) stores the projectile it was copied from (+0x320); nothing
	// in the engine or the scripts reads it back.
	reg("CCreature::SetOrigional(class CCreature)", [](const NativeContext &) {});
	// Marks the creature's last state for replication (+0x204 state +0x28 |= 8).
	reg("CCreature::RegisterLastStateTimeCritical()", [](const NativeContext &) {});
	reg("CCreature::AppendCreatureID(class CActor)", [](const NativeContext &) {});  // replication identity
	reg("CCreature::AppendCreatureID(class CCreature)", [](const NativeContext &) {});
	// Called when the creature's replicated state changes; a local server never receives one.
	reg("CCreature::RegisterCreatureStateChangedHook(class CClassExecutorInstance,Method())", [](const NativeContext &) {});
	// CValueFLOAT::Set: the value object's own value, seen by GetFLOATValue on its parent.
	reg("CValueFLOAT::Set(float)", [](const NativeContext &c) {
		GameObject *v = c.self();
		if (!v || !v->parent) return;
		float f = c.arg<float>(0);
		memcpy(&c.world.values[v->parent][v->name].bits, &f, 4);
	});
	reg("CSphereAffector::float Falloff()", [](const NativeContext &c) { auto *o = c.self(); c.ret_f32(o ? o->falloff : 0); });
	reg("CSphereAffector::float Hotspot()", [](const NativeContext &c) { auto *o = c.self(); c.ret_f32(o ? o->hotspot : 0); });
	// Property(i): the actor's properties in order, then those added at run time.
	auto property_at = [](World &w, GameObject *o, size_t i) -> GameObject * {
		auto refs = w.properties_of(*o);
		if (i < refs.size()) {
			std::function<GameObject *(GameObject *)> find = [&](GameObject *x) -> GameObject * {
				if (x->file == refs[i].file && x->index == refs[i].index) return x;
				for (GameObject *ch : x->children) {
					if (GameObject *h = find(ch)) return h;
				}
				return nullptr;
			};
			GameObject *p = find(o);
			return p ? p : w.global_object(refs[i]);
		}
		size_t k = i - refs.size();
		return k < o->components.size() ? o->components[k] : nullptr;
	};
	reg("CActor::class CObject Property(int)", [property_at](const NativeContext &c) {
		GameObject *o = c.self();
		int32_t i = c.arg<int32_t>(0);
		c.ret_object(o && i >= 0 ? property_at(c.world, o, size_t(i)) : nullptr);
	});
	// FindPropertyByType: the first property derived from the class.
	reg("CActor::class CObject FindPropertyByType(classtype )", [property_at](const NativeContext &c) {
		GameObject *o = c.self();
		std::string path = c.world.class_path_of_handle(c.arg<uint32_t>(0));
		if (!o || path.empty()) {
			c.ret_object(nullptr);
			return;
		}
		size_t n = c.world.properties_of(*o).size() + o->components.size();
		for (size_t i = 0; i < n; ++i) {
			GameObject *p = property_at(c.world, o, i);
			if (p && p->is(path.c_str())) {
				c.ret_object(p);
				return;
			}
		}
		c.ret_object(nullptr);
	});
	// NumActorChildren / ActorChild: the children that are actors (DE_CActor +0xfc).
	auto actor_children = [](GameObject *o) {
		std::vector<GameObject *> out;
		if (o) {
			for (GameObject *ch : o->children) {
				if (ch->is("Actor")) out.push_back(ch);
			}
		}
		return out;
	};
	reg("CActor::int NumActorChildren()", [actor_children](const NativeContext &c) { c.ret_u32(uint32_t(actor_children(c.self()).size())); });
	reg("CActor::class CActor ActorChild(int)", [actor_children](const NativeContext &c) {
		auto ch = actor_children(c.self());
		int32_t i = c.arg<int32_t>(0);
		c.ret_object(i >= 0 && size_t(i) < ch.size() ? ch[size_t(i)] : nullptr);
	});
	// DE_CCreature::AddComponent (Creature 0x15017180, vtable +0x188): the creature becomes this
	// one's component (+0x218, listed at +0x128) and its child; its creation time is aligned to
	// ours (+0x1d8, rounded down to its own update interval) so that it updates in step with us.
	// Update phases are not modelled here, so only the parenting and the owner remain.
	reg("CCreature::AddComponent(class CCreature)", [](const NativeContext &c) {
		GameObject *o = c.self(), *k = c.arg_object(0);
		if (!o || !k || k == o) return;
		k->component_owner = o;
		if (k->parent != o) {
			if (k->parent) {
				auto &v = k->parent->children;
				v.erase(std::remove(v.begin(), v.end(), k), v.end());
			}
			k->parent = o;
			o->children.push_back(k);
		}
	});
	reg("CPassiveActorReference::class CActor Create()", [](const NativeContext &c) {
		GameObject *r = c.world.create_from_reference(c.self());
		static const bool debug = getenv("OPENBG_DEBUG_REFS") != nullptr;
		if (debug && !r) fprintf(stderr, "[ref %u] Create on %s failed\n", c.world.frame(), c.self() ? c.self()->name.c_str() : "null");
		c.ret_object(r);
	});
	// AddProperty: the object joins the actor's properties (here its components).
	reg("CActor::AddProperty(class CObject)", [](const NativeContext &c) {
		GameObject *o = c.self(), *p = c.arg_object(0);
		if (o && p && std::find(o->components.begin(), o->components.end(), p) == o->components.end()) o->components.push_back(p);
		// A texture manager's or mesh's image property: what it draws from now on.
		if (o && p && o->file && p->file && p->is("Image") && (o->is("TextureManager") || o->is("Mesh"))) {
			c.world.texture_overrides[{o->file, o->index}] = de::ObjectRef{p->file, p->index};
			++c.world.texture_override_version;
		}
	});
	// RemoveAllPropertyCaller (Core 0x70127230 / Session): the object's properties are cleared.
	for (const char *k : {"CTextureManager::RemoveAllProperty()", "CMesh::RemoveAllProperty()"}) {
		reg(k, [](const NativeContext &c) {
			GameObject *o = c.self();
			if (!o) return;
			o->components.clear();
			if (o->file) {
				c.world.texture_overrides[{o->file, o->index}] = de::ObjectRef{};
				++c.world.texture_override_version;
			}
		});
	}
	reg("CActor::int NumProperties()", [](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_u32(o ? uint32_t(c.world.properties_of(*o).size() + o->components.size()) : 0);
	});

	// AddIncidentIfAbsent(time, reference, type): an incident of that type within the threshold
	// of `time` counts as present.
	reg("CScene::CIncidentRecorder::bool AddIncidentIfAbsent(dword,class CReferenceTarget,dword)", [](const NativeContext &c) {
		auto it = c.world.incident_recorders.find(c.self());
		if (it == c.world.incident_recorders.end()) {
			c.ret_bool(false);
			return;
		}
		auto &r = it->second;
		uint32_t t = c.arg<uint32_t>(0), type = c.arg<uint32_t>(2);
		static const bool debug = getenv("OPENBG_DEBUG_INCIDENTS") != nullptr;
		if (debug) {
			GameObject *who = c.owner();
			fprintf(stderr, "[incident %u] %s asks %u (type %u), %zu recorded\n", c.world.frame(), who ? who->name.c_str() : "?", t, type, r.incidents.size());
		}
		for (auto &[when, k] : r.incidents) {
			uint32_t d = when < t ? t - when : when - t;
			if (d <= r.threshold && k == type) {
				c.ret_bool(false);
				return;
			}
		}
		r.incidents.push_back({t, type});
		c.ret_bool(true);
	});

	// Arrays: [u32 data pointer][u32 length]...
	for (const char *cls : {"CObject::CSingleDimensionPrimitiveArray::", "CObject::CSingleDimensionReferenceArray::"}) {
		std::string k = cls;
		reg(k + "RemoveIndex(int)", [](const NativeContext &c) {
			uint32_t a = c.self_handle();
			int32_t i = c.arg<int32_t>(0);
			if (!a) return;
			uint32_t n = c.mem().get<uint32_t>(a + 4), data = c.mem().get<uint32_t>(a);
			if (i < 0 || uint32_t(i) >= n) return;
			for (uint32_t j = uint32_t(i); j + 1 < n; ++j) c.mem().set<uint32_t>(data + j * 4, c.mem().get<uint32_t>(data + (j + 1) * 4));
			c.mem().set<uint32_t>(a + 4, n - 1);
		});
	}
	reg("CObject::CSingleDimensionReferenceArray::int NumActive()", [](const NativeContext &c) {
		uint32_t a = c.self_handle();
		if (!a) {
			c.ret_u32(0);
			return;
		}
		uint32_t n = c.mem().get<uint32_t>(a + 4), data = c.mem().get<uint32_t>(a), live = 0;
		for (uint32_t j = 0; j < n; ++j) live += c.world.object(c.mem().get<uint32_t>(data + j * 4)) ? 1 : 0;
		c.ret_u32(live);
	});
}

}  // namespace game
