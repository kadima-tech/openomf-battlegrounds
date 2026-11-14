// Navigation links (Creature DE_CNavigationLink::CPath: SetupCreature 0x1500f290, GetPathAffector
// 0x1500eeb0 (2.123: 0x1020ef70), CalculatePath 0x1500ef90, GetMoveToPosition 0x1500f8b0; DE_CNavigationLink::StepTo
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
#include "game/forces.h"
#include "game/session.h"

namespace game {
namespace {

using Vec = std::array<float, 3>;
float dist(const Vec &a, const Vec &b) {
	float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
	return std::sqrt(x * x + y * y + z * z);
}

float radius(const GameObject &a) { return a.falloff > 0 ? a.falloff : a.hotspot; }

// DE_CAffector vtable +0x164 for a sphere: is the point inside it? Not in the original: a box area
// (Volcano's GroundBox, over all its ground) holds a point within its extent across and from 80
// under to 120 over it (the ground it marks can lie under the box).
bool inside(const GameObject &area, const Vec &p) {
	if (area.shape == GameObject::Shape::Box) {
		const de::Transform t = area.world();
		const auto l = t.inverse_rotate({p[0] - t.t[0], p[1] - t.t[1], p[2] - t.t[2]});
		return l[0] >= area.box_min[0] && l[0] <= area.box_max[0] && l[2] >= area.box_min[2] && l[2] <= area.box_max[2] &&
		       l[1] >= area.box_min[1] - 80 && l[1] <= area.box_max[1] + 120;
	}
	return area.shape == GameObject::Shape::Sphere && dist(area.world().t, p) <= radius(area);
}

// Where to head for an area: its centre; for a box, the nearest point of it (its centre can lie
// inside scenery, as GroundBox's lies in Volcano's cone).
Vec aim(const GameObject &area, const Vec &from) {
	const de::Transform t = area.world();
	if (area.shape != GameObject::Shape::Box) return t.t;
	auto l = t.inverse_rotate({from[0] - t.t[0], from[1] - t.t[1], from[2] - t.t[2]});
	for (int k = 0; k < 3; ++k) l[k] = std::clamp(l[k], area.box_min[size_t(k)], area.box_max[size_t(k)]);
	const auto w = t.rotate(l);
	return {t.t[0] + w[0], t.t[1] + w[1], t.t[2] + w[2]};
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
			l.to = w_.referent(o, lists[2][0][0]);
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

// GetPathAffector (Creature 0x1020ef70, 2.123): the scene's test sphere at the point against group
// 0x2000 (SetupCreature's filter), radius 0, then 5, then squared, until it touches something
// (null past 500); the first touched affector that is one of the links' areas. Touching only
// others, the sphere grows by 100 while under 500 (2.112 tested the same sphere again, forever).
GameObject *Navigation::area_at(const Vec &p) {
	GameObject *probe = w_.create("NavigationTestSphere", "Actor\\Affector\\Sphere");
	if (probe->shape == GameObject::Shape::None) w_.shapes_changed();
	probe->shape = GameObject::Shape::Sphere;
	probe->local = {};
	probe->local.t = p;
	probe->hotspot = 0;
	for (float r = 0;;) {
		probe->falloff = r;
		auto hits = touches(w_, *probe, 0x2000);
		if (hits.empty()) {
			r = r == 0 ? r + 5 : r * r;
			if (r > 500) return nullptr;
			continue;
		}
		for (const Contact &h : hits) {
			if (std::find(areas_.begin(), areas_.end(), h.touched) != areas_.end()) return h.touched;
		}
		r += 100;
		if (!(r < 500)) return nullptr;
	}
}

// Not as ours searches: in 2.123 a point on a floor touches the floor at radius 0, so the sphere
// grows by 100 to 400 (FINDINGS 221); ours only sees navigation areas and gives up after 25.
// Where that finds nothing (Volcano's outer ground lies 29 under its GroundBox), search as on a floor,
// for an area about the point's own level (not a ramp's top above it, which cannot be walked to).
GameObject *Navigation::area_near(const Vec &p) {
	if (GameObject *a = area_at(p)) return a;
	GameObject *probe = w_.create("NavigationTestSphere", "Actor\\Affector\\Sphere");
	for (float r = 100; r < 500; r += 100) {
		probe->local.t = p;
		probe->falloff = r;
		for (const Contact &h : touches(w_, *probe, 0x2000)) {
			if (std::find(areas_.begin(), areas_.end(), h.touched) == areas_.end()) continue;
			const float dy = h.touched->world().t[1] - p[1];
			if (dy < 40 && dy > -60) return h.touched;
		}
	}
	return nullptr;
}

void Navigation::calculate(Path &path, GameObject &creature, const Vec &dest) {
	path.areas.clear();
	path.dest = dest;
	GameObject *from = area_near(creature.world().t), *to = area_near(dest);
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

// A steep surface of the arena (a wall, the side of a block) on the straight line from the
// creature's focal point to `to` at the same height above the ground: walkable floor in between
// (a dune) does not count.
bool Navigation::wall_between(GameObject &creature, const Vec &to) {
	const Vec me = creature.world().t, from = focal_point(creature);
	const Vec at = {to[0], to[1] + (from[1] - me[1]), to[2]};
	auto c = segment(w_, from, at, 0x200000);
	return c && c->face_normal[1] < 0.75f;
}

// GetMoveToPosition: drop the next area (or the one after it) once inside it; head for the next
// area left, else for the destination.
//
// Not in the original, two ways round walls the areas do not account for:
//  - an area is dropped as soon as the creature is anywhere inside it, and areas can reach into
//    solid blocks (Desert's pyramid corners), so heading straight for the next point from the
//    area's edge can run into the block. While a wall stands between the creature and the next
//    point, it keeps heading for the dropped area's centre (where the arena's author placed it, in
//    the open), and goes on once the way is clear or it is there;
//  - with a wall between it and the point it heads for (no areas left, both ends in one area, or
//    a link straight across the walled volcano from Volcano's GroundBox), it follows the wall
//    (around_wall).
Vec Navigation::move_to(Path &path, GameObject &creature, float *distance) {
	Vec me = creature.world().t;
	auto &a = path.areas;
	if (!a.empty()) {
		if (inside(*a.back(), me)) {
			path.held = a.back();
			a.pop_back();
		} else if (a.size() > 1 && inside(*a[a.size() - 2], me)) {
			path.held = a[a.size() - 2];
			a.pop_back();
			a.pop_back();
		} else if (a.size() > 2 && inside(*a.front(), me)) {
			a.clear();
		}
	}
	Vec next = a.empty() ? path.dest : aim(*a.back(), me);
	if (path.held) {
		const Vec centre = aim(*path.held, me);
		const float flat = std::hypot(centre[0] - me[0], centre[2] - me[2]);
		if (flat < 30 || !wall_between(creature, next)) {
			path.held = nullptr;
		} else {
			if (distance) {
				float d = flat + dist(centre, next);
				for (size_t i = a.size(); i > 1; --i) d += dist(a[i - 1]->world().t, a[i - 2]->world().t);
				*distance = d;
			}
			return centre;
		}
	}
	if (!a.empty()) {
		float d = dist(next, me);
		for (size_t i = a.size() - 1; i > 0; --i) d += dist(a[i]->world().t, a[i - 1]->world().t);
		if (distance) *distance = d;
	} else {
		if (distance) *distance = dist(path.dest, me);
	}
	if (wall_between(creature, next)) return around_wall(path, creature, next);
	path.side = 0;
	return next;
}

// Not in the original: a wall stands between the creature and `target` (the next area or the
// destination) that the areas do not lead round.
// Head 120 units out along the first open direction turned 30, 60 ... 150 degrees from the target,
// towards the side picked when the wall was met (the first open one, left first), so it walks along
// the wall instead of into it and keeps going round the same way; with that side shut, the other.
Vec Navigation::around_wall(Path &path, GameObject &creature, const Vec &target) {
	const Vec me = creature.world().t;
	float dx = target[0] - me[0], dz = target[2] - me[2];
	const float len = std::hypot(dx, dz);
	if (len < 1) return target;
	dx /= len, dz /= len;
	auto open = [&](int side, Vec *out) {
		for (float deg = 30; deg <= 150; deg += 30) {
			const float a = float(side) * deg * 3.14159265f / 180, c = std::cos(a), s = std::sin(a);
			const Vec p = {me[0] + (dx * c - dz * s) * 120, me[1], me[2] + (dx * s + dz * c) * 120};
			if (!wall_between(creature, p)) {
				*out = p;
				return true;
			}
		}
		return false;
	};
	Vec p;
	for (int side : {path.side ? path.side : 1, path.side ? -path.side : -1}) {
		if (open(side, &p)) {
			path.side = side;
			return p;
		}
	}
	return target;
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
		const Vec me = creature.world().t;
		fprintf(stderr, "[nav %u] %s (%.0f %.0f %.0f) at %s -> %s (%.0f %.0f %.0f): %zu area(s)", w_.frame(), creature.name.c_str(), me[0], me[1], me[2],
		        from ? from->name.c_str() : "-", to ? to->name.c_str() : "-", dest[0], dest[1], dest[2], path.areas.size());
		for (auto it = path.areas.rbegin(); it != path.areas.rend(); ++it) fprintf(stderr, " %s", (*it)->name.c_str());
		if (path.held) fprintf(stderr, " (holding %s)", path.held->name.c_str());
		fprintf(stderr, "; heading for %.0f %.0f %.0f\n", (*waypoint)[0], (*waypoint)[1], (*waypoint)[2]);
	}
	return true;
}

float Navigation::scene_ms() const { return w_.scene_ms(); }

}  // namespace game
