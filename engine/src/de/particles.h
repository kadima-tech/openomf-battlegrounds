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
	// Rotator v1 (Particles 0x40008f10): mode (+0x60: 1 rotation += curve(age) x factor x dt,
	// 3 the same x size, 4 rotation = curve(age) + factor), `curve` (+0x64), factor (+0x78).
	// MiscellaneousNewParticleFunctions v1 (0x40004750): `curve` (+0x64) and the axis new
	// particles' orientation turns about (`misc_mode`, +0x60: 0 X, 1 Y, 2 Z).
	int32_t rotator_mode = 0;
	float rotator_factor = 0;
	// PushDownZModifier (0x40008730): v2 amount (+0x60, default 20) and mode (+0x64; v1: 5): moves
	// particles along the actor's X/Y/Z (modes 0-2, and 3-5 scaled by the life left).
	float push_amount = 20;
	int32_t push_mode = 0;
	// BeamSpawner (0x4000a1c0): length (+0x60), spacing and its variation (+0x68, +0x6c), v1/v2
	// the life span and variation (base +0x34/+0x38), +0x78, v2/v3 ring radius and variation
	// (+0x70, +0x74) and start distance (+100).
	float beam_length = 0, beam_spacing = 0, beam_spacing_variation = 0;
	float beam_radius = 0, beam_radius_variation = 0, beam_start = 0;
	int32_t beam_78 = 0;
	// LighteningSpawner (0x4000b7a0) v1/v2: jitter (+0x74, x a segment's length, default 0.2),
	// +0x78..+0x84, +0x8c, +0x90, +0x94, the shortest segment split (+0x88, default 400), taper
	// (+0x98: point size falls along the bolt), v2 follow (+0x9c: bolts move with their target).
	// References v1: the target actors (+0x68), one bolt to each.
	float bolt_jitter = 0.2f, bolt_min_segment = 400;
	std::array<float, 4> bolt_78{};
	int32_t bolt_8c = 0, bolt_94 = 0;
	float bolt_90 = 0;
	bool bolt_taper = false, bolt_follow = false;
	std::vector<ObjectRef> bolt_targets;
	// AutoExternalSpawner (0x40001300) v1: every new particle whose serial is a multiple of +0x6c,
	// or one in +0x70 at random, has the external manual spawner (references v1, +100) spawn one
	// at it, of size x +0x74 and life x +0x78.
	int32_t auto_every = 0, auto_random = 0;
	float auto_size = 1, auto_life = 1;
	ObjectRef auto_spawner;

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
	// StreamParticleRenderer v1 (DE_CStreamParticleRenderer 0x40017050): a ribbon through the
	// particles in order. `color` (+0x58), colour mode (+0x70: 0/3 constant, 1 x age, 2 x 1 - age,
	// 4 the value at age, 5 x the intensity channel), texture mode (+0x74: 0 whole image per
	// segment, 2 a 4x4 frame by serial, 4 along the stream: v = age x +0x78 + +0x7c), width
	// (+0x6c, x the particle's size), +0x80 breaks the ribbon at particles without flag 2 (or,
	// with +0x84, where consecutive particles are more than +0x84 ms apart).
	// FourFaceTriangle v1 (DE_CFourFaceTriangleParticleRenderer 0x40014320): per particle a
	// triangle (+0x6c, +0x78, +0x84, each corner plus up to +0x90..+0xb0, x size) drawn as three
	// faces round a centre vertex at the particle (the colour at its age; the corners fade to
	// black / transparent), its texture a planar projection (+0xcc: 0 XY, 1 XZ, 2 YZ) scaled by
	// +0xc0 at a random offset; +200 0 in the particle's frame, 1-3 turned to face the camera.
	std::array<float, 9> tri_corners{};
	std::array<float, 9> tri_jitter{};
	std::array<float, 4> tri_b4{0, 0, 1, 1};
	int32_t tri_facing = 0, tri_uv_plane = 1;
	int32_t stream_color_mode = 0, stream_uv_mode = 0;
	float stream_width = 1, stream_uv_scale = 1, stream_uv_offset = 0, stream_max_gap = 0;
	bool stream_breaks = false;
	ColorValue inner_color, outer_color;
	int32_t ring_plane = 0, ring_dc = 0, ring_d4 = 0, ring_d8 = 0;
	float ring_detail = 1;
	FloatValue inner_height, outer_height, inner_radius, outer_radius;
};

// Mesh\PointStreamer (Core DE_CPointStreamer 0x701d1520): a trail behind an actor. Contents after
// the mesh chunk, v2: [interval +0x50][life +0x54][start width +0x58][end width +0x5c][start
// alpha +100][end alpha +0x68][texture length +0x70][+0x7c, cleared]; v3 adds [mode +0x80]
// [rise +0x78]. Defaults 50 ms, 2000 ms, 0.4, 2.5, 1, 0, 2000, mode 2, rise 0.006.
struct PointStreamerDef {
	ObjectRef object, image;
	float interval = 50, life = 2000, width0 = 0.4f, width1 = 2.5f, alpha0 = 1, alpha1 = 0, texture_length = 2000, rise = 0.006f;
	int32_t mode = 2;
};
bool is_point_streamer(const ObjectRef &r);
PointStreamerDef read_point_streamer(const World &w, const ObjectRef &r);

bool is_particle_renderer(const ObjectRef &r);
ParticleRendererDef read_particle_renderer(const World &w, const ObjectRef &r);
// The first image a mesh object references (DE_CMesh's references chunk: its materials/images).
ObjectRef read_mesh_image(const World &w, const ObjectRef &r);

}  // namespace de
