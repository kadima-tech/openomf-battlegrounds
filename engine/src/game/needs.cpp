// Needs: what draws a computer fighter to a pickup (Creature DE_CCreature::InternalUpdateNeedInfo
// 0x15018400, AssureNeedInfoUpdated 0x150186e0, GreatestNeed 0x15008460, CommandAquire
// 0x15017e80; OMF DE_COMFNeed::DesireLevel 0x350679a0 / ObjProcessContents 0x35067790 /
// ObjPrepare 0x35067930; DE_COMF2Fighter::FindPowerupToAquire 0x3502bb70, PowerupLikelihood
// 0x3502c030 -- COMF2Fighter vtable 0x350795f0 slots +0x1c4 / +0x1cc / +0x1d0).
//
// A need (UniqueActorProperty\Need\OMF) hangs off the actor that offers it: the jet, nuke and
// lightning pickups' "Need" spheres, the energy boost itself, the football. Its contents (v1)
// carry five weights, read into +0x50, +0x58, +0x5c, +0x60, +0x54. DesireLevel(flags, creature)
// is zero for the need's own holder (or whoever already carries it, unless it is Exclusive), and
// for a creature other than its Exclusive value; with the Area value set the need's affector
// scales it by where the creature stands and a TeamIndex other than the creature's zeroes it.
// For a fighter with a pilot:
//   flags & 8: AffinityDefendObjective x +0x54
//   flags & 2: the greatest of AffinityCarryable x +0x5c, AffinityPowerup x +0x58, AffinityPowerup x +0x50
//   flags & 4: + 1.5 x +0x60 when the need's holder is a fighter and an enemy of the creature.
// The creature knows each need that is a property of an affector in collision group 0x800 that a
// 100000-unit test sphere at its focal point touches, each scene update (a need held by a plain
// creature, like the energy boost's, is never known); the greatest (desire with all flags, the
// previous greatest weighed x 1.3) is GreatestNeed.
//
// FindPowerupToAquire(threshold) asks each known need with flags 2, for the actor holding it (an
// affector's owner): nothing for what the fighter already carries, the carryable-hand stat for a
// carryable whose hand is free (zero when it is full), then x 1 / ((d + 6.5) / 50)^2 when that
// exceeds 1 (closer than 43.5). The best wins if it beats twice the threshold. PowerupLikelihood
// is the best desire with a threshold of 0.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

#include "game/forces.h"
#include "game/runtime.h"

namespace game {
namespace {

constexpr uint32_t kNeedGroup = 0x800;

float dist(const std::array<float, 3> &a, const std::array<float, 3> &b) {
	float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
	return std::sqrt(x * x + y * y + z * z);
}

struct NeedWeights {
	float w50 = 0, w54 = 0, w58 = 0, w5c = 0, w60 = 0;
};

// The v1 chunk after the UniqueActorProperty and DE_CNeed chunks.
NeedWeights weights_of(const GameObject &n) {
	NeedWeights w;
	if (!n.file) return w;
	try {
		de::Contents c(n.file->object_data(n.file->objects()[n.index]).contents);
		std::vector<std::pair<size_t, de::Chunk>> chunks;
		while (c.remaining() > 1) {
			de::Chunk ch = de::begin_chunk(c);
			if (ch.end > c.size()) break;
			chunks.push_back({c.pos(), ch});
			c.seek(ch.end);
		}
		if (chunks.size() < 3 || chunks[2].second.version != 1) return w;
		c.seek(chunks[2].first);
		w.w50 = c.f32(), w.w58 = c.f32(), w.w5c = c.f32(), w.w60 = c.f32(), w.w54 = c.f32();
	} catch (const std::exception &) {
	}
	return w;
}

GameObject *top_of(GameObject *o) {
	while (o && o->parent) o = o->parent;
	return o;
}

bool carryable(World &w, GameObject *o, const char *behavior) { return o && w.has_interface_named(o, behavior); }

}  // namespace

struct NeedSystem {
	World &w;
	struct Need {
		GameObject *object = nullptr;  // the need property
		GameObject *holder = nullptr;  // the actor it belongs to (UniqueActorProperty::Owner)
		NeedWeights weights;
	};
	std::map<GameObject *, NeedWeights> weights;
	struct Known {
		uint32_t frame = ~0u;
		std::vector<Need> needs;
		GameObject *greatest = nullptr;
		float greatest_desire = 0;
	};
	std::map<GameObject *, Known> known;

	explicit NeedSystem(World &world) : w(world) {}

	const NeedWeights &weights_for(GameObject *n) {
		auto it = weights.find(n);
		if (it == weights.end()) it = weights.emplace(n, weights_of(*n)).first;
		return it->second;
	}

	float desire(const Need &n, uint32_t flags, GameObject *creature) {
		auto *ex = w.object_value(n.object, "Exclusive");
		GameObject *exclusive = ex && !ex->objects.empty() ? w.object(ex->objects[0]) : nullptr;
		if (exclusive && exclusive != creature) return 0;
		if (top_of(n.holder) == creature && !exclusive) return 0;
		if (n.holder == creature) return 0;
		float area = 1, team = 1;
		auto *av = w.object_value(n.object, "Area");
		if (n.holder->shape == GameObject::Shape::Sphere && av && av->bits) {
			// The sphere's falloff weight at the creature (1 inside the hotspot).
			float d = dist(n.holder->world().t, creature->world().t);
			float hot = n.holder->hotspot, fall = std::max(n.holder->falloff, hot);
			area = d <= hot ? 1 : d >= fall ? 0 : (fall - d) / std::max(fall - hot, 1e-3f);
			// TeamIndex (the need's, else its holder's, else -1) against the fighter's: no teams
			// in play, every fighter is -1.
			auto *t = w.object_value(n.object, "TeamIndex");
			if (!t) t = w.object_value(n.holder, "TeamIndex");
			int32_t need_team = -1;
			if (t) memcpy(&need_team, &t->bits, 4);
			if (creature->fighter && need_team != -1) team = 0;
		}
		const Pilot *p = creature->fighter ? pilot_of(creature) : nullptr;
		if (!p) return 0;
		float now = w.scene_ms(), out = 0;
		const NeedWeights &k = n.weights;
		if (flags & 8) {
			float v = p->stat(0x688, now) * area * team * k.w54;
			if (v > 0) out = v;
		}
		if (flags & 2) {
			out = std::max(out, p->stat(0x5a8, now) * area * k.w5c);
			out = std::max(out, p->stat(0x5c8, now) * area * k.w58);
			out = std::max(out, p->stat(0x5c8, now) * area * k.w50);
		}
		// The holder's fighter (a fighter's own NeedAffector belongs to it) and an enemy: every other
		// fighter, there being no teams.
		GameObject *holder = n.holder->shape != GameObject::Shape::None ? w.affector_owner(n.holder) : n.holder;
		if ((flags & 4) && holder && holder->fighter && holder != creature) out += k.w60 * 1.5f;
		return out;
	}

	// InternalUpdateNeedInfo, once per scene time (k.frame: that time + 1).
	Known &update(GameObject *creature) {
		Known &k = known[creature];
		if (k.frame == uint32_t(w.scene_ms()) + 1) return k;
		k.frame = uint32_t(w.scene_ms()) + 1;
		GameObject *previous = k.greatest;
		k.needs.clear();
		k.greatest = nullptr;
		k.greatest_desire = 0;
		auto me = creature->world().t;
		static const bool debug = getenv("OPENBG_DEBUG_NEEDS") != nullptr;
		if (debug && w.frame() % 600 == 0) {
			for (GameObject *o : w.scene_objects()) {
				if (!o->is("UniqueActorProperty\\Need")) continue;
				GameObject *h = o->parent;
				const NeedWeights &k = weights_for(o);
				float now = w.scene_ms();
				fprintf(stderr, "[needs %u]   %s on %s (%s) active %d groups %#x file %d weights %.1f %.1f %.1f %.1f %.1f stats %.2f %.2f desire %.3f\n", w.frame(), o->name.c_str(),
				        h ? h->name.c_str() : "-", h ? h->class_path.c_str() : "-", h ? h->active : 0, h ? h->groups : 0, o->file != nullptr, k.w50, k.w54, k.w58, k.w5c, k.w60,
				        pilot_of(creature) ? pilot_of(creature)->stat(0x5a8, now) : -1, pilot_of(creature) ? pilot_of(creature)->stat(0x5c8, now) : -1, h ? desire({o, h, k}, ~0u, creature) : -1);
			}
		}
		// InternalUpdateNeedInfo: the scene's TestSphere at the creature's focal point, radius
		// 100000, groups 0x800; each touched affector's Need property (FindPropertyByType), the
		// results walked from the last.
		(void)me;
		GameObject *probe = w.create("NeedTestSphere", "Actor\\Affector\\Sphere");
		probe->shape = GameObject::Shape::Sphere;
		probe->local = {};
		probe->local.t = focal_point(*creature);
		probe->falloff = 100000;
		auto touched = touches(w, *probe, kNeedGroup);
		for (auto it = touched.rbegin(); it != touched.rend(); ++it) {
			GameObject *holder = it->touched, *o = nullptr;
			for (auto *list : {&holder->children, &holder->components}) {
				for (GameObject *ch : *list) {
					if (!o && ch->is("UniqueActorProperty\\Need")) o = ch;
				}
			}
			if (!o) continue;
			Need n{o, holder, weights_for(o)};
			float d = desire(n, ~0u, creature);
			if (d <= 0) continue;
			k.needs.push_back(n);
			if (o == previous) d *= 1.3f;
			if (d > k.greatest_desire) k.greatest_desire = d, k.greatest = o;
		}
		return k;
	}

	GameObject *find_powerup(GameObject *fighter, float &threshold) {
		Known &k = update(fighter);
		GameObject *best = nullptr;
		float best_desire = 0;
		float now = w.scene_ms();
		for (auto it = k.needs.rbegin(); it != k.needs.rend(); ++it) {
			float d = desire(*it, 2, fighter);
			GameObject *a = it->holder;
			if (a->shape != GameObject::Shape::None) {
				if (GameObject *owner = w.affector_owner(a); owner && owner != a && !owner->fighter) a = owner;
			}
			if (top_of(a) == fighter) d = 0;
			const Pilot *pilot = pilot_of(fighter);
			float hand = pilot ? pilot->stat(0x5a8, now) : 1;
			if (carryable(w, a, "BCarryableRight")) d = fighter->carry[1] ? 0 : d * hand;
			if (carryable(w, a, "BCarryableLeft")) d = fighter->carry[0] ? 0 : d * hand;
			if (d > 0) {
				float f = (dist(fighter->world().t, a->world().t) + 6.5f) / 50;
				f = 1 / (f * f);
				if (f > 1) d *= f;
			}
			if (best_desire < d) best_desire = d, best = a;
		}
		if (best_desire <= threshold + threshold) {
			threshold = 0;
			return nullptr;
		}
		threshold = best_desire;
		return best;
	}
};

void register_need_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };
	auto sys = std::make_shared<NeedSystem>(w);
	w.need_owners = [sys](GameObject *creature) {
		std::vector<std::pair<GameObject *, float>> out;
		for (auto &n : sys->update(creature).needs) {
			GameObject *owner = n.holder->shape != GameObject::Shape::None ? sys->w.affector_owner(n.holder) : n.holder;
			if (owner) out.push_back({owner, sys->desire(n, 4, creature)});
		}
		return out;
	};
	static const bool debug = getenv("OPENBG_DEBUG_NEEDS") != nullptr;

	reg("CCreature::class CUniqueActorProperty GreatestNeed()", [sys](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_object(o ? sys->update(o).greatest : nullptr);
	});
	reg("CCreature::float GreatestNeedDesire()", [sys](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_f32(o ? sys->update(o).greatest_desire : 0);
	});
	reg("CCreature::class CActor FindPowerupToAquire(float)", [sys](const NativeContext &c) {
		GameObject *o = c.self();
		float t = c.arg<float>(0);
		GameObject *p = o && o->fighter ? sys->find_powerup(o, t) : nullptr;
		if (debug && o) fprintf(stderr, "[needs %u] %s FindPowerupToAquire -> %s (%.3f)\n", c.world.frame(), o->name.c_str(), p ? p->name.c_str() : "-", t);
		c.ret_object(p);
	});
	reg("CCreature::float PowerupLikelihood()", [sys](const NativeContext &c) {
		GameObject *o = c.self();
		float t = 0;
		if (o && o->fighter) sys->find_powerup(o, t);
		if (debug && o && c.world.frame() % 60 == 0) {
			auto &k = sys->update(o);
			fprintf(stderr, "[needs %u] %s: %zu known, greatest %s (%.3f), powerup likelihood %.3f\n", c.world.frame(), o->name.c_str(), k.needs.size(),
			        k.greatest ? (k.greatest->parent ? k.greatest->parent->name.c_str() : k.greatest->name.c_str()) : "-", k.greatest_desire, t);
		}
		c.ret_f32(t);
	});
}

}  // namespace game
