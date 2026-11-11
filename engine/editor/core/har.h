// A HAR variant: a copy of a base robot's fighter file (the OMF2Fighter tree, its scripts,
// animations, input sequences and spheres) with a new Description\Creature\Fighter and
// FighterModelDescription, all with fresh GUIDs, edited as data.
#pragma once

#include <string>
#include <vector>

#include "addon.h"

namespace ed {

struct HarStat {
	std::string name;
	float value = 0;
	bool operator==(const HarStat &) const = default;
};

struct HarSphere {
	int object = -1;  // add-on object index (an Actor\Affector\Sphere)
	std::string name, bone;
	float radius = 0;
	bool operator==(const HarSphere &) const = default;
};

struct HarInput {
	std::string name;
	std::vector<uint32_t> steps;  // joystick state masks, one per step
	float window = 0;
	bool operator==(const HarInput &) const = default;
};

struct HarData {
	std::string name, bio;
	bool listed = true, locked = false;
	std::vector<HarStat> stats;
	std::vector<HarSphere> spheres;
	std::vector<HarInput> inputs;
	bool operator==(const HarData &) const = default;
};

struct HarObjects {
	int fighter = -1, description = -1, model_description = -1, stats = -1, inputs = -1, behavior = -1;
};

std::vector<std::string> base_fighters(const BaseData &base);
std::vector<uint32_t> addon_hars(const Project &p);
HarObjects har_objects(const Project &p, uint32_t description);
uint32_t create_har_variant(Project &p, const std::string &base_fighter, const std::string &name);
HarData read_har(const Project &p, const HarObjects &o);
void write_har(Project &p, const HarObjects &o, const HarData &d);

}  // namespace ed
