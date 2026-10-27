// Navigation links (Creature DE_CNavigationLink::CPath: SetupCreature 0x1500f290, GetPathAffector
// 0x1500eeb0, CalculatePath 0x1500ef90, GetMoveToPosition 0x1500f8b0; DE_CNavigationLink::StepTo
// 0x1500f5a0, PreStepCalculate 0x1500f790; DE_CMoveToCommand::Update 0x15021640).
//
// An arena's navigation areas are affectors (Desert's spheres under NavParent). Each carries
// NavigationLink properties naming a destination area (references chunk 2: BOOL, reference).
// A move-to command finds the area the creature is in and the one its destination is in, then
// spreads a wavefront over the open links from the first: a link's cost is the distance walked
// to its destination area plus that area's distance to the destination; each pass expands every
// link within 50 of the cheapest (StepTo closes it and opens the links leaving its destination)
// and keeps the rest for later, until a link reaches the destination's area. The path is the
// chain of areas back to the start. The creature then steers for the next area on the path,
// dropping areas as it enters them, and for the destination itself once none are left. The path
// is worked out again at most every 500 ms.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/navigation.h"
#include "game/session.h"

namespace game {
namespace {

using Vec = std::array<float, 3>;
float dist(const Vec &a, const Vec &b) {
	float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
	return std::sqrt(x * x + y * y + z * z);
}

float radius(const GameObject &a) { return a.falloff > 0 ? a.falloff : a.hotspot; }

// DE_CAffector vtable +0x164 for a sphere: is the point inside it?
bool inside(const GameObject &area, const Vec &p) {
	return area.shape == GameObject::Shape::Sphere && dist(area.world().t, p) <= radius(area);
}

}  // namespace

void Navigation::build() {
	built_ = true;
	static const bool dump = getenv("OPENBG_DEBUG_NAV") && !strcmp(getenv("OPENBG_DEBUG_NAV"), "dump");
	links_.clear();
	areas_.clear();
	for (GameObject *o : w_.scene_objects()) {
		if (!o->is("UniqueActorProperty\\NavigationLink") || !o->file || !o->parent) continue;
		Link l;
		l.object = o;
		l.from = o->parent;
		auto lists = reference_lists(w_.data(), {o->file, o->index});
		if (lists.size() > 2 && !lists[2].empty() && !lists[2][0].empty() && lists[2][0][0]) {
			l.to = nearest_copy(o, lists[2][0][0]);
		}
		if (!l.to) continue;
		if (dump) {
			auto a = l.from->world().t, b = l.to->world().t;
			fprintf(stderr, "[nav link] %s (%.0f %.0f %.0f r %.0f) -> %s (%.0f %.0f %.0f r %.0f)\n", l.from->name.c_str(), a[0], a[1], a[2], radius(*l.from), l.to->name.c_str(), b[0], b[1], b[2], radius(*l.to));
		}
		links_.push_back(l);
		for (GameObject *a : {l.from, l.to}) {
			if (std::find(areas_.begin(), areas_.end(), a) == areas_.end()) areas_.push_back(a);
		}
	}
}

// GetPathAffector: a sphere test at the point, growing until it meets a navigation area (or
// passes 500).
GameObject *Navigation::area_at(const Vec &p) {
	for (float r = 1; r <= 500; r *= 2) {
		GameObject *best = nullptr;
		float best_d = 1e30f;
		for (GameObject *a : areas_) {
			if (!a->active) continue;
			float d = dist(a->world().t, p) - radius(*a);
			if (d <= r && d < best_d) best = a, best_d = d;
		}
		if (best) return best;
	}
	return nullptr;
}

void Navigation::calculate(Path &path, GameObject &creature, const Vec &dest) {
	path.areas.clear();
	path.dest = dest;
	GameObject *from = area_at(creature.world().t), *to = area_at(dest);
	if (!from || !to || from == to) return;
	for (auto &l : links_) l.open = true, l.parent = nullptr;
	// PreStepCalculate: walked so far + this link's walk, plus its destination area to the goal.
	auto prestep = [&](Link &l, float walked) {
		l.walked = walked + dist(l.from->world().t, l.to->world().t);
		l.cost = l.walked + dist(l.to->world().t, dest);
	};
	std::vector<Link *> open, next;
	for (auto &l : links_) {
		if (l.from == from && l.open) {
			open.push_back(&l);
			prestep(l, 0);
		}
	}
	float threshold = 0;
	while (!open.empty()) {
		next.clear();
		float cheapest = 1e9f;
		for (Link *l : open) {
			if (!l->open) continue;  // closed by an earlier step of this pass
			if (l->to == to) {
				// The chain of areas back to the start: [destination area, ..., the first step].
				path.areas.push_back(to);
				for (Link *k = l; k && k->from != from; k = k->parent) path.areas.push_back(k->from);
				path.calculated = scene_ms();  // only a found path holds off the next search
				return;
			}
			if (l->cost < threshold || threshold == 0) {
				// StepTo: close every link into this destination, open those leaving it.
				for (auto &k : links_) {
					if (k.to == l->to) k.open = false;
				}
				for (auto &k : links_) {
					if (k.from == l->to && k.open) {
						prestep(k, l->walked);
						k.parent = l;
						next.push_back(&k);
					}
				}
				cheapest = std::min(cheapest, l->cost);
			} else {
				next.push_back(l);
			}
		}
		threshold = cheapest + 50;
		std::swap(open, next);
	}
}

// GetMoveToPosition: drop the next area (or the one after it) once inside it; head for the next
// area left, else for the destination.
Vec Navigation::move_to(Path &path, GameObject &creature, float *distance) {
	Vec me = creature.world().t;
	auto &a = path.areas;
	if (!a.empty()) {
		if (inside(*a.back(), me)) {
			a.pop_back();
		} else if (a.size() > 1 && inside(*a[a.size() - 2], me)) {
			a.pop_back();
			a.pop_back();
		} else if (a.size() > 2 && inside(*a.front(), me)) {
			a.clear();
		}
	}
	if (!a.empty()) {
		Vec next = a.back()->world().t;
		float d = dist(next, me);
		for (size_t i = a.size() - 1; i > 0; --i) d += dist(a[i]->world().t, a[i - 1]->world().t);
		if (distance) *distance = d;
		return next;
	}
	if (distance) *distance = dist(path.dest, me);
	return path.dest;
}

bool Navigation::steer(GameObject &creature, const Vec &dest, Vec *waypoint, float *distance) {
	if (!built_) build();
	if (areas_.empty()) return false;
	Path &path = paths_[&creature];
	// DE_CMoveToCommand::Update: more than 500 ms since the last path was found (a new
	// destination waits for that too).
	if (std::fabs(scene_ms() - path.calculated) > 500) calculate(path, creature, dest);
	*waypoint = move_to(path, creature, distance);
	static const bool debug = getenv("OPENBG_DEBUG_NAV") != nullptr;
	if (debug && w_.frame() % 30 == 0) {
		GameObject *from = area_at(creature.world().t), *to = area_at(dest);
		fprintf(stderr, "[nav %u] %s at %s -> %s (%.0f %.0f %.0f): %zu area(s)", w_.frame(), creature.name.c_str(), from ? from->name.c_str() : "-",
		        to ? to->name.c_str() : "-", dest[0], dest[1], dest[2], path.areas.size());
		for (auto it = path.areas.rbegin(); it != path.areas.rend(); ++it) fprintf(stderr, " %s", (*it)->name.c_str());
		fprintf(stderr, "; heading for %.0f %.0f %.0f\n", (*waypoint)[0], (*waypoint)[1], (*waypoint)[2]);
	}
	return true;
}

float Navigation::scene_ms() const { return w_.scene_ms(); }

}  // namespace game
