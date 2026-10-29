// The lights of a frame (FINDINGS 89): every active Light at its sphere or cone affector, and
// which drawn actors each one reaches.
#pragma once

#include <functional>
#include <set>
#include <utility>
#include <vector>

#include "game/runtime.h"

namespace render {

struct FrameLights {
	// The lightable render affectors (collision group 0x20; DE_CStandardRenderAffector::
	// SetAffector) and their owners (the affector's +0x118, else itself, whose render actor is
	// lit: an affector that owns itself owns no meshes).
	std::vector<std::pair<game::GameObject *, game::GameObject *>> render_affectors;
	std::set<const game::GameObject *> owners;
	// A drawn actor's lighting owner is the nearest one up its tree; without one it takes no lights.
	const void *owner_of(const game::GameObject *o) const {
		for (; o; o = o->parent) {
			if (owners.count(o)) return o;
		}
		return nullptr;
	}
};

// Fills g_lighting.lights from the scene objects `in_scene_layer` accepts (all when empty) and
// returns the render affectors they light.
FrameLights collect_lights(game::World &world, int frame, const std::function<bool(const game::GameObject *)> &in_scene_layer = {});

}  // namespace render
