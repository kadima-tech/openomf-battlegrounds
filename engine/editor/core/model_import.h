// glTF models into the open add-on (docs/EDITOR_SPEC.md §5.5): props (a prefab actor with its
// meshes, materials and images) and arenas (a new environment and description built from glTF
// collections). Robot (HAR) import builds on the same plan: analyse() fills an ImportPlan the
// Import dialog shows and edits page by page, then an import_* function writes the objects.
//
// Every object made is new (fresh GUIDs, §7) and written in the layouts the original reads:
//  - Mesh\Standard: base chunk v4 (name, flags, placement), v6 counts, ObjExtCon v1 geometry with
//    face adjacency and smoothing groups (from the glTF normals, or de_face_flags);
//  - TextureProcess\MultiStage v8 stages, one TextureManager (ObjExtCon v1 per-face UVs) or
//    TextureManager\Reflection per stage; Image\Standard pixels uncompressed (power-of-two sizes);
//  - actors (v7) with the mesh as hierarchy child and component.
// Arenas copy their environment (lights, sky, music, cameras, scripts) from a template arena and
// their gameplay objects from fixed base templates (spawn point, pickup spawner, death box, the
// collision and jump pad mesh affectors, navigation areas and links), then rewrite them.
#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "addon.h"
#include "gltf.h"
#include "validate.h"

namespace ed {

// What a glTF node becomes.
enum class Role : uint8_t { Visual, Floor, Wall, Kill, Death, JumpPad, Spawn, Pickup, Ignore, Count };
const char *role_name(Role r);

enum class ImportKind : uint8_t { Prop, Arena, Robot };

struct ImportSettings {
	ImportKind kind = ImportKind::Prop;
	std::string name;                 // the prop's or arena's name
	float units_per_metre = 10;       // DE units per glTF unit (asset.extras.de_units_per_metre when present)
	bool z_up = false;                // the file is Z-up (rotated to Y-up first)
	std::string template_arena = "Grudge Match";  // arenas: lights, sky, music and cameras from this one
	bool keep_sky = true;             // arenas: the template's sky and cloud meshes stay
	bool generate_nav = true;         // arenas: a navigation net over the floors
	float nav_spacing = 0;            // 0: chosen from the floor size
};

// One node of the file with a mesh or an empty that matters (spawn, pickup).
struct ImportItem {
	int node = -1;
	std::string name, path;   // the node's name and its ancestors' ("Collision/Ramp")
	std::string collection;   // the collection it was found under ("" for none)
	Role role = Role::Visual;
	size_t triangles = 0, vertices = 0;
	std::vector<std::string> pickups;  // Pickup: SharedObjects pickups it spawns
	std::array<float, 3> impulse{0, 1, 0};  // JumpPad: launch direction (glTF space)
	bool has_impulse = false;
};

struct ImportMaterial {
	int material = -1;
	std::string name;
	bool rebuilt = false;   // extras.de_stages: the DE stages come back as they were
	int image = -1;         // base colour image
	size_t stages = 1;
};

struct ImportImage {
	int image = -1;
	std::string name;
	int width = 0, height = 0;      // as in the file
	int out_width = 0, out_height = 0;  // as stored (power of two, at most kMaxTexture)
	bool used = false;
};

// Robots (har_import.h): how the file's skeleton, animations and hitboxes meet the base robot's rig.
enum class MapSource : uint8_t { None, Name, Preset, Guess, Manual };
const char *map_source_name(MapSource s);

struct BoneMapping {
	std::string bone;       // the rig's name (channel and bone actor)
	int joint = -1;         // the file's node
	MapSource source = MapSource::None;
	float confidence = 0;   // 0..1
	bool required = true;   // a bone of the base robot's skin; the others are placed from it when unmapped
};

struct SequenceMapping {
	std::string sequence;   // the base bank's name, as the scripts use it
	int animation = -1;     // the file's animation replacing it; -1: the base robot's, retargeted
	MapSource source = MapSource::None;
};

struct SphereSource {
	std::string sphere;     // the affector's name ("Sphere:<name>" in the file)
	int node = -1;
	std::string bone;       // the rig bone it ends up under
	bool known = false;     // the base robot has an affector of that name
};

struct RobotRig;

struct HarPlan {
	std::string base_robot = "Jaguar";
	std::string preset = "Auto";     // a rig_presets() name
	std::string detected;            // the preset Auto chose
	int skin = -1;
	std::vector<BoneMapping> bones;
	std::vector<SequenceMapping> sequences;
	std::vector<SphereSource> spheres;
	std::array<std::vector<int>, 3> lods;  // mesh nodes of LOD0..2 (LOD1 and LOD2 generated when empty)
	bool auto_orient = true;  // facing and mirror from the skeleton
	bool mirror = false;      // left and right swapped into the game's convention
	int turn = 0;             // degrees about the up axis: 0, 90, 180, 270
	bool ground = true;       // the lowest vertex on the floor
	enum class Paint : uint8_t { Materials, Colours } paint = Paint::Colours;
	bool decimate_lod0 = false;  // LOD0 to the base robot's budget as well
	std::shared_ptr<const RobotRig> rig;
	std::vector<std::string> notes;  // what the analysis found
};

struct ImportPlan {
	std::filesystem::path source;
	gltf::Scene scene;
	ImportSettings settings;
	std::vector<ImportItem> items;
	std::vector<ImportMaterial> materials;
	std::vector<ImportImage> images;
	std::vector<Issue> issues;  // validation: format limits, names, missing pieces
	bool from_editor = false;   // written by our exporter (round trip)
	HarPlan har;                // robots
	bool errors() const;
	size_t count(Role r) const;
};

constexpr int kMaxTexture = 1024;
constexpr size_t kMaxMeshVertices = 65535, kMaxMeshFaces = 65535;
constexpr size_t kMaxNavAreas = 64, kMaxNavLinks = 256;

// Reads and analyses a file: roles from collections (Visual, Collision, Death, JumpPad, Spawn,
// Pickup; a node named or under a node named so, or extras.de_collection / de_role), the
// collision material names (floor, wall, kill), sizes and limits. Throws gltf::Error.
ImportPlan analyse(const std::filesystem::path &file, const ImportSettings &settings);
ImportPlan analyse(gltf::Scene scene, const ImportSettings &settings);
// Re-runs validation after the dialog changed roles or settings.
void revalidate(ImportPlan &plan, const BaseData *base = nullptr);

// The pickups arenas spawn (objects named by any base arena's PickupsToSpawn), by name.
std::vector<std::string> pickup_names(const BaseData &base);

struct ImportResult {
	uint32_t root = 0;         // the prefab actor (prop) or the description (arena)
	std::vector<uint32_t> made;  // every object added
	size_t meshes = 0, materials = 0, images = 0, nav_areas = 0, nav_links = 0, spawns = 0, pickups = 0;
	std::vector<std::string> notes;
	std::string summary() const;
};

// A prefab: a Folder "<name>" holding the prefab actor (its meshes as components), the meshes
// with their materials, and the images. Place a copy with place_prefab.
ImportResult import_prop(Project &p, const ImportPlan &plan);
// A new arena: description (listed, unlocked, max players = spawn points), environment,
// collision, spawn points, pickup spawners, death boxes, jump pads and a navigation net.
ImportResult import_arena(Project &p, const ImportPlan &plan);

// The add-on's prefab actors (imported props): actors under a Folder whose components are meshes.
std::vector<uint32_t> prefabs(const Project &p);
// A copy of a prefab actor under `parent` (an environment or one of its actors) at `local`,
// drawing the prefab's meshes. Returns the new actor.
uint32_t place_prefab(Project &p, uint32_t prefab, uint32_t parent, const std::array<float, 9> &rotation, const std::array<float, 3> &position);

// Keeps a copy of the source file (and a .gltf's .bin and images) in the project's sidecar
// folder (<add-on>.assets/models/<name>/); returns the copied file's path.
std::filesystem::path keep_source(const Project &p, const std::filesystem::path &source, const std::string &name);
std::filesystem::path sidecar_dir(const Project &p);

}  // namespace ed
