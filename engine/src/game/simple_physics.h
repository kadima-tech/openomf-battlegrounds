// Simple physics for actors that aren't fighters (see simple_physics.cpp).
#pragma once

#include <algorithm>

#include <array>
#include <map>
#include <vector>

#include "game/forces.h"
#include "game/runtime.h"

namespace game {

struct SimplePhysics {
	bool read = false;
	bool enabled = true;  // DE_CPhysics +0x38
	int32_t rate = 0;     // +0x40: 0 = DeltaTick every scene update, else Tick every `rate` ms
	float gravity = 0;    // +0x74
	std::array<float, 3> momentum{0, 0, 0}, accel{0, 0, 0};  // +0x80, +0x98 (world)
	bool bounce = false;  // +0x118
	float lost_on_bounce = 0, multiplier_on_bounce = 1, variation_on_bounce = 0;  // +0x11c..+0x124
	uint32_t groups = 0;  // +0x94
	bool push_from_surfaces = false;  // +0x7c
	bool pack_momentum = false;       // +0x128: its states carry the momentum (PackState 0x701dc5b0)
	std::vector<GameObject *> affectors;  // +0x50: what it tests
	GameObject *force = nullptr;          // +0x60: applied to what it touches
	std::vector<std::pair<Brain *, uint32_t>> bounce_hooks;
	std::vector<Contact> contacts;  // the last PerformCollisionTests' results (CollisionResults())
	uint32_t last_ms = 0;
	bool moving() const {
		return momentum[0] != 0 || momentum[1] != 0 || momentum[2] != 0 || accel[0] != 0 || accel[1] != 0 || accel[2] != 0 ||
		       gravity != 0;
	}
};

// DE_CMomentumMonitor (Core 0x701d88d0 Tick, 0x701d8450 UpdateOnDemand): how far its actor moved and turned.
struct MomentumMonitor {
	bool read = false;
	bool enabled = false;  // DE_CPhysics +0x38
	int32_t rate = 0;      // +0x40
	bool ticked = false;
	de::Transform prev, cur;  // +0x58/+0x94 and +0x64/+0x70: world transform at the last two ticks
	uint32_t asked_ms = 0;    // +0x48
	std::array<float, 3> asked_pos{0, 0, 0}, on_demand{0, 0, 0};  // rate 0: +0x58, +0xb8
};

class SimplePhysicsSystem {
public:
	explicit SimplePhysicsSystem(World &w) : w_(w) {}
	SimplePhysics &state(GameObject &physics);
	GameObject *actor_of(GameObject &physics);
	// The actor's physics (children and properties; CActor::Physics(i) indexes it).
	std::vector<GameObject *> physics_of(GameObject &actor);
	void tick();
	MomentumMonitor &monitor(GameObject &physics);
	void monitor_tick(GameObject &physics);
	// DE_CActor::MomentumAtPosition (Core 0x700906a0) / RotationalMomentum (0x700905c0), per ms.
	std::array<float, 3> momentum_at(GameObject &actor, const std::array<float, 3> &p);
	std::array<float, 3> rotational_momentum(GameObject &actor);
	// DE_CSimplePhysics::DeltaTick (Core 0x701dc060): a step over the scene time since its last.
	void delta_tick(GameObject &physics);
	// The clock of `session`'s scene went back to `now`: nothing there has run after it.
	void clock_back(uint32_t now, const GameObject *session) {
		for (auto &[o, s] : states_) {
			if (w_.clock_key(o) == session) s.last_ms = std::min(s.last_ms, now);
		}
	}

private:
	void step(GameObject &p, SimplePhysics &s, GameObject &actor, float scale, bool delta);
	void bounce(GameObject &p, SimplePhysics &s, const std::vector<struct Contact> &contacts);
	World &w_;
	std::map<const GameObject *, SimplePhysics> states_;
	std::map<const GameObject *, MomentumMonitor> monitors_;
	std::map<std::pair<const GameObject *, const GameObject *>, GameObject *> monitor_copies_;
	std::map<const GameObject *, GameObject *> actors_;
	std::map<const GameObject *, std::vector<GameObject *>> lists_;
};

// The rotation by an axis times its angle (radians).
de::Transform rotation_of(const std::array<float, 3> &w);

void register_simple_physics_natives(World &w);

}  // namespace game
