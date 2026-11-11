// Arenas as the editor sees them: the playable list (Description\Environment plus its environment
// creature), an environment tree decoded with the engine's readers for the viewport and the
// Outliner, and the add-on variant of a base arena (docs/EDITOR_SPEC.md §4) with its edits.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "addon.h"
#include "de/lighting.h"
#include "de/scene.h"
#include "validate.h"

namespace ed {

struct ArenaEntry {
	std::string name;  // the description's, UTF-8
	Guid description, environment;
	int description_file = -1;  // base file of the description, -1 in the add-on
	int environment_file = -1;  // base file of the environment, -1 in the add-on
	bool addon = false;
	bool listed = true;  // offered by the game's arena lists (cinematic scenes are not)
	std::string stem;  // the base arena file drawn for it (a variant's source)
};
std::vector<ArenaEntry> base_arenas(const BaseData &base);
std::vector<ArenaEntry> addon_arenas(const Project &p);
// The base files an arena's environment needs (its file and what its GUID pool reaches, transitively).
std::vector<std::string> arena_files(const BaseData &base, const std::string &stem, const deo::File *addon = nullptr);

enum class NodeKind : uint8_t {
	Environment,
	Group,
	Geometry,
	Light,
	SpawnPoint,
	PickupSpawner,
	Hazard,
	Surface,
	ForceAffector,
	DeathVolume,
	NavArea,
	NavLink,
	Camera,
	Effect,
	Sound,
	Affector,
	Property,
	Value,
	Other,
	Count
};
const char *node_kind_name(NodeKind k);

struct SceneNode {
	uint32_t index = 0;  // in its file
	int parent = -1;     // node index
	std::vector<int> children;
	std::string name, class_path;
	Guid guid;
	NodeKind kind = NodeKind::Other;
	bool actor = false;
	de::ActorInfo info;
	de::Transform world;
	// DE_CAffector: collision groups and shape (game::read_affector).
	uint32_t groups = 0;
	enum class Shape : uint8_t { None, Sphere, Cone, Mesh, Box } shape = Shape::None;
	float hotspot = 0, falloff = 0;
	float near_begin = 0, near_end = 0, far_begin = 0, far_end = 0, umbra = 0, penumbra = 0;
	std::array<float, 3> box_min{0, 0, 0}, box_max{0, 0, 0};
	std::vector<std::array<float, 3>> triangles;  // a mesh affector's, in world space, 3 per face
	std::optional<de::LightInfo> light;
	int nav_to = -1;  // a NavLink's destination area (node index)
	std::string target;  // a passive reference's target name
	de::ObjectRef ref;
};

struct ArenaScene {
	const de::ObjectsFile *file = nullptr;
	int root = -1;
	std::vector<SceneNode> nodes;
	std::map<uint32_t, int> by_index;
	std::array<float, 3> lo{0, 0, 0}, hi{0, 0, 0};
	int node_for(uint32_t file_index) const;
	std::vector<int> of_kind(NodeKind k) const;
	// The nearest fighting-surface point straight below `p` within `drop`, if any.
	std::optional<float> surface_below(const std::array<float, 3> &p, float drop) const;
};
// The environment tree under `root` (an Actor\Creature\Environment) of `f`; `w` resolves references
// into other files (`f` need not be one of its files).
ArenaScene read_arena_scene(const de::World &w, const de::ObjectsFile &f, uint32_t root);

// An add-on variant of a base arena: the description (with its image) and the environment tree
// copied with fresh GUIDs, the description's resource pointing at the copied environment. Returns the
// new description's index.
uint32_t create_arena_variant(Project &p, const ArenaEntry &base, const std::string &name);

// Deep copy of objects of `src` (the add-on itself, or a base file) and their hierarchies with fresh
// GUIDs, references remapped by layout: known layouts, TextureProcess\MultiStage, then the reference
// grammar of content.h. Throws deo::Error when a references chunk cannot be explained.
std::vector<uint32_t> copy_tree(Project &p, const deo::File &src, bool src_is_addon, const std::vector<uint32_t> &roots,
                                std::map<uint32_t, uint32_t> *source_to_copy = nullptr);

// The first references chunk copy_tree cannot remap (-1: none).
int uncopyable_chunk(const std::string &class_path, const Bytes &references, size_t objects, size_t pool);

struct ArenaDescriptionData {
	std::string name;  // UTF-8
	int min_players = 2, max_players = 2;
	bool listed = true, locked = false;
	std::vector<Guid> casts;
	bool operator==(const ArenaDescriptionData &) const = default;
};
ArenaDescriptionData read_arena_description(const Project &p, uint32_t description);
void write_arena_description(Project &p, uint32_t description, const ArenaDescriptionData &d);

// An actor's local transform (contents chunk 0).
de::Transform actor_local(const deo::Object &o);
void set_actor_local(Project &p, uint32_t object, const de::Transform &local);

// A Light's embedded colour and intensity; false when the colour is a reference to a value object.
struct LightData {
	std::array<float, 4> rgba{1, 1, 1, 1};
	float intensity = 1;
	bool operator==(const LightData &) const = default;
};
bool read_light_data(const deo::Object &light, LightData *d);
void write_light_data(Project &p, uint32_t light, const LightData &d);

// A copy of an add-on actor and its subtree under the same parent, named `name`; returns its index.
uint32_t duplicate_actor(Project &p, uint32_t object, const std::string &name);
// Takes an add-on object out of its parent's hierarchy (it stays in the file, outside any tree).
void detach_object(Project &p, uint32_t object);
// The parent of an add-on object (hierarchy), -1 for a root.
int addon_parent(const Project &p, uint32_t object);

// Arena checks (spawns on fighting surfaces, nav net connected, players against spawn points).
std::vector<Issue> validate_arena(const ArenaScene &scene, int max_players);

}  // namespace ed
