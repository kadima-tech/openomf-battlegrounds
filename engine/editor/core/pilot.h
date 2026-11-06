// A pilot as the editor sees it: the Actor\Creature\Pilot, its Description\Pilot (with the
// PilotDescription bio value under it) and its photo, all in the add-on.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "addon.h"

namespace ed {

struct PilotTrait {
	uint8_t level = 4;      // 0..8
	uint8_t variation = 0;  // 0..5
	uint8_t wave = 0;       // 0..3
	bool operator==(const PilotTrait &) const = default;
};

// The 31 AI traits in data order (DE_CPilot::ObjProcessContents), with the game's labels.
struct TraitInfo {
	const char *name;   // the GetStat* native's name
	const char *label;  // Choose Pilot's RECON DATA wording where it has one
	const char *group;
};
const std::array<TraitInfo, 31> &trait_infos();
extern const char *const kTraitLevels[9];

enum Sex : uint32_t { kMale = 0, kFemale = 1, kSexUndefined = 2 };

struct PilotData {
	std::string name;  // UTF-8
	uint32_t sex = kMale;
	std::string bio;   // UTF-8
	bool listed = true, locked = false, visible_when_locked = false;
	std::vector<Guid> casts;
	std::array<float, 4> stats{50, 50, 50, 50};  // power, agility, endurance, focus
	std::array<std::array<float, 4>, 3> colours{};
	std::array<float, 3> weights{};
	std::array<PilotTrait, 31> traits{};
	std::string photo_source;  // the photo's import file name, when it has one
	bool operator==(const PilotData &) const = default;
};

struct PilotObjects {
	int pilot = -1, description = -1, bio = -1, photo = -1;
};

// Every Actor\Creature\Pilot in the add-on.
std::vector<uint32_t> addon_pilots(const Project &p);
PilotObjects pilot_objects(const Project &p, uint32_t pilot);
PilotData read_pilot(const Project &p, const PilotObjects &o);
// Throws deo::Error when a chunk does not have the expected layout.
void write_pilot(Project &p, const PilotObjects &o, const PilotData &d);

// New pilot = a copy of a base pilot (its description, bio and photo) with fresh GUIDs, listed and
// unlocked. Returns the new Actor\Creature\Pilot's index.
uint32_t create_pilot(Project &p, const std::string &base_pilot, const std::string &name);

// Replaces the photo's pixels with an RGBA image (any size; cropped to a square and scaled to
// 256 x 256, the size of the shipped photos), stored uncompressed.
void import_photo(Project &p, const PilotObjects &o, int width, int height, const std::vector<uint8_t> &rgba,
                  const std::string &source_name);
// The photo as RGBA, or empty.
std::vector<uint8_t> photo_rgba(const Project &p, const PilotObjects &o, int *width, int *height);
// An Image\Standard's full-size pixels as RGBA, or empty.
std::vector<uint8_t> image_rgba(const deo::Object &image, int *width, int *height);
// A base pilot's photo (the first image among its children), or empty.
std::vector<uint8_t> base_pilot_photo(const BaseData &base, const BaseObject &pilot, int *width, int *height);

// The base game's pilot descriptions, by name, for "copy from".
std::vector<std::string> base_pilot_names(const BaseData &base);
// A name for a GUID: an add-on object's or a base object's, or the GUID's text.
std::string object_name(const Project &p, const Guid &g);

}  // namespace ed
