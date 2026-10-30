// Game runtime: native objects that compiled scripts talk to.
//
// Every script-visible object is a GameObject with a handle: the arena address of a small
// block, so "pointers" held by scripts stay 32-bit (see vm/memory.h). Intrinsics are looked up
// by "CClass::signature" exactly as the bytecode names them (VMC2 intrinsic table), e.g.
// "CActor::class CScene Scene()". Arguments follow the original packing: slot 0 is `this` and
// receives the return value; parameters follow at their natural sizes (byte 1, short 2,
// int/float/bool/pointers 4).
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "de/anim.h"
#include "de/objects_file.h"
#include "de/scene.h"
#include "vm/executor.h"
#include "vm/script.h"

namespace game {

class World;
struct Brain;

// COMF2Fighter movement tuning (fighter object contents v2, DE_COMF2Fighter::ObjProcessContents
// 0x35024910; defaults from the constructor). Speeds are units/frame, turns degrees/frame, at the
// fighter scripts' 60 frames per second.
struct FighterStats {
	float run = 2.6f, run_turning = 2.4f;                          // +0x4c4/+0x4c8 (turning = 0.6x run)
	float run_turn_max = 1.0f, run_turn_step = 0.16f;              // +0x4cc/+0x4d0
	float turn_max = 3.0f, turn_step = 0.16f;                     // +0x4d4/+0x4d8 turning on the spot
	float walk = 1.45f, walk_turning = 1.0149f;                    // +0x4dc/+0x4e0
	float walk_turn_max = 1.0f, walk_turn_step = 0.19f;            // +0x4e4/+0x4e8
	float back = -1.16f, back_turning = -0.812f;                   // +0x4ec/+0x4f0
	float back_turn_max = 0.9f, back_turn_step = 0.16f;            // +0x4f4/+0x4f8
	float air_turn_max = 2.0f, air_turn_step = 0.16f;              // +0x4fc/+0x500
	float air_forward = 0.0122f, air_backward = -0.0122f;          // +0x504/+0x508
	float jump_forward = 0.45f, jump_backward = 0.35f, jump_up = 1.2f;    // +0x514/+0x518/+0x51c
	float super_forward = 0.35f, super_backward = 0.28f, super_up = 1.6f; // +0x520/+0x524/+0x528
	float air_gravity = 0;  // +0x52c: extra gravity JumpStandard adds each airborne frame (x 0.037)
	float push_size = 20;   // +0x50c: body size used when fighters push against each other
	float walk_lean = 0, run_lean = 0, lean_step = 0;  // +0x530 / +0x538 / +0x53c: AnimTurn (degrees)
	uint16_t block_mask = 0xf01f;  // +0x44c: the 16 attack directions (22.5 deg sectors, bit 0 = head-on) it can block
	// +0x540..+0x59c: attack trailer offset and half-width vectors (emitter space) for the right
	// punch, left punch, right kick and left kick.
	std::array<float, 24> trailer_vectors{};
};
// Pilot base stats (DE_CPilot +0x3b0..0x3bc: power, agility, endurance, focus, 0..100), read
// from the pilot's last contents chunk (v4) after its three colour references.
struct PilotStats {
	float power = 50, agility = 50, endurance = 50, focus = 50;
};

// DE_CPilot::CVariedStat (0x20 bytes): base + amplitude x sin((time + phase) x frequency x k),
// k by wave type {0.001, 0.002, 0.004, 0.015}. The pilot's data gives each stat's level (0..8),
// variation (0..5) and wave; DE_CPilot::CalculateAIValues (OMF 0x3504bf50) turns those into
// base/amplitude with per-stat phase, frequency and exponent.
struct VariedStat {
	uint32_t level = 4, variation = 0, wave = 0;
	float base = 1, amplitude = 0, phase = 0, frequency = 0;
	float value(float time_ms) const;
};
struct Pilot {
	std::string name;
	PilotStats stats;
	// DE_CPilot +0x368 + k x 0x18: the three user-defined colours (UserDefinedColor 0..2) that
	// paint the pilot's fighter, from the embedded RGBA of each colour reference.
	std::array<std::array<float, 3>, 3> colours{{{0.75f, 0.78f, 0.82f}, {0.10f, 0.35f, 0.85f}, {1.00f, 0.80f, 0.15f}}};
	std::map<uint32_t, VariedStat> ai;  // by DE_CPilot offset (0x3e8 AffinityLeft ... 0x7a8 EmotionValor)
	float quality = 0.43f;              // +0x7cc: AI quality degradation (higher = clumsier)
	int difficulty = 2;                 // DE_COMF2Manager::AIDifficulty: 0 Rookie .. 4 World Champion
	float difficulty_offset = 0;        // +0x3e4 (CPilot::SetAIDifficultyOffset)
	float stat(uint32_t offset, float time_ms) const;
	// The difficulty-dependent part of CalculateAIValues: quality degradation
	// clamp(1 - (difficulty + offset) / 3.5, 0.01, 0.99) and the perception stats built on it.
	void apply_difficulty();
};
// `difficulty` is the app's AI difficulty (DE_COMF2Manager::AIDifficulty, normally from the
// options screen): 0 Rookie, 1 Amateur, 2 Veteran, 3 Champion, 4 World Champion.
std::optional<Pilot> read_pilot(const de::ObjectsFile &f, const de::ObjectInfo &o, int difficulty);
std::optional<FighterStats> read_fighter_stats(const de::ObjectsFile &f, const de::ObjectInfo &o);

// Controller state (polled by the COMF2Fighter::IsJoystick*/Is*ButtonDown natives): the keyboard
// for the player, the AI's CCreatureController defaults for a computer fighter.
struct Input {
	bool up = false, down = false, left = false, right = false;
	bool left_punch = false, right_punch = false, left_kick = false, right_kick = false;
	bool jump = false, evade = false, duck = false;  // duck and evade are one button (id 5) in the original
	bool duck_evade() const { return duck || evade; }
};

struct GameObject {
	uint32_t handle = 0;
	const de::ObjectsFile *file = nullptr;
	uint32_t index = 0;
	std::string name;
	std::string class_path;
	GameObject *parent = nullptr;
	std::vector<GameObject *> children;
	std::vector<GameObject *> components;  // actor component references (meshes, properties, ...)
	bool is_actor = false;
	de::Transform local;
	bool visible = true;
	int32_t position_mode = 2;  // DE_CActor +0x10c (0 inherit, 1 static, 2 dynamic)
	uint32_t actor_flags = 6;   // DE_CActor +0x110 (2 shown at low detail, 4 at high detail)
	bool meshes_disabled = false;  // CActor::DisableAllMeshProperties: its own mesh properties are not drawn

	// Creature / fighter state.
	Brain *brain = nullptr;
	const de::AnimBank *bank = nullptr;  // animation bank used by this creature
	int sequence = -1;                   // index into bank->sequences
	float anim_time = 0;
	// AnimActivatePrelude (Creature 0x150159c0): the current pose is captured and anim_time 0..1
	// blends from it to `sequence` at prelude_time; AnimActivate ends the prelude.
	bool prelude = false;
	float prelude_time = 0;
	// The animated body's extra rotation (+0x458): AnimRotate*, AnimTurn(action) and AnimRotateTo
	// turn it about the fighter's axes; DE_COMF2Fighter::ValidateAnimationTime (0x35026110) turns
	// the Pelvis by it when it next poses the skeleton, and resets it.
	std::array<float, 9> anim_rot{1, 0, 0, 0, 1, 0, 0, 0, 1};
	// DE_CCreature +0x1c8: the bone actors show the current animation time. AnimSetTime clears it
	// and validates at once (ValidateAnimationTime); a prelude's capture validates first.
	bool pose_valid = false;
	std::vector<de::Transform> pose_now;       // the last validated pose (model space, per channel)
	std::vector<de::Transform> pose_captured;  // the pose a prelude blends from
	float anim_turn = 0;  // COMF2Fighter::AnimTurn: the walk/run lean (+0x450, degrees)
	bool skip_friction = false;  // COMF2Fighter::SkipFriction (physics +0x184): no slowdown on the next floor tick
	// AnimCaptureAsInitial / AnimCaptureAsFinal + AnimActivateEmpty: a move generated at run time,
	// anim_time blending from the captured initial pose to the captured final one.
	struct Capture {
		int sequence = -1;
		float time = 0;
	} capture_initial, capture_final;
	bool captured_blend = false;
	uint32_t activations = 0;  // bumped on every activation so renderers know when to capture
	// Fighter physics (DE_CFighterPhysics, the fighter's +0x1cc).
	bool fighter = false;  // has FighterStats and is moved by World::tick's physics step
	bool spawned = false;  // a fighter prepared into the scene (DE_CFighterPhysics +0x1a8): in play
	bool user_controlled = false;  // reads World::input; otherwise driven by AI commands
	bool autoplayed = false;       // OPENBG_AUTOPLAY: AI-driven, but still the user's to the scripts
	const Pilot *pilot = nullptr;  // the fighter's pilot (COMF2Fighter::Pilot())
	const std::map<std::string, std::vector<struct InputRequirement>> *inputs = nullptr;  // kind 24 bank

	// AI commands (DE_CCommand: navigation / look-at / attack) and the fighter script's hooks
	// (CStandardHookupExecutor::Register*Hook method handles), run each frame by World::tick.
	// DE_CCommand objects in the creature's DE_CCommandManager list, in arrival order.
	struct Command {
		uint32_t id = 0;
		uint32_t type = 0;      // 2 navigation, 4 look-at, 0x10 attack, 0x20 aquire
		uint32_t target = 0;    // actor handle (attack, aquire, look-at an actor)
		float value = 0;        // +0x78: aggression (attack) or urgency (aquire)
		std::array<float, 3> pos{0, 0, 0};   // move-to destination, or look-at position
		float distance = 0;     // move-to required distance (+0x80)
		std::array<float, 3> step{0, 0, 0};  // Update: move-to position on the path (+0x70)
		float step_distance = 0;             // ... and the distance left (+0x7c)
		bool updated = false;
		float sort = 1;         // +0x4c: priority (+0x48, always 1) after ModifySort
	};
	struct CommandManager {
		std::vector<Command> list;
		std::vector<size_t> order;     // +0x84: list indices, sorted
		bool dirty = true;             // +0xa0: sort before the next SortedCommand
		bool reassign = false;         // +0xa4
		uint32_t next_id = 1;
		// The creature's one controller (CControllerInfo).
		uint32_t candidate = 0;        // +0x2c/+0x34: the primary being chosen (command id)
		float candidate_conf = 0;      // +0x30
		uint32_t active = 0;           // +0x38: the primary command (0: none)
		std::vector<uint32_t> backups; // +0x40: alternate commands
		int counter = 0;               // +0x3c, never reset
		bool acted = true;             // hookup executor +0x30 (ExecutorDidNothing clears it)
		Command *find(uint32_t id) {
			for (auto &c : list) {
				if (c.id == id) return &c;
			}
			return nullptr;
		}
		bool type_active(uint32_t mask) const {
			for (auto &c : list) {
				if (c.type & mask) return true;
			}
			return false;
		}
	} commands;
	uint32_t hook_attack = 0, hook_aquire = 0, hook_move = 0, hook_look = 0;
	Input ai_input;                        // CCreatureController::SetDefault* (computer fighters)
	std::array<float, 22> attack_usage{};  // pilot +0x7d4: per attack category, for variety
	int32_t usage_second = 0;              // pilot +0x82c
	float ai_range_multiplier = 1;         // fighter +0x5f8 (AISetAttackRangeMultiplier)
	uint32_t momentum_vec = 0;             // CVector block returned by Momentum()
	uint32_t orientation_block = 0;        // CCoordinateSystem block returned by WorldOrientation()
	uint32_t local_orientation_block = 0;  // ... and by LocalOrientation() (the actor's own)
	GameObject *last_hit = nullptr, *last_hit_by = nullptr;  // physics +0x134 / +0x140
	int invulnerable_frames = 0;
	uint32_t creature_ticks = 0;    // DE_CCreature +0x1d4: updates run (Tick adds one at its end)
	float ai_range_base = 1;        // fighter +0x5f4: AIAttackRange's base, jittered every 64 updates
	bool temporal_registered = false;  // in its rate's temporal set
	uint64_t affector_seq = 0;  // its place in the scene's affector list (0: not in it)
	std::vector<uint32_t> linked_fighters;  // fighter +0x334, filled pairwise by ObjPrepare
	bool fighter_prepared = false;
	bool reference_created = false;  // a PassiveObjectReference that has made its object
	int32_t input_device = -1;       // a creature controller's bound device (+0x2c), -1 none
	float move_multiplier = 1, turn_multiplier = 1;  // fighter +0x4bc / +0x4c0
	float global_interest = 0;             // CActor::SetGlobalInterest (fighter +500)
	uint32_t interest_time = 0;            // fighter +0x488: scene ms GlobalInterest was last worked out
	float interest_cached = 1;             // fighter +0x48c
	uint32_t faction = 0;                  // pilot +0x32c (a handle): CPilot::SetFaction (team games)
	int32_t team = -1;                     // fighter +0x49c: its pilot's faction's TeamIndex when prepared
	std::array<float, 4> analog_axis{0, 0, 0, 0};  // a creature controller's analog movement (+0x90)
	uint8_t pad_orientation = 8;           // a non-fighter pad's default 2D orientation (SetDefault2DOrientation)
	uint16_t pad_record = 0;               // a bound non-fighter pad's last input record's directions (0: none)
	float camera_fov = 0;                  // DE_CCamera +0x2c set at run time (0: the data's)
	uint32_t current_target = 0;           // fighter +0x30c (a handle): CurrentTarget's pick
	uint32_t target_checked = 0;           // +0x4a4: scene ms the pick was last reconsidered
	uint32_t target_chosen = 0;            // +0x4a8: scene ms it was chosen
	uint32_t pilot_target = 0;             // pilot +0x338: current attack target
	float pilot_target_since = 0;          // pilot +0x3e0: when it became the target (ms)
	float quality_offset = 0;              // pilot +0x3d0: set by Attack on a new target
	float pilot_aggression = 0;            // pilot +0x3dc: the aggression of its last Attack
	std::vector<uint32_t> love, hate;      // pilot +0x348 / +0x35c: pilot handles
	// DE_COMF2Fighter::Guide state (physics +0x200 mode, +0x204 power, +0x208 frames).
	int guide_mode = 0, guide_frames = -1;
	float guide_power = 0;
	std::array<uint32_t, 2> ai_vec{0, 0};  // CVector argument blocks for the hooks
	FighterStats stats;
	std::array<float, 3> momentum{0, 0, 0};  // world units/frame
	float gravity = 1.0f;          // physics +0x19c, recovers towards 1 by gravity_recover per frame
	float gravity_recover = 1.0f;  // physics +0x1a0
	float traction = 1.0f;         // fighter +0x448
	bool on_floor = false;         // physics +0x18c
	bool propelled = false;        // physics +0x184: Propel ran this frame (no surface friction)
	float turn_rate = 0;           // physics +0x194: degrees/frame (positive turns left)
	float forward_speed = 0;       // physics +0x198: target speed for Propel

	// Affectors (Actor\\Affector\\*: DE_CAffector +0x174 collision groups; DE_CSphereAffector
	// hotspot +0x184 / falloff +0x17c; DE_CConeAffector near/far attenuation and umbra/penumbra).
	// DE_CBoxAffector min/max corners (its frame); DE_CMeshAffector's mesh (references chunk 3).
	enum class Shape : uint8_t { None, Sphere, Cone, Mesh, Box } shape = Shape::None;
	std::array<float, 3> box_min{0, 0, 0}, box_max{0, 0, 0};
	de::ObjectRef mesh;
	uint32_t groups = 0;
	bool active = true;
	bool detached = false;  // a global entry's template: outside the scene (no hits, no danger)
	bool removed = false;   // CScene::Remove: out of the scene until CScene::Add brings it back
	bool gui = false;       // a root of the front end's GUI scene (ui.h), drawn with its own camera
	int view = 0;           // a GUI root's scene: 0 the GUI's own, 1 a scene viewer control's
	float hotspot = 0, falloff = 0;                       // sphere radii
	float near_begin = 0, near_end = 0, far_begin = 0, far_end = 0, umbra = 0, penumbra = 0;  // cone
	GameObject *owner_actor = nullptr;                    // CAffector::SetOwnerActor (+0x118)
	GameObject *creature_owner = nullptr;                 // CCreature::SetOwner (a projectile's fighter)
	GameObject *component_owner = nullptr;                // CCreature::AddComponent (+0x218)
	GameObject *data_owner = nullptr;                     // +0x118 from the data (references chunk 2)
	bool data_owner_read = false;
	bool encompasser = false;                             // +0x16c: AutoEncompase may choose it
	bool auto_encompass = false;                          // +0x170: finds its encompasser when prepared
	GameObject *data_encompasser = nullptr;               // +0x124 from the data (references chunk 2 v4)
	bool data_encompasser_read = false;
	std::array<GameObject *, 2> carry{nullptr, nullptr};  // fighter: left / right hand carryable
	// DE_CCreature +0x1dc SceneTicksPerUpdate (creature contents; -1 not read yet) and +0x1d8
	// creation time.
	int32_t ticks_per_update = -1;
	bool physics_enabled = true;  // CActor::Enable/DisablePhysics
	bool path_moved = false;      // placed by an AnimationStreamPlayer path (drawn live)
	bool copied = false;          // made at run time (NewCopy, passive references): drawn live
	// DE_CPassiveActorReference::Create appends the reference's own properties to the actor's.
	std::vector<de::ObjectRef> extra_properties;
	uint32_t creation_time = 0;
	uint32_t anchor_prepared_frame = 0;
	float danger_level = 0, danger_delay = 0;             // a CDanger property on this affector
	bool has_danger = false;
	int channel = -2;  // animation channel driving this bone actor (-1 none, -2 not looked up)
	// A separate AnimationSequenceManager (Gargoyle's wings, DE_CAnimationSequenceManager): its
	// bank, its channels' actors, and a prelude's per-channel start and end locals. `managed`
	// marks a bone actor such a manager drives: the body's pose then writes it only from a stream.
	const de::AnimBank *manager_bank = nullptr;
	std::vector<GameObject *> manager_actors;
	std::vector<de::Transform> manager_initial, manager_final;
	bool managed = false;

	uint32_t vec_world_pos = 0;  // arena CVector returned by WorldPosition()
	uint32_t vec_local_pos = 0;

	bool is(const char *path) const { return class_path == path || class_path.rfind(std::string(path) + "\\", 0) == 0; }
	de::Transform world() const;
};

void read_affector(GameObject &g, const de::World &data, const de::ObjectsFile &f, const de::ObjectInfo &o);
// DE_COMF2Fighter::FocalPoint (OMF 0x3502ab10): a fighter's "Pelvis" actor, else the position.
std::array<float, 3> focal_point(const GameObject &o);
// Not under a template kept outside the scene, nor left out at this detail level
// (DE_CActor::ObjPrepare / UpdateForDetailLevel).
// A child actor that is inactive (+0x110 bit 8 clear) or not shown at this detail level is parked
// in its parent's inactive list (DE_CActor::ObjSetParent / SetInnactive, Core 0x7008afb0 /
// 0x700903e0): it and its branch are out of the scene. A root added to the scene is in it whatever
// its own flag.
inline bool parked(const GameObject &o) { return o.is_actor && (!o.active || !de::shown_at_detail(o.actor_flags)); }
inline bool in_scene(const GameObject &o) {
	const GameObject *r = &o;
	for (; r->parent; r = r->parent) {
		if (r->removed || parked(*r)) return false;
	}
	return !r->detached && !r->removed && !(r->is_actor && !de::shown_at_detail(r->actor_flags));
}
// In the GUI scene: under a root the front end put there.
inline bool in_gui(const GameObject &o) {
	const GameObject *r = &o;
	while (r->parent) r = r->parent;
	return r->gui;
}
// The scene a GUI object is in (GameObject::view of its root).
inline int gui_view(const GameObject &o) {
	const GameObject *r = &o;
	while (r->parent) r = r->parent;
	return r->view;
}
// DE_CActor::IsPositionStatic (Core 0x700917c0): the nearest set position mode up the tree is 1.
inline bool position_static(const GameObject &o) {
	for (const GameObject *p = &o; p; p = p->parent) {
		if (p->is_actor && p->position_mode != 0) return p->position_mode == 1;
	}
	return false;
}

// One running script brain (DE_CClassExecutorInstance) attached to an object.
struct Brain {
	vm::ScriptClass cls;
	vm::Instance inst;
	std::unique_ptr<vm::Executor> exec;
	GameObject *owner = nullptr;
	std::map<std::string, uint32_t> sequence_handles;  // "seqName" -> handle
	std::map<uint32_t, uint32_t> final_instances;      // op 61: persistent class instances by index
	std::vector<uint32_t> behaviors;                   // visible behaviors' global IDs (World::behavior_id)
	size_t attached = 0;                               // behaviors[0..attached) are the class's own
	std::vector<std::string> behavior_names;
	bool started = false;  // its entry has been run up to its first wait (activation)
	bool initial_tick = true;  // ClassExecutor +0x48: its entry runs as the scene adds it
	// Its ClassExecutor's interval (+0x44, ms): a temporal of that rate (DE_CClassExecutorInstance::
	// Setup), 0 none. A creature's behaviour executor is ticked by the creature instead.
	uint32_t interval = 0;
	bool temporal_registered = false;
	float tick_debt = 0;   // scene ms gathered towards its creature's next update
	std::vector<de::ObjectRef> globals;  // op 8 global entries, over the visible behaviors

	// Native -> script call of the most-derived method named like "Entry()".
	bool call(World &w, const std::string &name, std::vector<uint8_t> &args);
	// Call through a method pointer (op 38 handle), dispatched virtually against the running
	// state like op 36, so state-scoped overrides (e.g. Ground.AI.*.ExecutorMoveTo) win.
	// `args` holds just the parameters.
	bool call_handle(World &w, uint32_t handle, std::vector<uint8_t> &args);
};

struct NativeContext {
	World &world;
	vm::Executor &ex;
	uint32_t args;
	const std::vector<std::pair<std::string, uint32_t>> &params;  // type, offset

	vm::Memory &mem() const;
	GameObject *self() const;
	uint32_t self_handle() const;
	// The object whose brain is running this call (the fighter behind a component's native).
	GameObject *owner() const;
	// The controller state this call's owner sees: the keyboard for the player, neutral otherwise.
	const struct Input &input() const;
	template <typename T>
	T arg(size_t i) const;
	GameObject *arg_object(size_t i) const;
	std::string arg_string(size_t i) const;
	void ret_u32(uint32_t v) const;
	void ret_f32(float v) const;
	void ret_bool(bool v) const { ret_u32(v ? 1 : 0); }
	void ret_object(const GameObject *o) const { ret_u32(o ? o->handle : 0); }
};

using Native = std::function<void(const NativeContext &)>;


// JoystickInputSequenceGroup (Creature 0x15006790): sequences of 14-byte requirements
// {u16 button value, u16 button mask, u16 direction mask (9 bits: up, up-right, right, down-right,
// down, down-left, left, up-left, neutral), u16 min age, u16 min gap, u16 max age, u16 max gap}
// (0 max = unbounded), matched last-to-first against the input history newest-first
// (DE_CJoystickInputSequence::Verify 0x15006a90). Age = frames since the matching record began;
// gap = age of the record just newer than it (i.e. how long ago the matching record ended).
struct InputRequirement {
	uint16_t value, mask, directions, min_age, min_gap, max_age, max_gap;
};
using InputSequence = std::vector<InputRequirement>;
std::map<std::string, InputSequence> read_input_bank(const de::ObjectsFile &f, const de::ObjectInfo &o);

struct InputRecord {
	uint16_t buttons, directions;
	uint32_t start;  // frame this input state began
};

class World : public vm::Host {
public:
	void physics_step(GameObject &o);
	Brain *brain_of(vm::Executor &ex) const;
	// The animation bank a script's sequence references index: its creature's own, else the
	// bank among its behavior's globals (BWings' WingAnimationBank).
	const de::AnimBank *script_bank(vm::Executor &ex);
	// DE_CAnimationSequenceManager on its own actors (Core Activate 0x701c0070, SetTime
	// 0x701bfdd0, ActivatePrelude 0x701bff50).
	void manager_activate(GameObject &mgr, const de::AnimBank *bank, int sequence, bool prelude, float t);
	void manager_set_time(GameObject &mgr, float t);
	// A stable global ID for a Behavior object (BGlobalID; DE_CPCode::GlobalID).
	uint32_t behavior_id(const de::ObjectRef &behavior);
	std::vector<de::ObjectRef> referenced_behaviors(const de::ObjectRef &behavior) const;
	// A behavior's global entries (the objects its source names, e.g. BFighter's DangerSphere).
	std::vector<de::ObjectRef> behavior_globals(const de::ObjectRef &behavior) const;
	GameObject *global_object(const de::ObjectRef &r);
	Input input;
	std::map<std::string, InputSequence> standard_inputs;  // kind 26 (Fighter.DEObjects)
	std::map<std::string, InputSequence> fighter_inputs;   // kind 24 (the fighter's own bank)
	std::vector<InputRecord> input_history;
	bool verify(const InputSequence &seq) const;
	// Controller orientation as DE_CJoystickInputManager::Get2DOrientation reports it:
	// 0 up, 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left, 6 left, 7 up-left, 8 neutral.
	int orientation() const;
	std::map<uint32_t, const InputSequence *> input_handles;
	// Animation banks named as behavior globals (World::global_reference), by bank object.
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, std::unique_ptr<de::AnimBank>> global_banks_;
	explicit World(const std::filesystem::path &data_dir);

	vm::Memory &memory() { return mem_; }
	const de::World &data() const { return data_; }

	// Instantiate every object of a data file (and resolve its actor components).
	GameObject *instantiate_file(const std::string &stem);
	GameObject *object(uint32_t handle) const;
	// A runtime-only object (scene, session, pilot stand-ins, ...), created once per name.
	GameObject *create(const std::string &name, const std::string &class_path);
	// A new object (never shared by name) parented under `parent`.
	GameObject *create_child(GameObject *parent, const std::string &name, const std::string &class_path);
	// DE_COMF2Fighter::NumEnemies/Enemy and GlobalInterest (natives_ai.cpp).
	std::vector<GameObject *> enemies_of(GameObject &fighter);
	// DE_COMF2Fighter::IsEnemyFighter (OMF 0x3502a740): not itself, nor on its team.
	static bool enemy_fighter(const GameObject &f, const GameObject &other) {
		return &other != &f && !(other.team >= 0 && other.team == f.team);
	}
	// DE_CFaction::TeamIndex (OMF 0x35052130): its INT value "TeamIndex".
	int32_t faction_team(uint32_t faction);
	float global_interest_of(GameObject *o);
	// DE_COMF2Fighter::CurrentTarget (OMF 0x3502a8b0): the fighter it is facing up to.
	GameObject *current_target_of(GameObject &f);
	// CObject::NewCopy: a deep copy of `o` and its children (no brains), unparented.
	GameObject *copy_tree(const GameObject &o);
	// Set every bone actor under a fighter from its current animation pose.
	void pose_skeleton(GameObject &fighter);
	// DE_COMF2Fighter::ValidateAnimationTime: when the pose is stale, pose the skeleton and turn
	// the Pelvis by the accumulated body rotation, which is then reset.
	void validate_animation(GameObject &fighter);
	// Every channel's model-space transform as the bone actors stand now (the validated pose with
	// whatever scripts moved since: follows, the body rotation), for drawing.
	std::vector<de::Transform> current_pose(GameObject &fighter);
	GameObject *object_for(const de::ObjectsFile *f, uint32_t index) const;
	GameObject &make_object(const de::ObjectsFile *f, uint32_t index);
	uint32_t string(const std::string &s);
	std::string read_string(uint32_t addr);

	// Load the compiled class `class_object` and start it on `owner` at `entry`.
	Brain *attach_brain(GameObject *owner, const de::ObjectRef &class_object, const std::string &entry);
	// One frame, DE_CScene::PassTime (Core 0x70070040): the scene clock (real milliseconds)
	// advances by `ms`. On the way every temporal set (one per update rate) is advanced in time
	// order, at its own tick times, each ticking its elements newest first: creatures, executors
	// with an interval, collision testers, the input manager. Then, once, the delta-tick hooks
	// with the frame's time, the session's update and the effects.
	void tick(uint32_t ms = kFrameMs);
	uint32_t frame() const { return frame_; }
	uint32_t frame_ms() const { return frame_ms_; }
	// DE_CWindow::Run (Core) runs a frame once at least 20 ms have passed: 50 a second at most.
	static constexpr uint32_t kFrameMs = 20;
	// A fighter's update rate (SceneTicksPerUpdate, +0x1dc): 62.5 updates a second.
	static constexpr float kTickMs = 16;
	float scene_ms() const { return float(scene_ms_); }
	// DE_CTemporalMasterScene (TemporalMoveToScale / TemporalPassTime, Core 0x700788b0 / 0x70078bb0):
	// real time runs the scene at `time_scale`; passed time is added on the next frame, and a
	// negative amount moves the clock back.
	float time_scale = 1;
	double passed_ms = 0;
	// The scene ms this frame passes for `real_ms` of real time (taking back time first).
	uint32_t scene_step(float real_ms);
	// One creature tick with the scene clock at `at_ms` (DE_CCreature::SetTime's re-ticks).
	void retick(GameObject &o, uint32_t at_ms);
	// Runs `f` with the scene clock at `at_ms` (DE_CScene +0x1ac / +0x1b0).
	void at_scene_ms(uint32_t at_ms, const std::function<void()> &f);
	// DE_CScene +0x158: re-simulating (TemporalUpdate's deferred SetTimes), so creature ticks leave
	// their command managers alone.
	bool temporal_updating = false;
	// The scene's delta-tick list (+0x70): creatures with no tick rate whose +0x1e0 is set.
	std::vector<uint32_t> delta_creatures;
	// The GUI's own scenes: the top control's (GameObject::view 0) and a GUI scene viewer's (view 1,
	// the model picker's). Neither autopauses (DE_CScene +0x1c0 / +0x1bc), so the app passes them
	// real time: the first every frame, the second while its page is up (the robot's idle event).
	uint32_t gui_scene_ms[2] = {0, 0};
	// Passes `ms` on GUI scene `view`'s clock, ticking its temporal sets on the way.
	void tick_gui(int view, uint32_t ms);
	// The clock of the scene `o` is in.
	uint32_t scene_ms_of(const GameObject *o) const;
	// What Scene() returns for `o` (its actor's +0x10, else the game's): one object per GUI scene,
	// one for the game's (sessions' scenes are told apart by the caller).
	GameObject *scene_object(const GameObject *o);
	// The GUI scene view a Scene() object names, -1 for the game's.
	static int gui_scene_view(const GameObject *scene);
	// The input manager's ticks (DE_CJoystickInputManager, 16 ms): input records age in these.
	uint32_t input_ticks() const { return input_ticks_; }

	void register_native(const std::string &key, Native fn) { natives_[key] = std::move(fn); }
	// Floor height under (x, z) at or below `from_y`; the app wires in arena collision.
	std::function<float(float x, float z, float from_y)> floor = [](float, float, float) { return 0.0f; };
	// Does the segment a-b hit the arena's collision geometry (DE_CScene::TestLineSegment)?
	std::function<bool(const std::array<float, 3> &a, const std::array<float, 3> &b)> segment_blocked =
		[](const std::array<float, 3> &, const std::array<float, 3> &) { return false; };
	const std::map<std::string, uint64_t> &missing() const { return missing_; }
	std::vector<std::string> log;
	// Classes compiled from source (no VMC2), by Class object.
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, vm::VMClass> compiled_classes_;

	// --- Session support (session.cpp) ---
	// Every live session (a single-player tournament's master and the sub-session of its current
	// match), and the current one: the newest begun, whose scene is being played.
	std::vector<std::shared_ptr<struct SessionState>> sessions;
	std::shared_ptr<struct SessionState> session;
	// DE_CSpeaker::GlobalMusicVolume / GlobalSFXVolume (registry Audio\Music Volume, default 0.6,
	// and Audio\SFX Volume, 1.0; the options' Audio panel sets them).
	float music_volume = 0.6f, sfx_volume = 1.0f;
	// Preferences\Renderer\Glow Enabled / Lighting Enabled (the options' Video panel).
	bool glow_enabled = true, lighting_enabled = true;
	int shadow_type = 0;  // the applied Preferences\Renderer\Shadow Type
	// The applied Bump Mapping Enabled (options +8) and Texture Mip Map, the panel's Texture Detail
	// (options +0x1c: 0 High, 1 Medium, 2 Low), as the renderer holds them (DE_CRenderer::ApplyOptions).
	bool bump_enabled = true;
	int texture_detail = 1;
	// The mouse over the GUI (CObject::MouseScreenPositionX/Y: fractions of its width and height,
	// y up) and the screen's aspect ratio (ScreenAspectRatio, width / height); the app sets them.
	float mouse_x = 0.5f, mouse_y = 0.5f, screen_aspect = 4.0f / 3.0f;
	// The game's registry values (DE_CRegistry: AI Difficulty, SinglePlayerPlayed, unlocks, the
	// main menu's last choice, ...); the app loads and saves them.
	std::map<std::string, std::string> registry;
	std::string registry_string(const std::string &name) const {
		auto it = registry.find(name);
		return it == registry.end() ? std::string() : it->second;
	}
	// AddEventRelay(exThis(), Method(CEvent)): per object, the brains and methods events go to.
	std::map<const GameObject *, std::vector<std::pair<Brain *, uint32_t>>> relays;
	void send_event(GameObject *target, uint32_t event);
	// Start the ClassExecutor objects among `properties` on `o` (each runs its entry up to its
	// first wait, as DE_CClassExecutor::AssureActivated).
	std::vector<Brain *> activate_executors(GameObject *o, const std::vector<de::ObjectRef> &properties, bool run = true);
	// CActor::AddProperty of a ClassExecutor: it becomes one of the actor's properties, run when the
	// actor is prepared into the scene, or at once if it already was.
	void add_executor_property(GameObject *o, const de::ObjectRef &executor);
	// DE_CPassiveObjectReference::ObjPrepare: the copy a create-on-prepare reference makes.
	GameObject *create_object_reference(GameObject *reference);
	// DE_CAffector::OwnerActor (+0x118: set at run time, else from the data; the affector itself
	// without one -- a fighter's own affectors fall back to the fighter).
	GameObject *affector_owner(GameObject *a);
	// DE_CCreature::SceneTicksPerUpdate of the creature an object belongs to (0: every tick).
	uint32_t creature_rate(GameObject *o);
	// DE_CPassiveActorReference::Create: the reference's target, made and placed as the scene
	// would (nullptr if it names nothing).
	GameObject *create_from_reference(GameObject *reference);
	// Run an attached brain's entry up to its first wait.
	void start_brain(Brain *b);
	// Put a branch in the scene, starting the executors of its actors (arena scripts, spawn
	// points, ...) that are not running yet: those whose ClassExecutor has the initial tick set
	// (+0x48, contents v4+, the default) at once, the others (Jaguar's lasers, the arenas' own
	// executors) at the end of the tick, after the calling script has set the actor up.
	void enter_scene(GameObject *root);
	void start_pending_brains();
	// ObjNumBrainInstance / ObjBrainInstance, the brains Interface() searches, in order: an
	// object's own executors; a session's, then each modifier's (DE_CStandardSession 0x4500f610);
	// a slot's slot modifiers' (DE_CStandardSessionSlot 0x4501b130); a creature's, then its
	// components' (DE_CCreature 0x150173b0). Defined in session.cpp.
	std::vector<Brain *> brain_list(GameObject *o);
	// Every brain, in attach order.
	std::vector<Brain *> brains() const {
		std::vector<Brain *> v;
		for (auto &b : brains_) v.push_back(b.get());
		return v;
	}
	// DE_CCreature::SetAsUserControlled(device) (Creature 0x15016ca0, vtable +0x144), as
	// SetupCreatureControl calls it for the user's client: the creature is controlled through
	// `device` (-1: by the computer), each of its controllers bound to it (vtable +0x168).
	void set_user_controlled(GameObject &creature, int32_t device);
	// DE_COMF2Fighter::Tick's attack-range step (natives_ai.cpp).
	void jitter_attack_range(GameObject &fighter);
	GameObject *resolve_passive_reference(GameObject *reference);
	// An actor's properties (DE_CActor +0x20 list): its executors, state manager, ...
	std::vector<de::ObjectRef> actor_properties(const de::ObjectRef &r) const;
	// The same for a live object: its data's list plus what a passive reference gave it.
	std::vector<de::ObjectRef> properties_of(const GameObject &o) const;
	// Builds a fighter for a description's resource (the app's fighter setup); called with the
	// referenced OMF2Fighter actor.
	std::function<GameObject *(const de::ObjectRef &actor)> make_fighter;
	// A slot took a creature (slot index): the app learns who fights.
	std::function<void(GameObject *creature, int slot)> on_creature_owned;
	// A script launched a UI manager (CClient::LaunchRemoteUIManager) that has no preset choice:
	// the front end shows it (its pool and title are the session's UiManager record) and
	// completes it with complete_ui (session.h).
	std::function<void(GameObject *ui)> launch_ui;
	// CRulesDisplay (OMF DE_CRulesDisplay; FINDINGS 127): what the session scripts set, and the
	// front end's screen for it (shown by CRulesDisplay::Begin, closed by CloseUIManagerStack).
	struct RulesDisplay {
		std::string task;              // SetCurrentTask
		GameObject *session = nullptr;  // the session of the scene it was begun with
		bool authorized = true;        // +0x120
		bool prompted = false;         // +0x124: IsAuthorizedToContinue was asked while not authorized
		bool shown = false;
		bool displayed = false;  // IsDisplayedCaller (Core): its page is up and no page transition is busy
	};
	std::map<const GameObject *, RulesDisplay> rules_displays;
	std::function<void(GameObject *rules)> show_rules;
	// BVCRControls' UI manager (NewVCRControls): SetNextUIManager shows the app's VCR panel; it is
	// active locally while the panel is up, and deleting it closes the panel.
	GameObject *vcr_ui = nullptr;
	uint32_t vcr_handle = 0;
	bool vcr_shown = false;
	std::function<void(GameObject *ui)> show_vcr;
	std::function<void()> close_ui_stack;
	int ai_difficulty = 0;  // DE_COMF2Manager::AIDifficulty (registry default 0, Rookie) for pilots the session creates
	struct ObjectValue {
		uint32_t bits = 0;  // BOOL / INT / FLOAT
		std::string text;
		std::vector<uint32_t> objects;
	};
	std::map<const GameObject *, std::map<std::string, ObjectValue>> values;  // Set*Value
	ObjectValue *object_value(GameObject *o, const std::string &name);       // ... or the data's Value\\* child
	std::string instance_class(uint32_t instance) const {
		auto it = instance_classes_.find(instance);
		return it == instance_classes_.end() ? std::string() : it->second;
	}
	// op 44 class handles are interned engine class names; this maps one to a data class path.
	std::string class_path_of_handle(uint32_t handle);
	uint32_t class_handle(vm::Executor &ex, uint32_t index) override;
	// The scene: the arena, the fighters and anything Scene().Add put in; scene_objects() is
	// every object under them.
	std::vector<GameObject *> scene_roots;
	std::vector<GameObject *> scene_objects();
	// The session whose scene (DE_CStandardSession +0x16c, made by Begin) each root is in: set
	// before enter_scene, else the current session's; null is the application's.
	std::map<const GameObject *, GameObject *> root_session;
	GameObject *scene_session_of(const GameObject *o) const;
	bool has_behavior_property(GameObject *o, uint32_t class_handle);
	// CPrimitiveGeneratorMesh primitives (text, quads, status bars), by primitive object: what
	// the HUD and on-screen messages are made of. Positions are mesh space; in a screen-space
	// mesh, fractions of the screen with y down.
	struct Primitive {
		enum Kind { Text, Quad, StatusBar } kind = Text;
		GameObject *mesh = nullptr;
		std::string text;
		float x = 0, y = 0, z = 0, height = 0.05f;  // text
		float rgba[4] = {1, 1, 1, 1};
		bool overlay = false, shadow = false, centered = false, snap_right = false, in_game_font = false;
		int sort = 0;
		GameObject *texture = nullptr;               // an Image object
		float uv[4] = {0, 0, 1, 1};                  // UVAsBox
		float box[4] = {0, 0, 0, 0};                 // PositionAsBox / StatusBar SetPosition (x0 y0 x1 y1)
		bool additive = false;                       // SetBlendAddAlpha (else translucent alpha)
		bool modulate = false;                       // SetBlendModulate: the screen times the primitive
		float value = 0;                             // status bar fill, 0..1
		bool bottom_to_top = false;                  // status bar direction (else left to right)
	};
	std::map<const GameObject *, Primitive> texts;
	std::set<const GameObject *> screen_space_meshes;
	std::map<uint32_t, GameObject *> orientation_owners;
	// DE_CScene::CIncidentRecorder (rate limiting: hit sounds, throw damage, pickup spawns...):
	// incidents of a type within the threshold (ms, default 100) of now count as present.
	struct IncidentRecorder {
		uint32_t threshold = 100, max_age = 300000;
		std::vector<std::pair<uint32_t, uint32_t>> incidents;  // (time, type)
		// AddIncidentIfAbsent: an incident of that type within the threshold of t (inclusive)
		// counts as present; otherwise this one is recorded.
		bool add_if_absent(uint32_t t, uint32_t type) {
			for (auto &[when, k] : incidents) {
				uint32_t d = when < t ? t - when : when - t;
				if (d <= threshold && k == type) return false;
			}
			incidents.push_back({t, type});
			return true;
		}
	};
	std::map<const GameObject *, IncidentRecorder> incident_recorders;  // LocalOrientation() blocks -> actor
	// Sounds the scripts started this frame (DE_CScene::PlayNoiseSphere/PlayNoiseAmbient); the
	// app's mixer takes them: a positional sound is full volume within `hotspot` of its position,
	// fading to nothing at `falloff`.
	struct SoundRequest {
		GameObject *noise = nullptr;
		std::array<float, 3> pos{0, 0, 0};
		float hotspot = 0, falloff = 0, volume = 1, frequency = 1;
		bool positional = true;
	};
	std::vector<SoundRequest> sound_requests;
	GameObject *music_player = nullptr;  // MusicPlayerActor()
	GameObject *music_player_actor();
	// Particle effects (generator instances per drawing actor), stepped every game frame.
	std::shared_ptr<class ParticleSystem> particles;
	// Forces and materials (footfalls, launch pads, mines, rumble), ticked every game frame.
	std::shared_ptr<class ForceSystem> forces;
	// The music player's speakers and segment chain (game/music.h).
	std::shared_ptr<class MusicSystem> music;
	// Simple physics of actors that aren't fighters (pickups, projectiles, force applicators).
	std::shared_ptr<class SimplePhysicsSystem> simple_physics;
	// The observer camera's command executors (observer.h).
	std::shared_ptr<class ObserverSystem> observers;
	std::shared_ptr<class ImpulseRobot> impulses;  // the global impulse robot (g_GlobalImpulses)
	// The renderer's camera actor (CRenderer::SetCamera, through CSession::SetCamera); null until set.
	GameObject *renderer_camera = nullptr;
	GameObject *renderer_camera_actor() const {
		GameObject *c = renderer_camera;
		return c && object(c->handle) == c && in_scene(*c) ? c : nullptr;
	}
	// The relative mouse (x right, y down) gathered while relative_mouse is on: what a mouse-axis
	// controller bound to the user reads (the VCR's Manual Observer turns it on).
	std::array<float, 2> mouse_motion{0, 0};
	bool relative_mouse = false;
	// CGenericEvent::Add lists (DE_CGenericEvent +0x4c/+0x50), by event block.
	std::map<uint32_t, std::vector<uint32_t>> event_actors;
	// DE_CCreature::AssureNeedInfoUpdated: the actors owning a creature's known needs, each with
	// its need's DesireLevel(4, creature) (set by register_need_natives).
	std::function<std::vector<std::pair<GameObject *, float>>(GameObject *)> need_owners;
	// Navigation-link paths for computer creatures' move-to commands.
	std::shared_ptr<class Navigation> navigation;
	// DE_CCreature::Command* -> DE_CCommandManager::ReceiveCommand / Forget.
	void issue_command(GameObject &o, GameObject::Command c);
	void forget_commands(GameObject &o, uint32_t mask);
	// The fighter the camera follows (the user's): its HUD and effects count as rendered.
	const GameObject *focus = nullptr;
	// openbg-fight --watch: the computer drives the user's fighter too (a test mode, not the game's).
	bool all_ai = false;
	// CImage::SetPhase (DE_CImage +0x54) by image data object: an AVI image shows the frame for
	// it (the renderer's animated textures follow this map).
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, float> image_phases;
	// CImage::AddFilter: images given a filter at run time (Mantis's bug images get the fighter's
	// paint filter), by image object.
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, GameObject *> image_filters;
	// Texture managers (and meshes) whose image properties scripts replaced (RemoveAllProperty +
	// AddProperty: Canyon's gun shows its shooter's pilot photo, pilots their mood icon): the image
	// now shown, none after a RemoveAllProperty. `texture_override_version` counts the changes.
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, de::ObjectRef> texture_overrides;
	uint32_t texture_override_version = 0;
	// Scene().RegisterRenderTickHook: called every rendered frame with the time passed (ms).
	std::vector<std::pair<Brain *, uint32_t>> render_tick_hooks;
	// Scene().RegisterDeltaTickHook: called every scene tick with the time it covers (ms).
	std::vector<std::pair<Brain *, uint32_t>> delta_tick_hooks;
	// The game camera's projection for CRenderer::ConvertFromWorldToScreen (set by the app):
	// world point -> (x, y screen fractions, y up; z depth along the view).
	std::function<std::array<float, 3>(const std::array<float, 3> &)> world_to_screen;
	// DE_CRenderTargetImage::Capture (Core 0x70193570): the image and the camera actor to render
	// it from, for the app to draw after this frame's scene (256 x 256, its +0x7c).
	std::vector<std::pair<uint32_t, uint32_t>> capture_requests;  // handles
	// The nearest point where a segment meets the arena's collision surface (set by the app).
	std::function<std::optional<std::array<float, 3>>(const std::array<float, 3> &, const std::array<float, 3> &)> segment_hit;
	bool render_ticks_per_frame = true;  // false when the app calls render_tick per drawn frame
	// CObject::DeleteThis: unlinks the object (and its branch); handles to it go inactive.
	void destroy(GameObject *o);
	// DE_GLOBAL::AddObjectToClean (DeleteThisLater, Die): destroyed by the next housekeeping,
	// which runs before and after the frame's updates (DE_GLOBAL::UpdateSystem).
	void clean_later(GameObject *o);
	void housekeeping();

	// vm::Host
	void intrinsic(vm::Executor &ex, uint32_t index, uint32_t args) override;
	void message(vm::Executor &ex, const std::string &text) override;
	uint32_t behavior_global_id(vm::Executor &ex, uint32_t index) override;
	bool interface_valid(vm::Executor &ex, uint32_t iface) override;
	uint32_t interface_global_id(vm::Executor &ex, uint32_t iface) override;
	uint32_t interface_owner(vm::Executor &ex, uint32_t iface) override;
	void interface_execute(vm::Executor &ex, uint32_t iface, uint32_t method, uint32_t args, uint32_t result) override;
	// CObject::Interface(BGlobalID): a handle to the brain on `o` that carries the behavior.
	uint32_t interface_for(GameObject *o, uint32_t gid);
	// DE_CPCode::IsInterfaceOnObject by the behavior's name (BCarryableRight, ...).
	bool has_interface_named(GameObject *o, const std::string &behavior);
	uint32_t global_reference(vm::Executor &ex, uint32_t index) override;
	uint32_t global_entry(vm::Executor &ex, uint32_t index) override;
	uint32_t final_instance(vm::Executor &ex, uint32_t index) override;
	uint32_t new_instance(vm::Executor &ex, uint32_t class_index, uint32_t count) override;
	// A native value-class instance, initialised as its constructor would (CAttackInfo defaults).
	uint32_t make_instance(const std::string &class_name, uint32_t count);
	void reset_attack_info(uint32_t block);
	// The scene's affectors in DE_CScene's list order (+0x184), as DE_CAffector::IsTouching walks
	// them: DE_CScene::Add (Core 0x7006ed50) adds an actor's children first, the last first, then
	// the actor itself, so each branch joins in post-order; Remove keeps the others' order.
	std::vector<GameObject *> affectors();
	// Number a branch's affectors that are not yet in the list, in that order.
	void number_affectors(GameObject *root);
	// The affector that encompasses `a` (+0x124): the data's, else AutoEncompase's (Core 0x7007c360).
	GameObject *encompasser_of(GameObject *a);
	// DE_CAffector::IsTouching's walk (Core 0x7007b900, InternalIsTouching 0x7007baa0): the scene's
	// list holds only affectors nothing encompasses; one whose own groups miss but whose
	// sub-affectors' match is tested without a result, and when touched its sub-affectors are
	// walked, the last first. `test(target, record)` reports whether the source touches target.
	void walk_affectors(uint32_t groups, const std::function<bool(GameObject *, bool)> &test);
	// DE_CActor::ObjSetParent (Core 0x7008afb0): a branch parented into the scene from outside it
	// is added there and then (Scene().Add), joining the affector list at its end.
	void parented(GameObject *o, bool was_in_scene);
	// SetActive's Scene().Add of a branch that has just come out of parking under its parent:
	// prepared and numbered as enter_scene does, staying in its tree (not a root).
	void prepare_branch(GameObject *o);
	std::vector<GameObject *> fighters();  // every fighter object
	// A fighter's linked fighters (+0x334) still in the scene, in list order.
	std::vector<GameObject *> linked_fighters(GameObject &f);
	// COMF2Fighter::CurrentTarget: the nearest enemy fighter in front, within 200 units.
	GameObject *best_melee_target(GameObject &o);
	void guide(GameObject &o);  // DE_COMF2Fighter::Guide, once per frame
	void surface_friction(GameObject &o, float material = 1);  // DE_COMF2Fighter::ApplySurfaceFriction
	// Summed danger at `p` from active danger-group affectors (not `ignore_owner`'s own), and
	// optionally the strongest one's CDanger property and level.
	float danger_at(const std::array<float, 3> &p, const GameObject *ignore_owner, GameObject **greatest, float *greatest_level);

private:
	struct Bound {
		const Native *fn = nullptr;
		std::string key;
		std::vector<std::pair<std::string, uint32_t>> params;
	};
	const Bound &bind(const vm::ScriptClass &cls, uint32_t index);

	de::World data_;
	vm::Memory mem_;
	std::deque<GameObject> objects_;
	std::unordered_map<uint32_t, GameObject *> by_handle_;
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, GameObject *> by_source_;
	std::map<std::string, uint32_t> strings_;
	std::map<std::string, Native> natives_;
	std::map<std::pair<const vm::ScriptClass *, uint32_t>, Bound> bound_;
	std::map<std::string, uint64_t> missing_;
	std::unordered_map<uint32_t, std::string> instance_classes_;
	std::set<const GameObject *> activated_;  // objects whose executors were started
	std::vector<Brain *> pending_starts_;      // attached by a deferred enter_scene, not yet started
	std::deque<std::unique_ptr<Brain>> brains_;
	std::map<const vm::ScriptClass *, std::map<uint32_t, int>> ref_kinds_;
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, uint32_t> behavior_ids_;
	struct Interface {
		Brain *brain;
		uint32_t behavior;  // index into the brain's attached behaviors
		uint32_t gid;
	};
	std::map<uint32_t, Interface> interfaces_;                         // handle -> target
	std::map<std::pair<const Brain *, uint32_t>, uint32_t> interface_handles_;  // kind -> bank (global_reference)
	std::deque<de::AnimBank> banks_;
	std::vector<uint32_t> to_clean_;  // handles
	uint32_t frame_ = 0, frame_ms_ = kFrameMs, scene_ms_ = 0, input_ticks_ = 0;
	double scene_carry_ = 0;
	bool input_registered_ = false;
	uint64_t affector_seq_ = 0;
	// DE_CScene's temporal sets (TemporalElementSetForRate 0x70072230), in creation order.
	struct Temporal {
		enum Kind { Creature, Brain, Tester, Input } kind;
		uint32_t handle = 0;      // creature object / collision tester
		struct Brain *brain = nullptr;
	};
	struct TemporalSet {
		uint32_t rate = 0, next = 0;  // next tick: last tick (+0x38) + rate
		int scene = 0;                // 0 the game's, else GUI scene view + 1
		std::vector<Temporal> elements;
	};
	std::vector<TemporalSet> temporal_sets_;
	TemporalSet &temporal_set(uint32_t rate, int scene);
	int temporal_scene(const Temporal &t);
	void add_temporal(uint32_t rate, Temporal t);
	void register_temporals();
	void register_creature(GameObject *o);
	void register_branch(GameObject *o);
public:
	void register_brain(Brain *b);
private:
	void advance(TemporalSet &set);
	void reset_clock();
	void creature_tick(GameObject &o);
	void input_tick(uint32_t manager);

	friend void register_core_natives(World &);
	friend void register_ai_natives(World &);
	friend void register_combat_natives(World &);
	friend void register_session_natives(World &);
	friend void register_script_natives(World &);
	void run_commands(GameObject &o);

public:
	// CCollisionTester::Activate (Core): the tester joins the 16 ms temporal set.
	void activate_tester(uint32_t handle) { add_temporal(16, {Temporal::Tester, handle, nullptr}); }
};

void register_core_natives(World &w);
// Engine class name without "DE_C" (e.g. "OMF2Fighter") -> data class path.
const std::map<std::string, std::string> &class_paths();
void register_ai_natives(World &w);
void register_combat_natives(World &w);
void register_transform_natives(World &w);
void register_particle_natives(World &w);
void register_force_natives(World &w);
void register_grapple_natives(World &w);
void register_creature_natives(World &w);
void register_need_natives(World &w);
void register_music_natives(World &w);
// The pilot behind a pilot object or a fighter (the fighter's own, else a child pilot's).
const Pilot *pilot_of(GameObject *o);
// The copy of data object `r` nearest `from` (its own subtree, then each ancestor's).
GameObject *nearest_copy(GameObject *from, const de::ObjectRef &r);
void register_simple_physics_natives(World &w);
// A CData's bytes (DE_CDataBlock), by its block in script memory.
std::vector<uint8_t> &cdata_bytes(uint32_t block);
// Active collision testers (natives_combat.cpp), once per tick.
void tick_collision_tester(World &w, uint32_t handle);
// MSVCRT rand() on the main thread: one stream that Core, Creature, OMF and Particles all draw
// from (Core MATH::Rand / CenterRand stand on it); DE_GLOBAL::SystemInit seeds it with the time.
int crt_rand();
void crt_srand(uint32_t seed);
// MATH::Rand(1): rand() / 32768.
float random01(World &w);

// DE_CQuaternion::SphericalLinearInterpolate between two rotations (rows), t of the way.
// DE_CCoordinateSystem::RotateX/Y/Z (radians) on rotation rows.
void rotate_rows(std::array<float, 9> &r, int axis, float a);
// Turn a fighter's body rotation for `frame` (see GameObject::anim_rot).
void anim_rotate(GameObject &o, int axis, float a);
std::array<float, 9> slerp_rotation(const std::array<float, 9> &a, const std::array<float, 9> &b, float t);

// CScene::Time / PreciseTime: the clock of the scene `this` names, else of the caller's.
inline uint32_t scene_time(const NativeContext &c) {
	const GameObject *s = c.self();
	int v = World::gui_scene_view(s);
	if (v >= 0) return c.world.gui_scene_ms[v];
	return c.world.scene_ms_of(s && !s->file && s->name == "Scene" ? nullptr : c.owner());
}

// CVector blocks in VM memory (3 floats); null reads as zero, null writes are ignored.
std::array<float, 3> read_vec(vm::Memory &m, uint32_t a);
void write_vec(vm::Memory &m, uint32_t a, const std::array<float, 3> &v);

}  // namespace game
