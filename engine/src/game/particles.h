// Particle simulation (Core DE_CParticleGeneratorInstance::SetTime 0x701d5e70 and the modifiers'
// ManageParticles / ManageParticleUserData / Modify passes; Particles module modifiers).
//
// A particle renderer creates one generator instance per actor drawing it (NewInstance, at the
// renderer's ObjPrepare / first Process), and steps it to the scene time whenever it is drawn:
//  1. the instance takes the actor's world placement (world-space generators) and its movement
//     since the last step;
//  2. every modifier manages particles: spawners create them (timed by spawn time ± variation,
//     bursts, manual requests), placing new ones in the actor's frame;
//  3. if any were created, every modifier initialises the new ones (ManageParticleUserData:
//     momentum, orientation, colour, rotation);
//  4. ages advance by dt / life span; expired particles die unless looping; momentum moves them;
//  5. every modifier modifies the live ones (gravity, wind, size curves, wrapping...).
// Particle momentum is in world units per millisecond, like DE_CActor::Momentum.
#pragma once

#include <array>
#include <map>
#include <optional>
#include <memory>
#include <vector>

#include "de/particles.h"
#include "game/runtime.h"

namespace game {

struct Particle {
	std::array<float, 3> pos{}, prev{};  // +0x00, +0x0c (spawn position)
	float age = 0, age_step = 0;          // +0x18 (0..1), +0x1c
	float spawn_fraction = 0;             // +0x20: how far into the step it was born
	float size = 0;                       // +0x24
	int32_t serial = 0;                   // +0x28
	uint32_t flags = 0;                   // +0x2c: 2 relative, 4 alive, 8 new, 0x10 looping
	float inv_life = 0;                   // +0x30
	// Channels.
	std::array<float, 3> momentum{};
	std::array<float, 9> orientation{1, 0, 0, 0, 1, 0, 0, 0, 1};
	std::array<float, 4> color{1, 1, 1, 1};
	float intensity = 1;  // channel 4
	float rotation = 0;   // channel 5 (turns)
	float bolt_t = -1;    // DE_CLighteningSpawner's particle data: how far along its bolt (0..1)
	// A FourFaceTriangle renderer's per-particle triangle (corners x size, in its frame) and the
	// texture coordinates of the corners and the centre, made when the particle is born.
	std::array<float, 9> tri{};
	std::array<float, 8> tri_uv{};
};

// CManualParticleSpawner::CNewParticleInfo.
struct NewParticleInfo {
	std::array<float, 3> pos{};
	float life_span = 0, size = 0;
	std::array<float, 9> orientation{1, 0, 0, 0, 1, 0, 0, 0, 1};
	float intensity = 1, rotation = 0;
	float time = 0;         // scene time it was requested (actor spawns)
	bool relative = false;  // position in the generator actor's frame
};

// Script-side state of one modifier object (DE_CParticleModifier +0x2c..+0x48, +0x44 burst, the
// manual spawner's request list).
struct ModifierControl {
	bool initialised = false;
	float spawn_time = 0, spawn_time_variation = 0, life_span = 0, life_span_variation = 0;
	float spawn_size = 0, spawn_size_variation = 0;
	bool spawning = false;
	int32_t burst = 0;
	float radius = -1;  // CRandomEllipsoidSpawner::SetRadius (+0x84..: all three radii; -1 unset)
	float beam_length = -1, beam_start = -1;  // CBeamSpawner::SetTotalLength (+0x60) / SetInitialSpawnLength (+100)
	std::vector<NewParticleInfo> requests;
};

struct ParticleInstance {
	const de::ParticleRendererDef *def = nullptr;
	GameObject *actor = nullptr;
	std::vector<GameObject *> modifier_objects;  // the actor's copies of the generator's modifiers
	struct PerModifier {
		float countdown = 0;  // time until the next spawn (negative: overdue)
		int32_t serial = 0;
		int32_t initial = 0;
		int32_t mesh_burst = 0;  // DE_CMeshSpawner::Begin: one particle per vertex, once
		float kill_countdown = 5000;  // DE_CKillWithParticles +0x74 (ms; 999999 once it has killed)
		bool beam_done = false;       // DE_CBeamSpawner's instance flag: the beam is laid once
		std::array<float, 3> bolt_target{};  // DE_CLighteningSpawner's instance data: the target's last position
		bool bolt_target_set = false;
	};
	std::vector<PerModifier> per_modifier;
	std::vector<Particle> particles;
	float time = -1, dt = 0;
	bool spawning = true;  // +0x58 (Enable/DisableParticleSpawning)
	bool interpolate_position = false, interpolate_rotation = false;  // +0x5c / +0x60
	bool placed = false;
	std::array<float, 3> actor_pos{}, actor_move{};
	std::array<float, 9> actor_rot{1, 0, 0, 0, 1, 0, 0, 0, 1}, prev_rot{1, 0, 0, 0, 1, 0, 0, 0, 1};
	float size_multiplier = 1;  // +0x64
	int created = 0;
	uint32_t stepped = 0;  // the game frame it was last stepped in (drawn only then)
};

// A fighter's swing trail (OMF DE_CAttackTrailer 0x3503fa90 GeneratePoint, 0x35040250 Process):
// while the attack script calls Continue() every frame, each fighter tick adds a point at the
// emitter actor (offset and half-width vector in its frame) in the trailer's colour; once it stops
// the colour ramps to black over 8 points and points keep coming until it is dark. Each draw ages
// the points by the creature ticks since the last (life -= dt / length); consecutive points (born
// within 2 ticks) are joined by a quad.
struct AttackTrailer {
	GameObject *object = nullptr;   // the script handle (COMF2Fighter::*Trailer())
	GameObject *emitter = nullptr;  // "<Left|Right><Punch|Kick>TrailerEmitter" reference value
	de::ObjectRef image;            // the fighter's "AttackTrailer" property
	std::array<float, 3> offset{}, width{};
	float length = 30;              // +0xd8
	bool reverse_uv = false;        // +0xe4: ReverseUVs, which nothing calls
	uint32_t last = 0;              // +0xdc: the creature time of the last draw
	bool continuing = false;        // +0xe0
	std::array<float, 4> color{0, 0, 0, 0}, step{0, 0, 0, 0}, target{0, 0, 0, 0}, dest{0, 0, 0, 0};
	int steps = 0, dest_steps = 0;  // +0xd0, +0xcc
	struct Point {
		std::array<float, 3> pos, width;
		std::array<float, 4> color;
		float life = 1, decay = 0;
		float v = 0;  // 1 - life before the last draw's ageing
		uint32_t born = 0;
		bool cut = true;  // not joined to the previous point
	};
	std::vector<Point> points;
	void ramp_toward(const std::array<float, 4> &c, int n);
	void generate_point(uint32_t now);
};

// A hit's spark burst (OMF DE_COMF2Fighter::NewGetHitEffect 0x35026710): a copy of the fighter's
// "SparkEffect" whose Mesh\\Sparks shoots its sparks once, on its first draw
// (DE_CSparks::DE_CInstanceInfo::SetupSparks 0x35042f10), which then fly, fall and fade with the
// scene time (DE_CInstanceInfo::Process 0x350433e0). Its DE_CSparkProcess (Tick 0x3502f890) dims
// the light by 0.05 a tick and deletes the copy after 200 ticks.
struct SparkBurst {
	GameObject *actor = nullptr, *light = nullptr;
	de::ObjectRef image;  // the Sparks mesh's first image
	int type = 0;         // DE_CSparks +0x50: 0 a fan along the actor's Z, 1 a spray in its frame
	int count = 0;        // +0x54
	float life = 1, light_intensity = 0;  // process +0x2c, +0x28
	bool setup = false;
	float last_ms = 0;  // instance +0x14
	struct Spark {
		std::array<float, 3> pos{}, vel{};  // world, units per ms
		float width = 0, life = 0, decay = 0;
	};
	std::vector<Spark> sparks;
};

// A Mesh\PointStreamer trail on an actor (DE_CPointStreamer::DE_CInstanceInfo::Process, Core
// 0x701d17f0): while enabled a point every `interval` ms at the actor, each living `life` ms and
// rising by `rise` per ms; drawn from the oldest point to the actor.
struct Trail {
	GameObject *actor = nullptr;
	GameObject *mesh = nullptr;  // the actor's copy of the streamer (Enable/Disable)
	const de::PointStreamerDef *def = nullptr;
	struct Point {
		std::array<float, 3> pos;
		float time = 0, v = 0;
	};
	std::vector<Point> points;
	float next = 0, last = -1;
	bool enabled = true;
	uint32_t stepped = 0;
};

class ParticleSystem {
public:
	explicit ParticleSystem(World &w) : w_(w) {}
	// Step every particle effect in the scene to the current scene time.
	void step();
	const std::vector<std::unique_ptr<ParticleInstance>> &instances() const { return instances_; }
	ModifierControl &control(GameObject *modifier);
	// Enable/DisableParticleSpawning: the instances drawn by this actor.
	void set_spawning(GameObject *actor, bool on);
	// A generator instance's own switch (DE_CParticleGeneratorInstance +0x58), by generator and
	// actor: kept for instances not made yet.
	void set_generator_spawning(const de::ObjectRef &generator, const GameObject *actor, bool on);
	const de::ParticleRendererDef *renderer(const de::ObjectRef &r);
	// A material's particles (DE_CParticleSpawningMaterial::ObjPrepare adds its renderer to the
	// actor's properties): the request list of `spawner` in the actor's instance of `renderer`.
	std::vector<NewParticleInfo> *requests(GameObject *actor, const de::ObjectRef &renderer, const de::ObjectRef &spawner);
	// The fighter's trailers (0 left hand, 1 right hand, 2 left foot, 3 right foot), made on first use.
	AttackTrailer *trailer(GameObject *fighter, int which);
	AttackTrailer *trailer_of(const GameObject *object);
	const std::map<const GameObject *, std::array<AttackTrailer, 4>> &trailers() const { return trailers_; }
	// CLight state set by scripts (intensity, colour), over the light's own data.
	struct LightState {
		std::optional<float> intensity;
		std::optional<std::array<float, 3>> color;
	};
	LightState &light(const GameObject *o) { return lights_[o]; }
	const LightState *find_light(const GameObject *o) const {
		auto it = lights_.find(o);
		return it == lights_.end() ? nullptr : &it->second;
	}
	void add_sparks(SparkBurst b) { sparks_.push_back(std::move(b)); }
	const std::vector<SparkBurst> &sparks() const { return sparks_; }
	const std::vector<std::unique_ptr<Trail>> &trails() const { return trails_; }
	// CPointStreamer::Enable / Disable, on the streamer's object.
	void set_trail_enabled(GameObject *mesh, bool on);
	// DE_COMF2Fighter::Tick: every trailer of the fighter adds its point.
	void tick_trailers(GameObject &fighter);
	// DE_CAttackTrailer::Process (OMF 0x35040250), once per drawn frame: with two points or more
	// each ages by the creature ticks since the last draw; then dead points leave from the front.
	void process_trailers();

	// Value\RGBA objects set at run time (CValueRGBA::Set, and a billboard renderer's SetColor on
	// its colour value +0x58), by value object; null when the data's colour stands. An alpha
	// below 0 keeps the data's.
	const std::array<float, 4> *value_color(const de::ObjectRef &v) const {
		auto it = value_colors_.find({v.file, v.index});
		return !v || it == value_colors_.end() ? nullptr : &it->second;
	}
	void set_value_color(const de::ObjectRef &v, const std::array<float, 4> &rgba) { if (v) value_colors_[{v.file, v.index}] = rgba; }

private:
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, std::array<float, 4>> value_colors_;
	std::vector<const de::ParticleRendererDef *> &renderers_of(GameObject *o);
	ParticleInstance &instance(GameObject *actor, const de::ParticleRendererDef *def);
	void step(ParticleInstance &in, float now);
	World &w_;
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, std::unique_ptr<de::ParticleRendererDef>> defs_;
	std::map<const GameObject *, std::vector<const de::ParticleRendererDef *>> actor_renderers_;
	std::vector<std::unique_ptr<ParticleInstance>> instances_;
	std::map<const GameObject *, ModifierControl> controls_;
	std::map<const GameObject *, bool> actor_spawning_;
	std::map<std::pair<std::pair<const de::ObjectsFile *, uint32_t>, const GameObject *>, bool> generator_spawning_;
	std::map<const GameObject *, std::array<AttackTrailer, 4>> trailers_;
	std::map<const GameObject *, AttackTrailer *> trailer_objects_;
	std::map<const GameObject *, LightState> lights_;
	std::vector<GameObject *> kills_;  // KillWithParticles victims, deleted after the step
	std::vector<SparkBurst> sparks_;
	void step_sparks(float now);
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, std::unique_ptr<de::PointStreamerDef>> streamer_defs_;
	std::map<const GameObject *, std::vector<const de::PointStreamerDef *>> actor_streamers_;
	std::vector<std::unique_ptr<Trail>> trails_;
	std::map<const GameObject *, bool> trail_enabled_;
	void step_trails(float now);
};

// Scene time in milliseconds (the game runs at 60 frames a second).
float scene_time_ms(const World &w);

}  // namespace game
