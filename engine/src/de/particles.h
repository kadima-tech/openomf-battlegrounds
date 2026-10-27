// Particle data (Core DE_CParticleGenerator 0x701d5540, DE_CParticleModifier 0x701d7970,
// DE_CManualParticleSpawner 0x701d3cb0, DE_CParticleRenderer 0x701c2e60; Particles module
// spawners, modifiers and DE_CBillboardedQuadParticleRenderer 0x40010160).
//
// An effect is an actor whose mesh is a particle renderer. The renderer references the images it
// draws with and a ParticleGenerator; the generator lists its modifiers (spawners and per-particle
// modifiers) in order. Each class writes one contents chunk and one references chunk, base first.
//  - ParticleGenerator v4: [name][int local][BOOL orientation, color, momentum, channel4,
//    rotation, channel3 channels][BOOL reset per instance]. References v1: BOOL-terminated
//    modifier list.
//  - ParticleModifier v4: [name][spawn time, variation][life span, variation][spawn size,
//    variation][BOOL spawning][BOOL looping][int initial count +0x54][int interpolate +0x58]
//    [int extra per spawn +0x50]. Subclass chunks follow (see ParticleModifierDef).
//  - ParticleRenderer: [mesh chunk][v2: int blend][subclass chunk]. References: [object]
//    [mesh: BOOL-terminated images][v1: BOOL + generator][subclass].
#pragma once

#include <array>
#include <string>
#include <vector>

#include "de/scene.h"
#include "de/value.h"

namespace de {

// DE_CParticleGenerator channel indices (Setup 0x701d5b00): per-particle data blocks after the
// 0x34-byte particle header.
enum ParticleChannel { kChMomentum, kChOrientation, kChColor, kCh3, kChSize, kChRotation, kNumChannels };

struct ParticleModifierDef {
	ObjectRef object;
	std::string kind;  // class path after "ParticleModifier\"
	// DE_CParticleModifier (+0x2c..+0x58).
	float spawn_time = 0, spawn_time_variation = 0;
	float life_span = 0, life_span_variation = 0;
	float spawn_size = 0, spawn_size_variation = 0;
	bool spawning = false, looping = false;
	int32_t initial_count = 0, interpolate = 0, extra_per_spawn = 0;

	// RandomEllipsoidSpawner v1/v2 and RandomBoxSpawner v1-v3: a box (min xyz, max xyz); the box
	// spawner wraps particles inside it on the axes flagged in v3.
	std::array<float, 6> box{};
	std::array<bool, 3> wrap{};
	// Gravity / Wind v1 (+0x60): strength; wind response time.
	float strength = 0;
	// MiscellaneousParticleFunctions v1: a FLOAT curve over particle age and what it drives
	// (0 = size, 1 = rotation channel).
	FloatValue curve;
	uint32_t misc_mode = 0;
	// NewParticleModifier v1: initial rotation / orientation / momentum / colour modes, momentum
	// scale (+0x74), inherited actor momentum (+0x7c, capped at +0x80, scaled by +0x84), add to
	// the existing momentum instead of replacing it (+0x6c).
	int32_t rotation_mode = 0, orientation_mode = 0, momentum_mode = 0, color_mode = 0;
	float momentum_scale = 0, f78 = 0;
	bool inherit_momentum = false, add_momentum = false;
	float inherit_cap = 10, inherit_scale = 0;
	// MomentumAndAcceleration v4: actor-momentum inheritance per axis (+0x60), random (+0x6c,
	// centred) and fixed (+0x78) initial momentum, acceleration mode (+0x84) and amount (+0x88),
	// angle (+0x8c, mode 3).
	std::array<float, 3> inherit_axes{1, 1, 1}, random_momentum{}, initial_momentum{};
	int32_t accel_mode = 0;
	float accel = 0, accel_angle = 0;
	// ManualParticleSpawner v1: copy the spawn orientation into the orientation channel.
	bool manual_orientation = false;
	// MeshSpawner v3: burst count at Begin (+0x6c); vertex order (+0x70: 0 random, 1 cycling,
	// 2 cycling backwards, 3 once through); the mesh whose vertices it spawns at.
	int32_t mesh_burst = 0, mesh_order = 0;
	ObjectRef mesh;
	std::vector<std::array<float, 3>> mesh_vertices;
	// KillWithParticles: the objects deleted once its instance has no particles left, at least
	// 5 s after it began (DE_CKillWithParticles::Modify).
	std::vector<ObjectRef> kill;

	bool is_spawner() const;
};

struct ParticleGeneratorDef {
	ObjectRef object;
	bool local = false;  // +0x4c: particles live in the actor's space (else world space)
	std::array<bool, kNumChannels> channels{};
	bool reset_per_instance = false;  // +0x98
	std::vector<ParticleModifierDef> modifiers;
};

struct ParticleRendererDef {
	ObjectRef object;
	std::string kind;  // class path after "Mesh\ParticleRenderer\"
	int32_t blend = 0;  // +0x54: 0 and 1 add (colour premultiplied by alpha), 2 alpha-blends
	ObjectRef image;
	ParticleGeneratorDef generator;
	// BillboardedQuad v2-v7.
	ColorValue color;
	ObjectRef color_ref;  // the colour's Value\RGBA (scripts set it at run time)
	float depth_offset = 0;   // +0x6c
	int32_t mode = 0;         // +0x88: 0 = screen-aligned square, others oriented/stretched
	int32_t uv_mode = 0;      // +0x8c: 0 whole image, 1 4x4 flipbook by age, 2 frame by index
	int32_t color_mode = 0;   // +0x90: 0 constant, 1/2 colour x age / 1-age, 3 x size channel,
	                          // 4/5 alpha x age / 1-age, 7 colour value at age, 8 colour channel
	float width = 1, height = 1;   // +0x70 / +0x74
	int32_t normalize = 0;         // +0xac
	std::array<float, 3> offset{};  // +0x78..+0x80
	int32_t cone_mode = 0;         // +0x94
	FloatValue glow;               // +0x98
	float glow_factor = 0;         // a "GlowFactor" child: drawn into the glow layer (DE_CShader::GetShader)
	float f84 = 0.1f;
	// Ring v1 (DE_CRingParticleRenderer 0x400158d0): an annulus per particle; inner and outer
	// colours (+0x6c / +0x58), plane (+0xe0: 0 the particle's / actor's frame, 1 and 2 turned 90
	// degrees about X / Y, 3 facing the camera), detail (+0x80), and curves over the particle's age
	// (x size) for the inner and outer edges' heights (+0x84 / +0x98) and radii (+0xac / +0xc0).
	ColorValue inner_color, outer_color;
	int32_t ring_plane = 0, ring_dc = 0, ring_d4 = 0, ring_d8 = 0;
	float ring_detail = 1;
	FloatValue inner_height, outer_height, inner_radius, outer_radius;
};

bool is_particle_renderer(const ObjectRef &r);
ParticleRendererDef read_particle_renderer(const World &w, const ObjectRef &r);
// The first image a mesh object references (DE_CMesh's references chunk: its materials/images).
ObjectRef read_mesh_image(const World &w, const ObjectRef &r);

}  // namespace de
