// A HAR variant: a copy of a base robot's fighter file (the OMF2Fighter tree, its scripts,
// animations, input sequences, spheres and sounds) with a new Description\Creature\Fighter and
// FighterModelDescription, all with fresh GUIDs, edited as data.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "addon.h"

namespace ed {

// The fighter's movement chunk (DE_COMF2Fighter, the 30 floats read_fighter_stats reads).
struct HarStatInfo {
	const char *name, *group;
	bool derived;  // recomputed by the game from another value
};
const std::array<HarStatInfo, 30> &har_stat_infos();

struct HarSphere {
	int object = -1;  // an add-on Actor\Affector\Sphere
	std::string name, bone;
	float hotspot = 0, falloff = 0;
	bool operator==(const HarSphere &) const = default;
};

// DE_CJoystickInputSequence's requirement (game::InputRequirement).
struct HarStep {
	uint16_t value = 0, mask = 0, directions = 0, min_age = 0, min_gap = 0, max_age = 0, max_gap = 0;
	bool operator==(const HarStep &) const = default;
};
struct HarInput {
	std::string name;
	std::vector<HarStep> steps;
	bool operator==(const HarInput &) const = default;
};

struct HarSound {
	int object = -1;  // an add-on Noise2\Stream
	std::string name, source;
	bool operator==(const HarSound &) const = default;
};

// A numeric final of the fighter's behavior (damage and stun multipliers, armour, speeds).
struct HarConstant {
	std::string name, type, comment;
	double value = 0;
	bool operator==(const HarConstant &) const = default;
};

struct HarData {
	std::string name;
	bool listed = true, locked = false;
	std::array<float, 30> stats{};
	std::vector<HarSphere> spheres;
	std::vector<HarInput> inputs;
	std::vector<HarSound> sounds;
	std::vector<HarConstant> constants;
	bool operator==(const HarData &) const = default;
};

struct HarObjects {
	int description = -1, fighter = -1, inputs = -1, behavior = -1, behavior_class = -1;
	std::vector<int> model_descriptions;  // Description\FighterModelDescription naming this fighter
};

std::vector<std::string> base_fighters(const BaseData &base);
std::vector<uint32_t> addon_hars(const Project &p);
HarObjects har_objects(const Project &p, uint32_t description);
// Returns the new Description\Creature\Fighter.
uint32_t create_har_variant(Project &p, const std::string &base_fighter, const std::string &name);
HarData read_har(const Project &p, const HarObjects &o);
// Data only; a changed constant rewrites the behavior's members (compile afterwards).
void write_har(Project &p, const HarObjects &o, const HarData &d);
void import_sound(Project &p, uint32_t noise, const Bytes &wav, const std::string &source);

// A new paint job: a copy of the base model (meshes, materials, the RGBMain / ReflectionMask /
// Bump images) and a FighterModelDescription for it. Returns the description.
uint32_t create_paint_job(Project &p, const HarObjects &o, const std::string &name);
struct PaintImage {
	int object = -1;
	std::string name;
};
// The images of a paint job the add-on holds (empty for one that uses the base model).
std::vector<PaintImage> paint_images(const Project &p, uint32_t model_description);
// The bone (nearest Actor above it) a sphere hangs from.
std::string sphere_bone(const Project &p, int sphere);

}  // namespace ed
