// A fighter's highest-detail mesh, skin, skeleton and moves, posed on the CPU and drawn in the
// pilot's paint (DE_COMF2FighterImageFilter); shared by openbg-viewer and the editor's preview.
#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "de/anim.h"
#include "de/image.h"
#include "de/mesh.h"
#include "de/objects_file.h"

namespace fighter {

struct Vertex {
	float pos[3];
	float normal[3];
	float uv[2];
};

struct Model {
	de::Mesh mesh;
	uint32_t mesh_index = 0;
	std::vector<std::array<std::array<float, 2>, 3>> face_uv;  // per face
	de::Image skin;
	de::Image colours;  // RGBStatic: the model's own colours, added unpainted
	float center[3] = {};
	float radius = 1;
};

// Skeleton, skin and moves for one fighter.
struct Rig {
	de::AnimBank bank;
	std::vector<de::SkinBone> bones;
	std::vector<int> bone_channel;  // channel index per bone
	std::vector<de::Transform> inv_bind;
};

std::vector<uint8_t> read_file(const std::filesystem::path &path);
// FighterModel -> highest-detail Mesh\Standard child -> its MultiStage material -> "RGBMain"
// TextureManager (UVs); the skin is the FighterModel's "RGBMain" Image\Standard. `spec` names a
// FighterModel or "#index"; null takes the first.
Model load_fighter(const de::ObjectsFile &f, const char *spec, bool verbose = true);
// The rig: the fighter file's bank and deform info, the first at or after `robot` (an add-on may hold several).
std::unique_ptr<Rig> load_rig(const de::ObjectsFile &fighter, const de::ObjectsFile &model_file, uint32_t mesh_index,
                              bool verbose = true, size_t robot = 0);

// Positions per mesh vertex for the current pose (linear blend skinning, inferred:
// v' = sum_b w_b * Pose_b * Bind_b^-1 * v, weights normalised per vertex).
void skin_vertices(const Model &m, const Rig *rig, const de::AnimSequence *seq, float t,
                   std::vector<std::array<float, 3>> &pos);
std::vector<Vertex> build_vertices(const Model &m, const std::vector<std::array<float, 3>> &pos);

// Uniforms: mvp, light, skin (texture unit 0) and pilot (a mat3 whose columns paint the mask's
// R, G and B channels).
extern const char *const kVertexShader;
extern const char *const kFragmentShader;

}  // namespace fighter
