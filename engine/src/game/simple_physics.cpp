// Simple physics for everything that isn't a fighter (Core DE_CSimplePhysics: ObjProcessContents
// 0x701db600, ObjPrepare 0x701db9e0, Tick 0x701dbfc0, DeltaTick 0x701dc060, PerformCollisionTests
// 0x701dc1f0, PushFromSurfacesTouched, Bounce 0x701dbbb0; DE_CPhysics base contents and
// UpdateEnabled).
//
// A Physics\Simple belongs to an actor (a child of it, or one of its properties). Its base chunk
// says whether it is enabled and its rate: 0 steps it every scene update by the time passed
// (DeltaTick: momentum in units/ms, gravity 0.037/256 per ms^2), otherwise it ticks every `rate`
// ms (Tick: momentum per tick, gravity 0.037 per tick). Each step moves the actor by its momentum
// (acceleration added first), applies gravity, then tests its affectors against its groups: it
// can push out of the surfaces it touched, bounce off them (calling its bounce hooks with the
// impact strength) and apply its force to them.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "game/forces.h"
#include "game/particles.h"
#include "game/session.h"
#include "game/simple_physics.h"

namespace game {
namespace {

using Vec = std::array<float, 3>;
Vec add(const Vec &a, const Vec &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec mul(const Vec &a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float dot(const Vec &a, const Vec &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float len(const Vec &a) { return std::sqrt(dot(a, a)); }

GameObject *find_in_branch(GameObject *o, const de::ObjectRef &r) {
	if (o->file == r.file && o->index == r.index) return o;
	for (GameObject *c : o->children) {
		if (GameObject *hit = find_in_branch(c, r)) return hit;
	}
	return nullptr;
}

}  // namespace

SimplePhysics &SimplePhysicsSystem::state(GameObject &p) {
	SimplePhysics &s = states_[&p];
	if (s.read || !p.file) return s;
	s.read = true;
	try {
		auto od = p.file->object_data(p.file->objects()[p.index]);
		de::Contents c(od.contents);
		// [physics v2: name, BOOL enabled, int rate][simple physics v3].
		de::Chunk base = de::begin_chunk(c);
		c.block();
		if (base.version == 2) {
			s.enabled = c.boolean();
			s.rate = c.i32();
		}
		c.seek(base.end);
		de::Chunk sp = de::begin_chunk(c);
		if (sp.version >= 2) {
			s.gravity = c.f32();
			for (auto &v : s.momentum) v = c.f32();
			for (auto &v : s.accel) v = c.f32();
		}
		if (sp.version >= 3) {
			s.bounce = c.boolean();
			s.lost_on_bounce = c.f32();
			s.multiplier_on_bounce = c.f32();
			s.variation_on_bounce = c.f32();
			s.groups = c.u32();
			c.squeezed(), c.squeezed();
			s.push_from_surfaces = c.boolean();
		}
		// References (DE_CSimplePhysics::ObjProcessReferences, Core 0x701db8b0): [object][physics]
		// [simple physics v2: the test affectors (+0x50, a BOOL-terminated list), then BOOL + the
		// force (+0x60)] -- the single reference reads as the chunk's second list.
		auto lists = reference_lists(w_.data(), {p.file, p.index});
		GameObject *actor = actor_of(p);
		if (lists.size() > 2 && !lists[2].empty() && actor) {
			for (auto &r : lists[2][0]) {
				if (!r) continue;
				if (GameObject *a = nearest_copy(actor, r)) {
					if (a->is("Actor\\Affector")) s.affectors.push_back(a);
				}
			}
			if (lists[2].size() > 1 && !lists[2][1].empty() && lists[2][1][0]) {
				if (GameObject *f = nearest_copy(actor, lists[2][1][0]); f && f->is("UniqueActorProperty\\Force")) s.force = f;
			}
		}
	} catch (const std::exception &) {
	}
	// DE_CPhysics::ObjPrepare: an affector actor tests itself; moving when there is momentum or
	// acceleration (both given in the actor's frame).
	GameObject *actor = actor_of(p);
	if (s.affectors.empty() && actor && actor->is("Actor\\Affector")) s.affectors.push_back(actor);
	if (s.force && actor) {
		GameObject *owner = actor->owner_actor ? actor->owner_actor : actor;
		for (GameObject *f = actor; f; f = f->parent) {
			if (f->fighter) owner = f;
		}
		w_.forces->set_actor(*s.force, owner);
	}
	if (actor) {
		de::Transform t = actor->world();
		s.momentum = t.rotate(s.momentum);
		s.accel = t.rotate(s.accel);
	}
	s.last_ms = uint32_t(scene_time_ms(w_));
	return s;
}

GameObject *SimplePhysicsSystem::actor_of(GameObject &p) {
	auto it = actors_.find(&p);
	if (it != actors_.end()) return it->second;
	for (GameObject *a = p.parent; a; a = a->parent) {
		if (a->is_actor) return a;
	}
	return nullptr;
}

std::vector<GameObject *> SimplePhysicsSystem::physics_of(GameObject &actor) {
	auto it = lists_.find(&actor);
	if (it != lists_.end()) return it->second;
	std::vector<GameObject *> out;
	for (GameObject *c : actor.children) {
		if (c->is("Physics")) out.push_back(c);
	}
	if (actor.file) {
		for (auto &r : w_.actor_properties({actor.file, actor.index})) {
			if (!r || r.info().class_path.rfind("Physics", 0) != 0) continue;
			GameObject *p = find_in_branch(&actor, r);
			if (!p) {
				if (GameObject *g = w_.global_object(r)) p = w_.forces->unique_property(actor, *g);
			}
			if (p && std::find(out.begin(), out.end(), p) == out.end()) {
				out.push_back(p);
				actors_[p] = &actor;
			}
		}
	}
	for (GameObject *p : out) actors_.emplace(p, &actor);
	return lists_[&actor] = out;
}

void SimplePhysicsSystem::step(GameObject &p, SimplePhysics &s, GameObject &actor, float scale, bool delta) {
	if (s.moving()) {
		s.momentum = add(s.momentum, mul(s.accel, scale));
		Vec move = mul(s.momentum, scale);
		if (actor.parent) move = actor.parent->world().inverse_rotate(move);
		actor.local.t = add(actor.local.t, move);
	}
	if (s.gravity != 0) s.momentum[1] -= s.gravity * (delta ? 0.037f / 256 : 0.037f) * scale;
	// PerformCollisionTests.
	std::vector<Contact> &contacts = s.contacts;
	contacts.clear();
	for (GameObject *a : s.affectors) {
		if (!s.groups || !in_scene(*a)) continue;
		auto c = touches(w_, *a, s.groups);
		contacts.insert(contacts.end(), c.begin(), c.end());
	}
	if (contacts.empty()) return;
	if (s.push_from_surfaces) {
		Vec push{0, 0, 0};
		for (auto &c : contacts) push = add(push, mul(c.normal, c.depth));
		push = mul(push, 1.0f / float(contacts.size()));
		if (actor.parent) push = actor.parent->world().inverse_rotate(push);
		actor.local.t = add(actor.local.t, push);
	}
	if (s.bounce) bounce(p, s, contacts);
	if (s.force) {
		for (auto &c : contacts) w_.forces->deliver(*s.force, c);
	}
}

// Bounce: off the first surface the motion runs into, reflect the direction; the speed loses
// lost-on-bounce x the cosine, is scaled by 1 - (1 - multiplier) x cosine and jittered by the
// variation; the bounce hooks get the speed into the surface.
void SimplePhysicsSystem::bounce(GameObject &, SimplePhysics &s, const std::vector<Contact> &contacts) {
	auto call = [&](float strength) {
		auto hooks = s.bounce_hooks;
		for (auto &[b, m] : hooks) {
			if (!b || b->inst.shut_down) continue;
			std::vector<uint8_t> args(4);
			memcpy(args.data(), &strength, 4);
			b->call_handle(w_, m, args);
		}
	};
	float speed = len(s.momentum);
	if (speed == 0) {
		call(0);
		return;
	}
	Vec dir = mul(s.momentum, 1 / speed);
	for (auto &c : contacts) {
		Vec n = mul(c.face_normal, -1);
		float d = dot(n, dir);
		if (d < 0) continue;
		dir = add(dir, mul(n, -2 * d));
		float left = speed - d * s.lost_on_bounce;
		if (left > 0) {
			s.momentum = mul(dir, left);
			if (s.multiplier_on_bounce != 1) s.momentum = mul(s.momentum, 1 - (1 - s.multiplier_on_bounce) * d);
			if (s.variation_on_bounce != 0) {
				for (auto &v : s.momentum) v += float(crt_rand()) * s.variation_on_bounce / 32768.0f - s.variation_on_bounce * 0.5f;
			}
		} else {
			s.momentum = {0, 0, 0};
		}
		call(d * speed);
		return;
	}
}

void SimplePhysicsSystem::tick() {
	uint32_t now = uint32_t(scene_time_ms(w_));
	for (GameObject *actor : w_.scene_objects()) {
		if (!actor->is_actor || actor->fighter || !in_scene(*actor)) continue;
		for (GameObject *p : physics_of(*actor)) {
			if (p->class_path != "Physics\\Simple") continue;
			SimplePhysics &s = state(*p);
			if (!s.enabled || !actor->physics_enabled) {
				s.last_ms = now;
				continue;
			}
			if (s.rate <= 0) {
				float dt = float(now - s.last_ms);
				s.last_ms = now;
				if (dt > 0) step(*p, s, *actor, dt, true);
			} else {
				while (now - s.last_ms >= uint32_t(s.rate)) {
					s.last_ms += uint32_t(s.rate);
					step(*p, s, *actor, 1, false);
				}
			}
		}
	}
}

void SimplePhysicsSystem::delta_tick(GameObject &p) {
	GameObject *actor = actor_of(p);
	SimplePhysics &s = state(p);
	uint32_t now = uint32_t(scene_time_ms(w_));
	if (now <= s.last_ms) {
		s.last_ms = now;
		return;
	}
	float dt = float(now - s.last_ms);
	s.last_ms = now;
	if (s.enabled && actor) step(p, s, *actor, dt, true);
}

void register_simple_physics_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };
	auto S = [](const NativeContext &c) -> SimplePhysics * {
		GameObject *p = c.self();
		return p ? &c.world.simple_physics->state(*p) : nullptr;
	};
	reg("CActor::class CPhysics Physics(int)", [](const NativeContext &c) {
		GameObject *a = c.self();
		int32_t i = c.arg<int32_t>(0);
		if (!a || i < 0) {
			c.ret_u32(0);
			return;
		}
		auto list = c.world.simple_physics->physics_of(*a);
		c.ret_object(size_t(i) < list.size() ? list[size_t(i)] : nullptr);
	});
	reg("CActor::EnablePhysics()", [](const NativeContext &c) { if (auto *a = c.self()) a->physics_enabled = true; });
	reg("CActor::DisablePhysics()", [](const NativeContext &c) { if (auto *a = c.self()) a->physics_enabled = false; });
	reg("CPhysics::Enable()", [S](const NativeContext &c) { if (auto *s = S(c)) s->enabled = true; });
	// DeltaTickCaller (vtable +0x44): the Delta argument is not used; the step covers the scene
	// time since the last one.
	reg("CPhysics::DeltaTick(float)", [](const NativeContext &c) {
		GameObject *p = c.self();
		if (p && p->class_path == "Physics\\Simple") c.world.simple_physics->delta_tick(*p);
	});
	reg("CPhysics::Disable()", [S](const NativeContext &c) { if (auto *s = S(c)) s->enabled = false; });
	reg("CSimplePhysics::SetMomentum(class CVector)", [S](const NativeContext &c) {
		if (auto *s = S(c)) s->momentum = read_vec(c.mem(), c.arg<uint32_t>(0));
	});
	// SetMomentumLocalCaller (Core): (x, y, z) in the actor's frame, turned to world.
	reg("CSimplePhysics::SetMomentumLocal(float,float,float)", [S](const NativeContext &c) {
		auto *s = S(c);
		GameObject *p = c.self();
		GameObject *a = p ? c.world.simple_physics->actor_of(*p) : nullptr;
		if (s && a) s->momentum = a->world().rotate({c.arg<float>(0), c.arg<float>(1), c.arg<float>(2)});
	});
	reg("CSimplePhysics::SetMomentum(float,float,float)", [S](const NativeContext &c) {
		if (auto *s = S(c)) s->momentum = {c.arg<float>(0), c.arg<float>(1), c.arg<float>(2)};
	});
	reg("CSimplePhysics::SetAcceleration(float,float,float)", [S](const NativeContext &c) {  // SetAccelerationCaller
		if (auto *s = S(c)) s->accel = {c.arg<float>(0), c.arg<float>(1), c.arg<float>(2)};
	});
	reg("CSimplePhysics::SetAcceleration(class CVector)", [S](const NativeContext &c) {
		if (auto *s = S(c)) s->accel = read_vec(c.mem(), c.arg<uint32_t>(0));
	});
	reg("CSimplePhysics::class CVector Momentum()", [S](const NativeContext &c) {
		static uint32_t v = 0;
		if (!v) v = c.mem().alloc(12);
		auto *s = S(c);
		write_vec(c.mem(), v, s ? s->momentum : Vec{0, 0, 0});
		c.ret_u32(v);
	});
	// GetMomentumVectorCaller (Core 0x701d92c0): physics vtable +0x124 copies the momentum out.
	reg("CPhysics::GetMomentum(class CVector)", [S](const NativeContext &c) {
		auto *s = S(c);
		if (uint32_t v = c.arg<uint32_t>(0)) write_vec(c.mem(), v, s ? s->momentum : Vec{0, 0, 0});
	});
	reg("CSimplePhysics::float MomentumLength()", [S](const NativeContext &c) {
		auto *s = S(c);
		c.ret_f32(s ? len(s->momentum) : 0);
	});
	// IncrementMomentumFLOATCaller (Core 0x701d9f90): the speed grows by v along the current
	// direction, stopping at zero.
	reg("CSimplePhysics::IncrementMomentum(float)", [S](const NativeContext &c) {
		auto *s = S(c);
		if (!s) return;
		float l = len(s->momentum), n = l + c.arg<float>(0);
		if (n <= 0) s->momentum = {0, 0, 0};
		else if (l > 0) s->momentum = mul(s->momentum, n / l);
	});
	reg("CSimplePhysics::IncrementMomentum(class CVector)", [S](const NativeContext &c) {
		if (auto *s = S(c)) s->momentum = add(s->momentum, read_vec(c.mem(), c.arg<uint32_t>(0)));
	});
	// IncrementMomentumLocalX/Y/Z: along the actor's own axis.
	for (int axis = 0; axis < 3; ++axis) {
		std::string n(1, "XYZ"[axis]);
		reg("CSimplePhysics::IncrementMomentumLocal" + n + "(float)", [S, axis](const NativeContext &c) {
			auto *s = S(c);
			GameObject *p = c.self();
			GameObject *a = p ? c.world.simple_physics->actor_of(*p) : nullptr;
			if (!s || !a) return;
			Vec d{0, 0, 0};
			d[size_t(axis)] = c.arg<float>(0);
			s->momentum = add(s->momentum, a->world().rotate(d));
		});
	}
	reg("CSimplePhysics::CapMomentum(float)", [S](const NativeContext &c) {
		auto *s = S(c);
		if (!s) return;
		float l = len(s->momentum), cap = c.arg<float>(0);
		if (l > cap && l > 0) s->momentum = mul(s->momentum, cap / l);
	});
	reg("CSimplePhysics::SetMomentumLostOnBounce(float)", [S](const NativeContext &c) { if (auto *s = S(c)) s->lost_on_bounce = c.arg<float>(0); });
	reg("CSimplePhysics::SetMomentumMultiplierOnBounce(float)", [S](const NativeContext &c) { if (auto *s = S(c)) s->multiplier_on_bounce = c.arg<float>(0); });
	reg("CSimplePhysics::SetMomentumVariationOnBounce(float)", [S](const NativeContext &c) { if (auto *s = S(c)) s->variation_on_bounce = c.arg<float>(0); });
	// SetWeightCaller (Core 0x701d9f20): +0x74, the gravity weight (the same field the data sets).
	reg("CSimplePhysics::SetWeight(float)", [S](const NativeContext &c) { if (auto *s = S(c)) s->gravity = c.arg<float>(0); });
	reg("CSimplePhysics::RegisterBounceHook(class CClassExecutorInstance,Method(float))", [S](const NativeContext &c) {
		if (auto *s = S(c)) s->bounce_hooks.push_back({c.world.brain_of(c.ex), c.arg<uint32_t>(1)});
	});
	reg("CSimplePhysics::UnregisterBounceHook(class CClassExecutorInstance,Method(float))", [S](const NativeContext &c) {
		auto *s = S(c);
		if (!s) return;
		Brain *b = c.world.brain_of(c.ex);
		uint32_t m = c.arg<uint32_t>(1);
		auto &v = s->bounce_hooks;
		v.erase(std::remove_if(v.begin(), v.end(), [&](auto &h) { return h.first == b && h.second == m; }), v.end());
	});
}

}  // namespace game
