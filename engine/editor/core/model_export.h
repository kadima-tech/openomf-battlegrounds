// DE content to glTF (docs/EDITOR_SPEC.md §5.5): meshes, LOD switchers, props, arenas and robots.
//
// Space: DE is left-handed with +Y up; glTF is right-handed. A DE point (x, y, z) in units is the
// glTF point (x, y, -z) / units_per_metre, triangles are reversed so they keep facing out, and a
// DE transform M becomes S M S (S = diag(1, 1, -1)). asset.extras.de_units_per_metre records the
// scale so an import of the file restores DE units exactly.
//
// What the extras carry (so an import rebuilds the objects as they were):
//  - mesh: de_mesh_flags, de_placement, de_face_flags (smoothing groups, per DE face),
//    de_vertex_count; primitive: de_faces (the DE face of each triangle) and the custom vertex
//    attribute _DE_VERTEX (the DE vertex of each glTF vertex).
//  - material: de_stages, one per TextureProcess\MultiStage stage: name, class, image (glTF image
//    index or -1), uv_set (TEXCOORD_n), mode, colour, stacked, marked, flag_30, flag_v7, disabled,
//    the multistage version, and base GUIDs (image_guid, cube_guid, colour_value_guid) for what
//    the importer can reference instead of copying.
//  - LODs: nodes LOD0.. under the switcher's node, extras.de_lod_switch per LOD.
//  - arenas: root collections Visual, Collision (materials "floor" / "wall"), Death, JumpPad
//    (extras.impulse: the launch direction), Spawn, Pickup (extras.pickups: SharedObjects names),
//    Navigation (areas with extras.radius and extras.links, for reference).
//  - robots: an Armature of the animation bank's channels with the skin's inverse binds, up to 4
//    weights per vertex, and every sequence as an animation over 1 s (extras.de_normalised).
#pragma once

#include <string>
#include <vector>

#include "de/scene.h"
#include "gltf.h"

namespace ed {

// A robot is about 30 units tall and Grudge Match 770 across: at 10 units a metre, 3 m and 77 m.
constexpr float kDefaultUnitsPerMetre = 10;

struct ExportOptions {
	float units_per_metre = kDefaultUnitsPerMetre;
	bool animations = true;  // robots
	bool gameplay = true;    // arenas: the collision and gameplay collections
};

// DE <-> glTF space (see above).
gltf::Vec3 to_gltf(const std::array<float, 3> &p, float units_per_metre);
std::array<float, 3> from_gltf(const gltf::Vec3 &p, float units_per_metre);
gltf::Mat4 to_gltf(const de::Transform &t, float units_per_metre);
de::Transform from_gltf(const gltf::Mat4 &m, float units_per_metre);  // rotation re-orthonormalised

// What exporting an object of this class makes: "mesh", "lod", "prop", "arena", "robot", or ""
// when it cannot be exported.
std::string export_kind(const std::string &class_path);

// The object as a glTF scene; `w` must hold every file its references reach. Throws
// std::runtime_error when it cannot be exported.
gltf::Scene export_object(const de::World &w, const de::ObjectRef &object, const ExportOptions &opt = {});

// DE_CStandardMesh::CalculateNormals: each face corner's normal from the faces around it that share
// a smoothing group (the renderer's rule), in mesh space.
std::vector<std::array<std::array<float, 3>, 3>> corner_normals(const de::Mesh &mesh);

}  // namespace ed
