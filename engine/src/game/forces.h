// Forces and materials (Core DE_CForce, DE_CCommonForce, DE_CMaterial and its particle-spawning
// subclasses, DE_CAffector::ApplyForce, DE_CSimplePhysics::PerformCollisionTests).
//
// A force is a property of an actor. Applying it from an affector (a foot's sphere, a fighter's
// floor sphere, an explosion's rumble sphere) collision-tests that affector against the affectors
// in the given groups and sends each touched one a collision event carrying the force. The
// touched affector's materials process it:
//  - DE_CMaterial: an optional mesh element filter (the face's byte channel of that name) and
//    incident rate limit, then every script hook registered with RegisterProcessForceHook runs
//    with (force, touch point, push-back normal);
//  - DE_CCommonForceParticleSpawningMaterial: only common forces whose type bits meet the
//    material's mask, and only within its size and strength ranges; spawns one particle (size
//    and life span interpolated over those ranges) from the material's manual spawner and
//    copies the material's actor to create at the touch point, then runs the hooks.
// Common forces with a rate tick themselves: every `rate` ms they test their own affector
// against group 0x200 and apply themselves to what it touches (the bounce rumble).
// A Physics\Simple with a force tests its actor every tick (group from its contents) and
// applies the force to every touched surface: a fighter's floor sphere carries PlayerForce
// (type 0x20) into jump pads, mine fields and the death volume this way.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "de/value.h"
#include "game/runtime.h"

namespace game {

// DE_CAffector::DE_CCollisionResults::DE_CSingleCollisionInfo: what a test touched and where.
struct Contact {
	GameObject *source = nullptr;   // the testing affector that touched
	GameObject *touched = nullptr;  // the affector it touched (+0x00)
	std::array<float, 3> point{0, 0, 0};   // touch point (+0x24)
	std::array<float, 3> normal{0, 1, 0};  // push-back normal (+0x08): from the surface towards the source
	std::array<float, 3> face_normal{0, 1, 0};  // the touched surface's own normal (+0x18)
	float distance = 0;   // source centre to touch point
	float depth = 0;      // how far the source sphere reaches past the surface (+0x14)
	int face = -1;        // mesh affector face (+0x30)
};

// Does `source` (a sphere) touch `target`? Spheres, boxes, meshes and cones (a cone only against a
// sphere).
std::optional<Contact> touch(World &w, GameObject &source, GameObject &target);
// Every face of a mesh affector that `source` touches.
std::vector<Contact> touch_faces(World &w, GameObject &source, GameObject &target);
// Every active scene affector in `groups` that `source` touches (not `source` itself or its own
// branch's fighter), in the scene's affector order (DE_CAffector::IsTouching 0x7007b900 does not
// sort; scripts ask for SortByTouchDistance when they want the nearest first).
std::vector<Contact> touches(World &w, GameObject &source, uint32_t groups);
// The nearest mesh or flat box surface in `groups` the segment a-b meets.
std::optional<Contact> segment(World &w, const std::array<float, 3> &a, const std::array<float, 3> &b, uint32_t groups);
// DE_CScene::TestLineSegment (Core 0x700731b0): the scene's ray cone (penumbra 0, far end the
// length) against every active affector in `groups`, in scene order: each mesh face it crosses
// from behind, in face order, facing the start (only the first with `one_per_mesh`,
// NoRepeatOwners); one collision for a sphere or a flat box (only its touch point).
std::vector<Contact> segment_hits(World &w, const std::array<float, 3> &a, const std::array<float, 3> &b, uint32_t groups, bool one_per_mesh = false);
// Strength of a sphere / cone / box affector at a point (DE_CAffector vtable +0x164): the falloff
// profile times its "Intensity" value (1 unless set).
float affector_intensity_at(World &w, GameObject &a, const std::array<float, 3> &p);

struct CommonForceDef {
	uint32_t type = 0;            // +0x54
	de::FloatValue strength;      // +0x58 (advanced reference; SetStrength sets the value)
	float size = 1;               // +0x6c
	int32_t mode = 0;             // +0x50: 0 strength, 1 strength x |momentum|, 2 strength x (1 + |momentum|)
	int32_t rate = 0;             // +0x70: ms between self-ticks (0: applied by others only)
};

struct MaterialDef {
	std::string element;          // +0x5c: mesh element (byte data channel) the material covers
	int32_t threshold = 0;        // +0x64: incident recorder threshold (ms)
	float value = 1;              // +0x6c
	bool spawning = false;        // ParticleSpawningMaterial
	de::ObjectRef renderer;       // +0x74
	de::ObjectRef spawner;        // +0x80
	int32_t spawn_mode = 0;       // +0x88: 0 at the point, 1 oriented by the normal and pushed out
	float spawn_offset = 0;       // +0x8c (x particle size along the normal)
	bool common = false;          // CommonForceParticleSpawningMaterial
	uint32_t mask = 0;            // +0x9c
	bool mesh_heights = false;    // +0xc4: the created actor takes the touched mesh face's heights (not ported)
	float size_lo = 0, size_hi = 1000, strength_lo = 0, strength_hi = 1000;  // +0xa0..+0xac
	float psize_lo = 1, psize_hi = 100, life_lo = 1000, life_hi = 10000;      // +0xb0..+0xbc
	float intensity_lo = 1;       // +0xc0: particle intensity at the lowest strength
	de::ObjectRef actor_to_create;  // +0x94
};

class ForceSystem {
public:
	explicit ForceSystem(World &w) : w_(w) {}
	// CAffector::ApplyForce(force, groups): true when the source touched something.
	bool apply(GameObject &source, GameObject &force, uint32_t groups);
	// The collision event a force sends to what it touched (DE_CMaterial::ProcessEvent).
	void deliver(GameObject &force, const Contact &c);
	// Self-ticking forces and force-applying physics, once per game frame.
	void tick();

	CommonForceDef &force(GameObject &o);
	bool is_force(const GameObject &o) const { return o.is("UniqueActorProperty\\Force"); }
	bool is_common_force(const GameObject &o) const { return o.is("UniqueActorProperty\\Force\\Common"); }
	// DE_CCommonForce::Strength.
	float strength(GameObject &force);
	// DE_CCommonForce::StrengthAtPosition: its affector's intensity there times its strength.
	float strength_at(GameObject &force, const std::array<float, 3> &p);
	// DE_CUniqueActorProperty +0x20: the actor a force or material belongs to.
	GameObject *actor_of(GameObject &property);
	void set_actor(GameObject &property, GameObject *actor) { owners_[&property] = actor; }
	// A per-actor copy of a unique property the actor references from elsewhere (forces shared
	// by every fighter's data, ...): each actor gets its own, as the engine attaches them.
	GameObject *unique_property(GameObject &actor, GameObject &shared);

	const MaterialDef &material(GameObject &o);
	std::vector<GameObject *> materials_of(GameObject &affector);
	bool valid_surface(GameObject &material, const Contact &c);
	GameObject *material_of(const Contact &c);
	void add_hook(GameObject &material, Brain *b, uint32_t method) { hooks_[&material].push_back({b, method}); }

	// Camera rumble and debug: forces delivered this frame.
	uint32_t delivered = 0;

	// DE_CFighterPhysics (OMF Tick 0x35039970, UpdateCollisionData 0x3503ad60): the fighter's
	// floor sphere (the physics' test affector) against the surfaces in its groups.
	struct FighterSurfaces {
		bool resolved = false;
		GameObject *sphere = nullptr;  // physics +0x50[0] (FloorCollisionAffector)
		uint32_t groups = 0;           // physics +0x94 (0x200000: the fighting surfaces)
		GameObject *force = nullptr;   // fighter +0x5a0 (PlayerForce)
		std::vector<Contact> contacts; // physics results (+0xe4)
		std::array<float, 3> accel{0, 0, 0};  // +0x98: gravity while airborne
		float friction = 1;            // +0x100: the floor contact's material value
		GameObject *push_sphere = nullptr;  // +0x1b0: "OtherFighterPushSphere"
		bool skip_push = false;        // +0x188: SkipFighterCollision, for one tick
		std::array<float, 3> last_pos{0, 0, 0};  // +0x178: where the last tick left the fighter
		std::vector<std::pair<Brain *, uint32_t>> hit_wall, touch_floor, leave_floor, surface_change;
	};
	FighterSurfaces &surfaces(GameObject &fighter);
	// One physics tick; false when the fighter has no floor sphere or the scene no surfaces (the
	// caller keeps its height-field stand-in).
	bool fighter_tick(GameObject &fighter);
	// The push against the other fighters (both physics paths call it).
	void push_against_fighters(GameObject &fighter);
	// COMF2Fighter::ApplySurfaceCollisionForce (0x3502ab60): the fighter's force on every
	// surface it touched last tick, once each, at 100 x multiplier x (momentum . normal) + 3
	// (3 when it is not moving).
	void apply_surface_collision_force(GameObject &fighter, float multiplier);
	// COMF2Fighter::NumWallSurfaces / AverageWallSurfaceNormal over the last tick's contacts.
	std::vector<const Contact *> walls(GameObject &fighter);

private:
	void process(GameObject &material, GameObject &force, const Contact &c);
	bool collide(GameObject &fighter, FighterSurfaces &fs, bool probe, bool was_on_floor);  // UpdateCollisionData
	std::map<const GameObject *, FighterSurfaces> fighters_;
	World &w_;
	std::map<const GameObject *, CommonForceDef> forces_;
	std::map<const GameObject *, MaterialDef> materials_;
	std::map<const GameObject *, std::vector<GameObject *>> affector_materials_;
	std::map<const GameObject *, std::vector<std::pair<Brain *, uint32_t>>> hooks_;
	std::map<const GameObject *, GameObject *> owners_;
	std::map<std::pair<const GameObject *, const GameObject *>, GameObject *> copies_;
	std::map<const GameObject *, uint32_t> last_tick_;  // self-ticking forces: scene ms of the last tick
	std::map<const GameObject *, World::IncidentRecorder> incidents_;  // each material's recorder (+0x68)
	uint32_t vec_a_ = 0, vec_b_ = 0;  // hook argument blocks
};

void register_force_natives(World &w);

}  // namespace game
