// Skeletal animation: AnimationSequenceGroup banks and ActorDeformInfo skins.
//
// AnimationSequenceGroup v4 (Core 0x701bbf70): name; i32 n sequences {name; i32 n streams
// {u16 channel; DE_CStream v1 chunk (0x701bcd80): i32 n {f32 t, x, y, z}; i32 n {f32 t, quat}}};
// i32 n channels {name; BOOL; BOOL; i16 parent; f32 rest[3]; quat rest}. Quaternions are
// (x, y, z, w) (DE_CQuaternion::ProcessContents 0x700274c0; identity stored as 0,0,0,1).
// Sequence time is normalised to 0..1.
//
// ActorDeformInfo v3 (0x701201e0): name; BOOL-terminated deformers, each a v3 chunk
// (0x70120d50) {f32 bind position[3]; DE_CCoordinateSystem rotation}. References (0x701204b0)
// give each deformer's bone actor. Per-vertex weights are mesh children named
// "Deform<Bone>VertexData" (DataChannel\UnitFloats, 0x701168a0: one byte per vertex).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "de/scene.h"

namespace de {

using Quat = std::array<float, 4>;  // x, y, z, w

Transform transform_from(const std::array<float, 3> &pos, const Quat &q);

struct AnimChannel {
	std::string name;
	int16_t parent = -1;
	std::array<float, 3> rest_pos{};
	Quat rest_rot{0, 0, 0, 1};
};

struct AnimSequence {
	std::string name;
	struct PosKey {
		float t;
		std::array<float, 3> p;
	};
	struct RotKey {
		float t;
		Quat q;
	};
	struct Stream {
		uint16_t channel;
		std::vector<PosKey> pos;
		std::vector<RotKey> rot;
	};
	std::vector<Stream> streams;
};

struct AnimBank {
	std::string name;
	std::vector<AnimSequence> sequences;
	std::vector<AnimChannel> channels;

	const AnimSequence *find(const std::string &name) const;
	int channel(const std::string &name) const;
	// Model-space transform of every channel at normalised time t (channels without a stream
	// use their rest transform). The root channel "Body" carries the move's placement in the
	// animator's scene (root motion); with root_motion = false it is held at the origin.
	std::vector<Transform> pose(const AnimSequence &seq, float t, bool root_motion = false) const;
};

AnimBank read_anim_bank(const ObjectsFile &f, const ObjectInfo &o);

// AnimationStreamPlayer (Core 0x701c1cb0) v1: [name][DE_CStream v1: position and orientation
// keys]. Its loop flag (+0x48) is cleared by ObjInitContents and set by nothing, so times are
// never wrapped.
struct AnimPath {
	std::vector<AnimSequence::PosKey> pos;
	std::vector<AnimSequence::RotKey> rot;
	// DE_CAnimationStreamPlayer::SetTime's key search: the keys around t (equal before the
	// first key or with a single key) and the factor between them, not clamped past the last.
	template <typename Key>
	static bool around(const std::vector<Key> &k, float t, size_t &a, size_t &b, float &u) {
		if (k.empty()) return false;
		a = 0, b = 0, u = 0;
		if (k.size() > 1 && k[0].t <= t) b = 1;
		if (k[b].t < t) {
			while (b + 1 < k.size()) {
				++a, ++b;
				if (t <= k[b].t) break;
			}
		}
		if (a != b) u = (t - k[a].t) / (k[b].t - k[a].t);
		return true;
	}
	bool position(float t, std::array<float, 3> &p, size_t *next = nullptr) const;
	bool rotation(float t, Quat &q) const;
	float time_for_distance(float distance, float t) const;  // GetTimeForDistance
};
AnimPath read_anim_path(const ObjectsFile &f, const ObjectInfo &o);
Quat slerp_quat(const Quat &a, const Quat &b, float t);

// Blend two model-space poses: lerp translations and rotation rows, then re-orthonormalise.
std::vector<Transform> blend_poses(const std::vector<Transform> &a, const std::vector<Transform> &b, float w);

struct SkinBone {
	std::string name;     // bone actor name, e.g. "LeftUpperArm"
	Transform bind;       // model space at bind time
	std::vector<uint8_t> weights;  // per mesh vertex, 0..255 (may be empty)
};

// Deformers of an ActorDeformInfo plus the weights stored on `mesh` (a Mesh\Standard object).
std::vector<SkinBone> read_skin(const ObjectsFile &deform_file, const ObjectInfo &deform_info,
                                const ObjectsFile &mesh_file, uint32_t mesh_index);

}  // namespace de
