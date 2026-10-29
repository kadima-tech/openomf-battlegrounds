// The observer camera's autopilot (Creature module): DE_CObserveCommand and the command manager
// path for creatures whose DE_CCommandManager has native command executors (DE_CCommandManager
// DeltaTick 0x1501e1b0 / ReassignExecutors 0x1501dd30, the DigitalOrientationPropulsion,
// MouseAxisNavigation and Teleport executors). BObserverCamera (Session.DEObjects) flies the
// ObserverCamera creature from what the executors leave: the PropelOffsetX/Y/Z values and the
// mouse controller's analog axes. Fighters keep World::run_commands (hooks).
//
// The observer only reads the simulation (positions, GlobalInterest, line of sight) and draws
// from its own MSVCRT rand() stream, so it never changes what a match or a replay does.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "game/runtime.h"

namespace game {

class ObserverSystem {
public:
	explicit ObserverSystem(World &w);
	~ObserverSystem();
	// DE_CCreature::CommandObserveInterestingCreatures / CommandObserve (0x15017bc0 / 0x15017960 /
	// 0x15017a80): forget every command, then receive a new observe command.
	void observe_interesting(GameObject &creature, float threshold);
	void observe(GameObject &creature, const std::vector<GameObject *> &targets);
	// DE_CCreature::DeltaTick (0x15016700): once per scene time, the command manager's DeltaTick.
	void delta_tick(GameObject &creature, float delta);
	// The creature that last received an observe command and is still in the scene (the VCR's).
	GameObject *current();
	struct Manager;

private:
	Manager &manager(GameObject &creature);
	World &w_;
	std::map<const GameObject *, std::unique_ptr<Manager>> managers_;
	GameObject *last_ = nullptr;
};

// DE_CCreature::Camera(i): the i-th actor of the creature's camera list (references 2.0); a
// fighter's passive reference is replaced by the camera it made, the actor carrying a Camera.
GameObject *creature_camera(World &w, GameObject *creature, int index);
// Get2DOrientation of a controller that is not a fighter's: the keyboard when bound to a device,
// else its default orientation (SetDefault2DOrientation).
int controller_orientation(World &w, const GameObject *controller);
// CRenderer::SetCamera (Core DE_CRenderer::SetCamera 0x70184030): the view's actor; the scene's
// relays hear a RendererSetCamera generic event (Object1 the renderer).
void set_renderer_camera(World &w, GameObject *actor);
void register_observer_natives(World &w);

}  // namespace game
