// Static scene assembly: actors, transforms, meshes and their base textures.
//
//  - DE_CActor contents (Core 0x7008fca0): name, f32 position[3], DE_CCoordinateSystem (0x70025f70,
//    9 floats: orthonormal rotation rows), flags. DE_CObject writes no contents chunk, so the actor
//    chunk is the first one for every Actor-derived class.
//  - DE_CActor references (0x7008ff90): after the DE_CObject chunk, a BOOL-terminated list of
//    component references (meshes, materials, ...).
//  - DE_CTextureManager references (0x70131c40): after the DE_CObject chunk, i32 count + images.
//  - References may point into other files through the GUID pool, so a World loads every file.
#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "de/image.h"
#include "de/mesh.h"
#include "de/objects_file.h"
#include "de/texture.h"

namespace de {

struct ObjectRef {
	const ObjectsFile *file = nullptr;
	uint32_t index = 0;
	explicit operator bool() const { return file != nullptr; }
	const ObjectInfo &info() const { return file->objects()[index]; }
};

class World {
public:
	// Every *.DEObjects in `data_dir`, plus the objects files the engine modules carry as
	// "DEOBJECTS" resources (Core.ModuleDLL: COREMeshes -- fonts, primitives -- and COREImages)
	// when the game's Engine directory sits next to it.
	explicit World(const std::filesystem::path &data_dir);

	const std::filesystem::path &dir() const { return dir_; }
	const ObjectsFile *file(const std::string &stem) const;
	const std::map<std::string, std::unique_ptr<ObjectsFile>> &files() const { return files_; }
	ObjectRef resolve(const ObjectsFile &from, const Reference &r) const;
	ObjectRef by_guid(const Guid &g) const;
	// The first object of that class and name in any file (stems in order).
	ObjectRef find(const std::string &class_path, const std::string &name) const {
		for (auto &[stem, f] : files_) {
			for (uint32_t i = 0; i < f->objects().size(); ++i) {
				if (f->objects()[i].class_path == class_path && f->objects()[i].name == name) return {f.get(), i};
			}
		}
		return {};
	}

private:
	void add(const std::string &stem, std::vector<uint8_t> data);
	std::filesystem::path dir_;
	std::map<std::string, std::unique_ptr<ObjectsFile>> files_;
	std::map<std::array<uint8_t, 16>, ObjectRef> guids_;
};

// Row-major rotation (rows are the local X/Y/Z axes in parent space) plus translation.
struct Transform {
	std::array<float, 9> r{1, 0, 0, 0, 1, 0, 0, 0, 1};
	std::array<float, 3> t{0, 0, 0};

	std::array<float, 3> apply(const std::array<float, 3> &p) const {
		return {t[0] + p[0] * r[0] + p[1] * r[3] + p[2] * r[6], t[1] + p[0] * r[1] + p[1] * r[4] + p[2] * r[7],
		        t[2] + p[0] * r[2] + p[1] * r[5] + p[2] * r[8]};
	}
	std::array<float, 3> rotate(const std::array<float, 3> &p) const {
		return {p[0] * r[0] + p[1] * r[3] + p[2] * r[6], p[0] * r[1] + p[1] * r[4] + p[2] * r[7],
		        p[0] * r[2] + p[1] * r[5] + p[2] * r[8]};
	}
	// World-to-local direction (transpose of rotate).
	std::array<float, 3> inverse_rotate(const std::array<float, 3> &p) const {
		return {p[0] * r[0] + p[1] * r[1] + p[2] * r[2], p[0] * r[3] + p[1] * r[4] + p[2] * r[5],
		        p[0] * r[6] + p[1] * r[7] + p[2] * r[8]};
	}
	// this = parent * local
	Transform then(const Transform &local) const;
	// Inverse of a rigid transform (rows are orthonormal).
	Transform inverse() const {
		Transform o;
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 3; ++j) {
				o.r[i * 3 + j] = r[j * 3 + i];
			}
		}
		auto rt = o.rotate(t);
		o.t = {-rt[0], -rt[1], -rt[2]};
		return o;
	}
};

struct ActorInfo {
	std::string name;
	Transform local;
	// DE_CActor::ObjProcessContents (Core 0x7008fca0): v4+ the position mode (+0x10c: 0 inherit,
	// 1 static, 2 dynamic), v5+ the flags (+0x110: 2 shown at low detail, 4 at high detail).
	// ObjInitContents' defaults stand for older versions.
	int32_t position_mode = 2;
	uint32_t flags = 6;
};
// The detail level (the registry's high-detail BOOL, default 1, global 0x7035dec0).
inline bool g_high_detail = true;
// DE_CActor::ObjPrepare: an actor without its detail level's flag is left out of the scene.
inline bool shown_at_detail(uint32_t flags) { return (flags & (g_high_detail ? 4u : 2u)) != 0; }
// Throws FormatError when the object's first contents chunk is not an actor chunk.
ActorInfo read_actor(const ObjectsFile &f, const ObjectInfo &o);

// DE_CMultiStageTextureProcess::ObjProcessReferences (Core 0x701190d0), references chunk 2:
// i32 count, then per stage [texture manager][BOOL colour is a reference: else RGBA floats]
// [mode: v4 a name, v5+ i32][BOOL +0x2c][i32 process +0x4c][v6+ BOOL +0x30][v7 BOOL][v8 BOOL +0x34].
// Modes: 0 Replace/Opaque, 1 Add, 2 Modulate, 3 AddToLightMap, 4 SemiTranslucent.
struct StageInfo {
	Reference manager;
	Reference colour_value;  // an animated colour, when set
	std::array<float, 4> colour{1, 1, 1, 1};
	int32_t mode = 0;
	bool flag_2c = false, flag_30 = false, disabled = false;
};
struct MultiStageInfo {
	int version = 0;
	int32_t first_marked = 0;  // process +0x4c
	std::vector<StageInfo> stages;
};
MultiStageInfo read_multistage(const ObjectsFile::ObjectData &d);

// One TextureManager stage of a TextureProcess\MultiStage material.
struct Layer {
	std::string name;
	ObjectRef manager;  // the TextureManager object (scripts can swap its image at run time)
	std::vector<FaceUV> uvs;  // also defines which mesh faces the stage covers
	ObjectRef image;
	bool lightmap = false;    // named like a lightmap (for the simple viewers)
	bool reflection = false;  // TextureManager\Reflection / reflection masks
	// From the MultiStage reference stream (StageInfo): blend mode, flat colour, whether the
	// stage stacks onto the previous one in the same pass (+0x2c), and the environment
	// reflection (a TextureManager\Reflection: camera-space reflection vector into the cube map).
	int32_t mode = 0;
	std::array<float, 4> colour{1, 1, 1, 1};
	bool stacked = false;
	bool environment = false;
	bool disabled = false;
	// The stage's colour channel: one RGB per face corner, in `uvs` order (white when empty).
	std::vector<std::array<float, 3>> corner_colours;
	// A "SpecularFactor" child (1 without one): a pass with a reflection stage (render element
	// property 0x100, DE_CReflectionTextureManager::Process) is also drawn into the glow layer
	// with the specular cube, scaled by its last stage's factor (DE_CShader::GetShader, 0xc00).
	float specular = 1;
	// An environment stage's own cube (an Image\CubeEnvironment reference); else the arena's.
	ObjectRef cube;
	// The stage image's "Bump" child image, its "BumpFactor" and "RotateValue" (DE_CShader::CStage::
	// SetStates, Direct3D 0x20031b60): the stage image's du/dv channels (6, 7) bend the reflection.
	ObjectRef bump;
	float bump_factor = 0, bump_rotate = 0;
};

struct Material {
	std::string name;
	std::vector<Layer> layers;
	// The mesh's "GlowFactor" child: every pass is also drawn into the glow layer (flag 0x400).
	float glow = 0;
	// Simplified two-stage view: base colour and lightmap (either may be null).
	const Layer *base() const;
	const Layer *light() const;
};

struct DrawItem {
	std::string actor;
	uint32_t actor_index = 0;  // the actor (in the scene file) whose mesh it is
	// DE_CActor::IsPositionStatic (0x700917c0): the nearest set position mode up the tree is 1.
	bool position_static = false;
	ObjectRef mesh_object;
	Mesh mesh;
	Transform world;
	std::vector<Material> materials;
};

// The TextureProcess\\MultiStage materials under a mesh object, with their stages' UVs and images.
std::vector<Material> read_materials(const World &w, const ObjectRef &mesh);

// The Mesh\\Standard objects of an actor: its mesh children (unless `children` is false) and its
// mesh components. Only the components are drawn (DE_CD3DRenderActor::GetValidActorType).
std::vector<ObjectRef> actor_meshes(const World &w, const ObjectRef &actor, bool children = true);

// Every mesh drawn by the actors under `root_file` (all actors in that file).
std::vector<DrawItem> collect_scene(const World &w, const ObjectsFile &root_file);

}  // namespace de
