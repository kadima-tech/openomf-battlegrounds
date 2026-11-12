// Robot (HAR) import (docs/EDITOR_SPEC.md §5.5): a skinned glTF model becomes a new HAR variant of
// a base robot whose model is replaced, so the HAR editor's tabs keep working on it.
//
// The rig the scripts drive stays the base robot's: its animation bank's channels (bone actors,
// HeadAnchor, cameras, grab handles) and its skin's bones. The file's joints are mapped onto that
// rig (by name, by a preset for common rigs, then by the skeleton's shape), the mesh is bound to the
// mapped joints, and the base robot's sequences are retargeted to the new proportions: each bone
// keeps the base's rotations, its bind frame is turned so the bone points the way the base's does,
// and offsets scale with the bone lengths. A file animation named as a sequence replaces it.
//
// What import_robot writes (all new GUIDs): create_har_variant's copy of the fighter file, a copy of
// the base's default FighterModel with its meshes rewritten in place (geometry, the fighter
// material's per-face UVs, one Deform<Bone>VertexData channel per skin bone), its images replaced
// (RGBMain paint mask, ReflectionMask, Bump; RGBStatic for the model's own colours, which the
// fighter filter adds unpainted: FINDINGS 165), the bank's channels and sequences, the deform
// info's binds, the bone actors' rest, and the sphere affectors moved and scaled.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "de/anim.h"
#include "de/mesh.h"
#include "json.h"
#include "model_import.h"

namespace ed {

// A base robot's rig and model, read from its files.
struct RobotRig {
	std::string robot, fighter_stem, model_stem, model_name, bank_name, deform_name;
	de::AnimBank bank;
	std::vector<std::array<uint8_t, 2>> channel_flags;  // the two BOOLs each bank channel stores
	std::vector<de::Transform> bind;  // per channel, model space: the skin's bind, or the rest pose
	std::vector<int> deformer;        // per channel: its index in `skin`, or -1
	std::vector<de::SkinBone> skin;   // the deform info's bones, with LOD0's weights
	struct Lod {
		std::string name;
		size_t faces = 0, vertices = 0;
	};
	std::vector<Lod> lods;  // the model's meshes, most faces first
	de::Mesh mesh;          // LOD0
	struct Sphere {
		std::string name, parent;  // the affector and the actor it hangs from
		int channel = -1;          // the parent's bank channel, when it is a bone actor
		de::Transform local;
		float hotspot = 0, falloff = 0;
	};
	std::vector<Sphere> spheres;  // in the fighter file's order
	std::vector<std::string> script_sequences;  // "seq<Name>" globals of its scripts that name sequences
	float height = 0;                         // the skin bones' vertical span (DE units)
	std::array<float, 3> forward{0, 0, 1}, left{1, 0, 0};  // where it faces and its left (DE space)
	int channel(const std::string &name) const { return bank.channel(name); }
	bool required(int channel) const { return channel >= 0 && size_t(channel) < deformer.size() && deformer[size_t(channel)] >= 0; }
};

// The rig of a base robot (cached per robot); throws std::runtime_error.
std::shared_ptr<const RobotRig> load_rig(const BaseData &base, const std::string &robot);

// Bone-mapping presets: "Auto", then the named rigs (Mixamo, Blender Rigify, Unreal, 3ds Max Biped,
// Generic).
const std::vector<std::string> &rig_presets();

// A robot import's analysis: the base robot's rig, the skin, LODs, bone mapping (plan.har.preset),
// orientation, sequences and spheres; then the checks. Throws std::runtime_error when the base
// robot cannot be read.
void analyse_robot(ImportPlan &plan, const BaseData &base);
// The automatic mapping again (manual choices kept) and the orientation that follows from it.
void map_bones(ImportPlan &plan);
void detect_orientation(ImportPlan &plan);
void robot_issues(const ImportPlan &plan, std::vector<Issue> &out);

// The file's space to the game's, in glTF terms: up axis, mirror, turn and ground (scale is the
// plan's units). A joint's bind transform in the file: its inverse bind's inverse, else its node.
gltf::Mat4 robot_fix(const ImportPlan &plan);
gltf::Mat4 joint_bind(const ImportPlan &plan, int node);

// What the import makes, as data (the dialog's preview animates it).
struct RobotLod {
	std::vector<std::array<float, 3>> vertices;
	std::vector<de::Mesh::Face> faces;
	std::vector<std::vector<std::array<std::array<float, 2>, 3>>> uvs;  // per UV layer, per face
	std::vector<std::vector<uint8_t>> weights;                          // per skin bone, per vertex
};
struct RobotImage {
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;
	bool given = false;  // from the file (else generated)
};
struct RobotBuild {
	std::vector<RobotLod> lods;
	std::vector<de::Transform> binds;  // per skin bone (RobotRig::skin order)
	de::AnimBank bank;                 // the rig's channels at their new rest, every sequence
	std::vector<de::Transform> rest;   // per channel, model space, at the new rest pose
	std::map<std::string, int> stage_layer;  // fighter-material stage (texture manager name) -> UV layer, -1 none; others 0
	float scale = 1;                   // new size / the base robot's
	RobotImage mask, reflection, colours, bump, scaled;  // RGBMain, ReflectionMask, RGBStatic, Bump, RGBScaled
	struct Sphere {
		size_t index = 0;  // into RobotRig::spheres
		std::string bone;  // the bone actor it hangs from ("" keeps its parent)
		de::Transform local;
		float hotspot = 0, falloff = 0;
	};
	std::vector<Sphere> spheres;
	size_t retargeted = 0, supplied = 0;
	size_t contacts = 0;  // retargeted sequences whose feet, throw grip or head anchor were corrected
	std::vector<std::string> notes;
};
// `lod0_only` (the preview) leaves out the generated LODs.
RobotBuild build_robot(const ImportPlan &plan, bool lod0_only = false);

// A new HAR variant with the imported model; ImportResult::root is its Description\Creature\Fighter.
ImportResult import_robot(Project &p, const ImportPlan &plan);

// The choices a re-import keeps (base robot, preset, bone and sequence mapping, orientation), as
// JSON by joint and animation name; stored beside the kept source.
json::Value robot_mapping_json(const ImportPlan &plan);
void apply_robot_mapping(ImportPlan &plan, const json::Value &v);
std::filesystem::path robot_mapping_path(const Project &p, const std::string &name);

}  // namespace ed
