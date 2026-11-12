// Robot (HAR) import (docs/EDITOR_SPEC.md §5.5, editor/core/har_import.h).
//
//   editor-har-import-tests <Data dir>            Jaguar exported and imported back, a generated
//                                                 humanoid with unknown joint names and other
//                                                 proportions, and a Mixamo-named rig: mapping,
//                                                 retargeting (no stretched or exploded poses),
//                                                 the saved add-on read back by the engine
//   editor-har-import-tests --samples <dir>       writes the generated rigs (humanoid.glb, mixamo.glb)
//   editor-har-import-tests <Data dir> --keep <dir>   also keeps the add-ons (for openbg --addon)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <random>
#include <set>

#include "addon.h"
#include "de/anim.h"
#include "de/mesh.h"
#include "de/scene.h"
#include "game/descriptions.h"
#include "gltf.h"
#include "har.h"
#include "har_import.h"
#include "model_export.h"
#include "model_import.h"
#include "content.h"
#include "pilot.h"
#include "validate.h"

namespace fs = std::filesystem;
using V3 = std::array<float, 3>;

static int failures = 0;

#define CHECK(cond)                                                                   \
	do {                                                                              \
		if (!(cond)) {                                                                \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                               \
		}                                                                             \
	} while (0)

static void set_env(const char *name, const std::string &value) {
#ifdef _WIN32
	_putenv_s(name, value.c_str());
#else
	setenv(name, value.c_str(), 1);
#endif
}

static fs::path scratch() {
	static std::random_device rd;
	fs::path d = fs::temp_directory_path() / ("openbg-har-import-tests-" + std::to_string(rd()) + std::to_string(rd()));
	fs::create_directories(d);
	return d;
}

static float dist(const V3 &a, const V3 &b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

// ---------------------------------------------------------------------------------------------
// Generated rigs: a T-posed humanoid of boxes, one per bone, facing +Z with its left at +X (the
// glTF convention), skinned with a blend at each joint.

struct Joint {
	std::string name;
	int parent;
	V3 at;  // world, metres
};

enum class Naming { Mixamo, Unknown };

static bool name_is(const std::string &n, const std::string &suffix) { return n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0 && (n.size() == suffix.size() || n[n.size() - suffix.size() - 1] == ':'); }

static std::vector<Joint> humanoid_joints(Naming naming, float arm, float leg) {
	const bool mx = naming == Naming::Mixamo;
	auto n = [&](const char *mixamo, int k) { return mx ? std::string("mixamorig:") + mixamo : "joint" + std::to_string(k); };
	std::vector<Joint> j;
	const float hip = 0.9f * leg;
	j.push_back({n("Hips", 0), -1, {0, hip, 0}});
	j.push_back({n("Spine", 1), 0, {0, hip + 0.12f, 0}});
	j.push_back({n("Spine1", 2), 1, {0, hip + 0.26f, 0}});
	j.push_back({n("Spine2", 3), 2, {0, hip + 0.42f, 0}});
	j.push_back({n("Neck", 4), 3, {0, hip + 0.58f, 0}});
	j.push_back({n("Head", 5), 4, {0, hip + 0.66f, 0.01f}});
	j.push_back({n("HeadTop_End", 6), 5, {0, hip + 0.86f, 0.01f}});
	int k = 7;
	for (int side = 0; side < 2; ++side) {
		const float sx = side == 0 ? 1 : -1;
		const std::string S = side == 0 ? "Left" : "Right";
		int sh = int(j.size());
		j.push_back({n((S + "Shoulder").c_str(), k++), 3, {sx * 0.07f, hip + 0.52f, 0}});
		j.push_back({n((S + "Arm").c_str(), k++), sh, {sx * 0.17f, hip + 0.52f, 0}});
		j.push_back({n((S + "ForeArm").c_str(), k++), sh + 1, {sx * (0.17f + 0.28f * arm), hip + 0.52f, 0}});
		j.push_back({n((S + "Hand").c_str(), k++), sh + 2, {sx * (0.17f + 0.53f * arm), hip + 0.52f, 0}});
		j.push_back({n((S + "HandIndex1").c_str(), k++), sh + 3, {sx * (0.17f + 0.62f * arm), hip + 0.52f, 0.02f}});
		j.push_back({n((S + "HandIndex2").c_str(), k++), sh + 4, {sx * (0.17f + 0.68f * arm), hip + 0.52f, 0.02f}});
		int up = int(j.size());
		j.push_back({n((S + "UpLeg").c_str(), k++), 0, {sx * 0.1f, hip - 0.05f, 0}});
		j.push_back({n((S + "Leg").c_str(), k++), up, {sx * 0.1f, hip * 0.53f, 0.01f}});
		j.push_back({n((S + "Foot").c_str(), k++), up + 1, {sx * 0.1f, 0.08f, 0}});
		j.push_back({n((S + "ToeBase").c_str(), k++), up + 2, {sx * 0.1f, 0.02f, 0.13f}});
		j.push_back({n((S + "Toe_End").c_str(), k++), up + 3, {sx * 0.1f, 0.02f, 0.2f}});
	}
	return j;
}

static gltf::Scene humanoid(Naming naming, float arm, float leg, bool with_animation) {
	auto joints = humanoid_joints(naming, arm, leg);
	gltf::Scene s;
	s.generator = "editor-har-import-tests";
	// Mixamo through Blender: an Armature node at a hundredth scale, joints in centimetres.
	const bool cm = naming == Naming::Mixamo;
	const float k = cm ? 100.0f : 1.0f;
	gltf::Node arm_node;
	arm_node.name = "Armature";
	if (cm) arm_node.scale = {0.01f, 0.01f, 0.01f};
	int armature = s.add_node(arm_node);
	std::vector<int> node(joints.size());
	for (size_t i = 0; i < joints.size(); ++i) {
		gltf::Node n;
		n.name = joints[i].name;
		V3 p = joints[i].at;
		if (joints[i].parent >= 0) {
			const V3 &q = joints[size_t(joints[i].parent)].at;
			p = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
		}
		n.translation = {p[0] * k, p[1] * k, p[2] * k};
		node[i] = s.add_node(n, joints[i].parent >= 0 ? node[size_t(joints[i].parent)] : armature);
	}
	gltf::Skin skin;
	skin.name = "Armature";
	skin.skeleton = armature;
	for (size_t i = 0; i < joints.size(); ++i) {
		skin.joints.push_back(node[i]);
		skin.inverse_binds.push_back(gltf::inverse(s.world(node[i])));
	}
	s.skins.push_back(skin);
	// A box along each bone (from the joint to its first child), half its weight to the next bone
	// at the far end.
	gltf::Primitive prim;
	gltf::Material mat;
	mat.name = "Body";
	mat.base_colour = {0.7f, 0.72f, 0.78f, 1};
	mat.metallic = 0;
	s.materials.push_back(mat);
	gltf::Material mat2 = mat;
	mat2.name = "Joints";
	mat2.base_colour = {0.9f, 0.4f, 0.2f, 1};
	s.materials.push_back(mat2);
	gltf::Primitive prim2;
	prim.material = 0, prim2.material = 1;
	for (size_t i = 0; i < joints.size(); ++i) {
		int child = -1;
		for (size_t c = 0; c < joints.size(); ++c) {
			if (joints[c].parent == int(i)) {
				child = int(c);
				break;
			}
		}
		if (child < 0) continue;
		V3 a = joints[i].at, b = joints[size_t(child)].at;
		V3 d{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
		float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (len < 1e-4f) continue;
		V3 x{d[0] / len, d[1] / len, d[2] / len};
		V3 up = std::fabs(x[1]) < 0.9f ? V3{0, 1, 0} : V3{0, 0, 1};
		V3 y{x[1] * up[2] - x[2] * up[1], x[2] * up[0] - x[0] * up[2], x[0] * up[1] - x[1] * up[0]};
		float yl = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
		y = {y[0] / yl, y[1] / yl, y[2] / yl};
		V3 z{x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
		float w = std::clamp(len * 0.3f, 0.03f, 0.08f);
		gltf::Primitive &P = (i % 3 == 0) ? prim2 : prim;
		const uint32_t base = uint32_t(P.positions.size());
		for (int c = 0; c < 8; ++c) {
			float t = (c & 1) ? len : 0;
			float sy = (c & 2) ? w : -w, sz = (c & 4) ? w : -w;
			V3 p{a[0] + x[0] * t + y[0] * sy + z[0] * sz, a[1] + x[1] * t + y[1] * sy + z[1] * sz, a[2] + x[2] * t + y[2] * sy + z[2] * sz};
			P.positions.push_back({p[0], p[1], p[2]});
			bool leaf = std::none_of(joints.begin(), joints.end(), [&](const Joint &jj) { return jj.parent == child; });
			if ((c & 1) && !leaf) P.joints.push_back({uint16_t(i), uint16_t(child), 0, 0}), P.weights.push_back({0.5f, 0.5f, 0, 0});
			else P.joints.push_back({uint16_t(i), 0, 0, 0}), P.weights.push_back({1, 0, 0, 0});
		}
		const int q[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
		for (auto &f : q) {
			for (int t : {0, 1, 2, 0, 2, 3}) P.indices.push_back(base + uint32_t(f[t]));
		}
	}
	gltf::Mesh mesh;
	mesh.name = "Body";
	mesh.primitives.push_back(prim);
	mesh.primitives.push_back(prim2);
	s.meshes.push_back(mesh);
	gltf::Node body;
	body.name = "Body";
	body.mesh = 0;
	body.skin = 0;
	s.add_node(body);
	if (naming == Naming::Unknown) {
		// Spheres: a new body sphere on the left forearm, a new attack sphere on the right hand, and
		// one whose name no script looks for.
		for (auto [name, joint] : {std::pair<const char *, const char *>{"Sphere:GetHitAffectorForearm", "joint9"}, {"Sphere:RightPunchMediumHitDetector3", "joint21"}, {"Sphere:Antenna", "joint5"}}) {
			gltf::Node n;
			n.name = name;
			n.scale = {0.4f, 0.4f, 0.4f};
			for (size_t i = 0; i < joints.size(); ++i) {
				if (joints[i].name == joint) s.add_node(n, node[i]);
			}
		}
	}
	if (with_animation) {
		// "Winning1": the left arm waved, so a supplied sequence replaces the base's.
		gltf::Animation a;
		a.name = "Armature|Winning1";
		gltf::Animation::Sampler smp;
		int arm_joint = -1;
		for (size_t i = 0; i < joints.size(); ++i) {
			if (name_is(joints[i].name, "LeftArm") || (naming == Naming::Unknown && joints[i].name == "joint8")) arm_joint = int(i);
		}
		for (int key = 0; key <= 10; ++key) {
			float t = float(key) / 10.0f;
			smp.input.push_back(t);
			float ang = std::sin(t * 6.2831853f) * 0.8f;
			smp.output.insert(smp.output.end(), {0, 0, std::sin(ang / 2), std::cos(ang / 2)});
		}
		a.samplers.push_back(smp);
		a.channels.push_back({0, node[size_t(arm_joint)], gltf::Animation::Path::Rotation});
		s.animations.push_back(a);
	}
	return s;
}


// ---------------------------------------------------------------------------------------------
// Posing the result as the renderer does (fighter_model.cpp skin_vertices).

static std::vector<V3> skin(const ed::RobotLod &lod, const std::vector<de::Transform> &binds, const std::vector<std::string> &bones, const de::AnimBank &bank,
                            const de::AnimSequence &seq, float t) {
	auto pose = bank.pose(seq, t);
	std::vector<de::Transform> m(binds.size());
	for (size_t b = 0; b < binds.size(); ++b) {
		int c = bank.channel(bones[b]);
		m[b] = c >= 0 ? pose[size_t(c)].then(binds[b].inverse()) : de::Transform{};
	}
	std::vector<V3> out(lod.vertices.size());
	for (size_t v = 0; v < lod.vertices.size(); ++v) {
		float acc[3] = {0, 0, 0}, ws = 0;
		for (size_t b = 0; b < binds.size(); ++b) {
			uint8_t w = b < lod.weights.size() && v < lod.weights[b].size() ? lod.weights[b][v] : 0;
			if (!w) continue;
			auto p = m[b].apply(lod.vertices[v]);
			for (int k = 0; k < 3; ++k) acc[k] += p[size_t(k)] * float(w);
			ws += float(w);
		}
		out[v] = ws > 0 ? V3{acc[0] / ws, acc[1] / ws, acc[2] / ws} : lod.vertices[v];
	}
	return out;
}

// The worst edge stretch over every sequence (sampled), and the farthest vertex from the origin.
struct PoseStats {
	float stretch = 0, reach = 0, direction = 1, mesh_direction = 1, base_mesh_direction = 1;
	std::string worst;
};
static float stretch_of(const ed::RobotLod &lod, const std::vector<V3> &p) {
	float worst = 1;
	for (auto &f : lod.faces) {
		for (int k = 0; k < 3; ++k) {
			uint16_t a = f.v[size_t(k)], c = f.v[size_t((k + 1) % 3)];
			float l0 = dist(lod.vertices[a], lod.vertices[c]);
			if (l0 < 0.3f) continue;
			worst = std::max(worst, dist(p[a], p[c]) / l0);
		}
	}
	return worst;
}

// Stretch is measured against the base robot's own in the same sequence (some moves stretch it).
static PoseStats pose_stats(const ed::RobotBuild &b, const ed::RobotRig &rig, const std::set<std::string> &supplied = {}) {
	PoseStats st;
	std::vector<std::string> bones;
	for (auto &bone : rig.skin) bones.push_back(bone.name);
	ed::RobotLod base_lod;
	base_lod.vertices = rig.mesh.vertices;
	std::vector<de::Transform> base_binds;
	for (auto &bone : rig.skin) base_lod.weights.push_back(bone.weights), base_binds.push_back(bone.bind);
	base_lod.faces = rig.mesh.faces;
	const ed::RobotLod &lod = b.lods[0];
	// The limbs point the base robot's way in every pose.
	for (size_t q = 0; q < b.bank.sequences.size(); ++q) {
		if (supplied.count(b.bank.sequences[q].name)) continue;
		bool throws = false;
		for (auto &s : rig.bank.sequences[q].streams) throws |= rig.bank.channels[s.channel].name == "ThrowFollowTarget" && s.pos.size() > 1;
		if (throws) continue;
		for (float t : {0.0f, 0.5f}) {
			auto pn = b.bank.pose(b.bank.sequences[q], t), pb = rig.bank.pose(rig.bank.sequences[q], t);
			// Legs (and an arm holding a throw) are bent by the contact correction; contact_stats checks them.
			for (const char *pair : {"LeftUpperArm:LeftLowerArm", "RightLowerArm:RightHand", "Pelvis:Chest"}) {
				std::string s = pair;
				int a = rig.bank.channel(s.substr(0, s.find(':'))), c = rig.bank.channel(s.substr(s.find(':') + 1));
				if (a < 0 || c < 0) continue;
				V3 dn{pn[size_t(c)].t[0] - pn[size_t(a)].t[0], pn[size_t(c)].t[1] - pn[size_t(a)].t[1], pn[size_t(c)].t[2] - pn[size_t(a)].t[2]};
				V3 db{pb[size_t(c)].t[0] - pb[size_t(a)].t[0], pb[size_t(c)].t[1] - pb[size_t(a)].t[1], pb[size_t(c)].t[2] - pb[size_t(a)].t[2]};
				float d = (dn[0] * db[0] + dn[1] * db[1] + dn[2] * db[2]) / std::max(1e-6f, dist(dn, {0, 0, 0}) * dist(db, {0, 0, 0}));
				st.direction = std::min(st.direction, d);
				// And the mesh a limb carries lies along it (the pelvis's hangs below the chest's joint).
				if (s.rfind("Pelvis", 0) == 0) continue;
				size_t bi = 0;
				for (size_t k2 = 0; k2 < rig.skin.size(); ++k2) bi = rig.skin[k2].name == rig.bank.channels[size_t(a)].name ? k2 : bi;
				auto p = skin(lod, b.binds, bones, b.bank, b.bank.sequences[q], t);
				V3 m{0, 0, 0};
				float n = 0;
				for (size_t v = 0; v < p.size(); ++v) {
					if (lod.weights[bi][v] >= 120) m = {m[0] + p[v][0], m[1] + p[v][1], m[2] + p[v][2]}, n += 1;
				}
				if (n == 0) continue;
				V3 dm{m[0] / n - pn[size_t(a)].t[0], m[1] / n - pn[size_t(a)].t[1], m[2] / n - pn[size_t(a)].t[2]};
				float dd = (dm[0] * dn[0] + dm[1] * dn[1] + dm[2] * dn[2]) / std::max(1e-6f, dist(dm, {0, 0, 0}) * dist(dn, {0, 0, 0}));
				st.mesh_direction = std::min(st.mesh_direction, dd);
				auto pbm = skin(base_lod, base_binds, bones, rig.bank, rig.bank.sequences[q], t);
				m = {0, 0, 0}, n = 0;
				for (size_t v = 0; v < pbm.size(); ++v) {
					if (base_lod.weights[bi].size() > v && base_lod.weights[bi][v] >= 120) m = {m[0] + pbm[v][0], m[1] + pbm[v][1], m[2] + pbm[v][2]}, n += 1;
				}
				if (n == 0) continue;
				dm = {m[0] / n - pb[size_t(a)].t[0], m[1] / n - pb[size_t(a)].t[1], m[2] / n - pb[size_t(a)].t[2]};
				dd = (dm[0] * db[0] + dm[1] * db[1] + dm[2] * db[2]) / std::max(1e-6f, dist(dm, {0, 0, 0}) * dist(db, {0, 0, 0}));
				st.base_mesh_direction = std::min(st.base_mesh_direction, dd);
			}
		}
	}
	for (size_t q = 0; q < b.bank.sequences.size(); ++q) {
		for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
			auto p = skin(lod, b.binds, bones, b.bank, b.bank.sequences[q], t);
			float base = stretch_of(base_lod, skin(base_lod, base_binds, bones, rig.bank, rig.bank.sequences[q], t));
			float r = stretch_of(lod, p) / base;
			if (r > st.stretch) st.stretch = r, st.worst = b.bank.sequences[q].name;
			for (auto &v : p) st.reach = std::max(st.reach, std::sqrt(v[0] * v[0] + v[2] * v[2]));
		}
	}
	return st;
}

// Contacts: where a base foot is down, the new one is on the new floor and moves as the base's does
// (no sliding against the game's base speeds); in a throw the gripping limb keeps its place on the
// target.
struct ContactStats {
	float floor = 0, slide = 0, grip = 0;
	std::string floor_worst, slide_worst;
	size_t samples = 0, grips = 0, off_floor = 0, planted = 0, sliding = 0;
};
static ContactStats contact_stats(const ed::RobotBuild &b, const ed::RobotRig &rig, const std::set<std::string> &supplied = {}) {
	ContactStats st;
	const float tol = rig.height * 0.05f;
	const int tf = rig.bank.channel("ThrowFollowTarget");
	for (size_t q = 0; q < b.bank.sequences.size(); ++q) {
		if (supplied.count(b.bank.sequences[q].name)) continue;
		const int n = 41;
		std::vector<std::vector<de::Transform>> pb(n), pn(n);
		for (int i = 0; i < n; ++i) pb[size_t(i)] = rig.bank.pose(rig.bank.sequences[q], float(i) / (n - 1)), pn[size_t(i)] = b.bank.pose(b.bank.sequences[q], float(i) / (n - 1));
		for (const char *foot : {"LeftFoot", "RightFoot"}) {
			int f = rig.bank.channel(foot);
			size_t bi = 0;
			for (size_t k = 0; k < rig.skin.size(); ++k) bi = rig.skin[k].name == foot ? k : bi;
			if (f < 0) continue;
			const float floor_b = rig.skin[bi].bind.t[1], floor_n = b.binds[bi].t[1];
			for (int i = 0; i < n; ++i) {
				const auto &B = pb[size_t(i)][size_t(f)].t, &N = pn[size_t(i)][size_t(f)].t;
				if (B[1] >= floor_b + tol) continue;
				++st.samples;
				float e = std::fabs((N[1] - floor_n) - (B[1] - floor_b));
				if (e > st.floor) st.floor = e, st.floor_worst = b.bank.sequences[q].name;
				st.off_floor += e > rig.height * 0.02f;
				if (i == 0 || pb[size_t(i - 1)][size_t(f)].t[1] >= floor_b + tol) continue;
				const auto &B0 = pb[size_t(i - 1)][size_t(f)].t, &N0 = pn[size_t(i - 1)][size_t(f)].t;
				// Planted (the base's foot still): the new one must not move.
				if (std::hypot(B[0] - B0[0], B[2] - B0[2]) > 0.05f) continue;
				float slide = std::hypot(N[0] - N0[0], N[2] - N0[2]);
				++st.planted;
				st.sliding += slide > rig.height * 0.01f;
				if (slide > st.slide) st.slide = slide, st.slide_worst = b.bank.sequences[q].name;
			}
		}
		bool throws = false;
		for (auto &s : rig.bank.sequences[q].streams) throws |= int(s.channel) == tf && s.pos.size() > 1;
		if (!throws) continue;
		int grip = -1;
		float best = rig.height * 0.5f;
		for (const char *end : {"LeftFoot", "RightFoot", "LeftHand", "RightHand"}) {
			int e = rig.bank.channel(end);
			for (int i = 0; e >= 0 && i < n; ++i) {
				float d = dist(pb[size_t(i)][size_t(e)].t, pb[size_t(i)][size_t(tf)].t);
				if (d < best) best = d, grip = e;
			}
		}
		for (int i = 0; grip >= 0 && i < n; ++i) {
			float d = dist(pb[size_t(i)][size_t(grip)].t, pb[size_t(i)][size_t(tf)].t);
			if (d > best + rig.height * 0.1f) continue;
			++st.grips;
			st.grip = std::max(st.grip, std::fabs(dist(pn[size_t(i)][size_t(grip)].t, pn[size_t(i)][size_t(tf)].t) - d));
		}
	}
	return st;
}

static std::vector<std::string> bone_names(const ed::RobotRig &rig) {
	std::vector<std::string> out;
	for (auto &b : rig.skin) out.push_back(b.name);
	return out;
}

// ---------------------------------------------------------------------------------------------

static ed::ImportPlan robot_plan(const fs::path &file, ed::BaseData &base, const std::string &robot, const std::string &name) {
	ed::ImportSettings st;
	st.kind = ed::ImportKind::Robot;
	st.name = name;
	ed::ImportPlan plan = ed::analyse(file, st);
	plan.har.base_robot = robot;
	ed::analyse_robot(plan, base);
	ed::revalidate(plan, &base);
	return plan;
}

static void print_issues(const ed::ImportPlan &plan) {
	for (auto &i : plan.issues) {
		if (i.severity != ed::Issue::Ok) fprintf(stderr, "  %s: %s\n", i.severity == ed::Issue::Error ? "error" : "warning", i.text.c_str());
	}
}

static void print_mapping(const ed::ImportPlan &plan) {
	int name = 0, preset = 0, guess = 0, missing = 0;
	for (auto &b : plan.har.bones) {
		switch (b.source) {
		case ed::MapSource::Name: ++name; break;
		case ed::MapSource::Preset: ++preset; break;
		case ed::MapSource::Guess: ++guess; break;
		case ed::MapSource::Manual: break;
		default: missing += b.required ? 1 : 0;
		}
	}
	printf("  mapping: %d by name, %d by preset%s, %d guessed, %d missing; mirror %d, turn %d\n", name, preset,
	       plan.har.detected.empty() ? "" : (" (" + plan.har.detected + ")").c_str(), guess, missing, plan.har.mirror, plan.har.turn);
	for (auto &b : plan.har.bones) {
		if (getenv("HAR_TEST_VERBOSE")) printf("    %-14s <- %s (%s)\n", b.bone.c_str(), b.joint >= 0 ? plan.scene.nodes[size_t(b.joint)].name.c_str() : "-", ed::map_source_name(b.source));
	}
}

// The add-on as the engine reads it: the HAR's description, model, skin and bank.
static void check_engine_reads(const fs::path &data, const fs::path &file, const std::string &name, size_t lod0_faces, size_t paint_jobs = 1) {
	set_env("OPENBG_ADDONS", file.string());
	std::set<std::string> keep{file.stem().string(), "Jaguar", "JaguarModel1", "Katana", "KatanaModel1", "SharedObjects", "Fighter"};
	de::World w(data, [&](const std::string &s) { return keep.count(s) > 0; });
	set_env("OPENBG_ADDONS", "");
	const de::ObjectsFile *f = w.file(file.stem().string());
	CHECK(f);
	if (!f) return;
	de::ObjectRef desc;
	for (uint32_t i = 0; i < f->objects().size(); ++i) {
		if (f->objects()[i].class_path == "Description\\Creature\\Fighter" && f->objects()[i].name == name) desc = {f, i};
	}
	CHECK(desc);
	auto models = game::fighter_models(w, desc);
	CHECK(models.size() == paint_jobs);
	size_t defaults = 0;
	for (auto &m : models) defaults += game::read_description(w, m).flag_d8;
	CHECK(defaults <= 1);
	if (paint_jobs > 1) {
		de::ObjectRef large = game::read_description(w, desc).large;
		CHECK(large && large.file == f && large.info().class_path == "ControlCreator");
	}
	// The model's biggest mesh with its weights, and the fighter's bank.
	const de::ObjectInfo *model = nullptr, *bank = nullptr, *deform = nullptr;
	for (auto &o : f->objects()) {
		if (o.class_path == "FighterModel" && o.name == name + " Model") model = &o;
		if (!bank && o.class_path == "AnimationSequenceGroup") bank = &o;
		if (!deform && o.class_path == "ActorDeformInfo") deform = &o;
	}
	CHECK(model && bank && deform);
	if (!model || !bank || !deform) return;
	uint32_t mesh = 0;
	size_t faces = 0;
	for (uint32_t c : de::ObjectsFile::children(f->object_data(*model))) {
		if (f->objects()[c].class_path != "Mesh\\Standard") continue;
		de::Mesh m = de::read_mesh(*f, f->objects()[c]);
		if (m.faces.size() > faces) faces = m.faces.size(), mesh = c;
	}
	CHECK(faces == lod0_faces);
	auto skin = de::read_skin(*f, *deform, *f, mesh);
	de::Mesh m = de::read_mesh(*f, f->objects()[mesh]);
	size_t weighted = 0;
	for (size_t v = 0; v < m.vertices.size(); ++v) {
		int sum = 0;
		for (auto &b : skin) sum += v < b.weights.size() ? b.weights[v] : 0;
		weighted += sum > 0;
	}
	CHECK(weighted == m.vertices.size());
	de::AnimBank bk = de::read_anim_bank(*f, *bank);
	CHECK(bk.sequences.size() > 50);
	auto mats = de::read_materials(w, {f, mesh});
	CHECK(!mats.empty() && mats[0].base() && mats[0].base()->uvs.size() == faces);
}

static void test_round_trip(const fs::path &data, ed::BaseData &base, const fs::path &keep) {
	fs::path dir = scratch();
	de::World w(data, [](const std::string &s) { return s == "Jaguar" || s == "JaguarModel1" || s == "SharedObjects"; });
	const de::ObjectsFile *mf = w.file("JaguarModel1");
	de::ObjectRef model;
	for (uint32_t i = 0; i < mf->objects().size() && !model; ++i) {
		if (mf->objects()[i].class_path == "FighterModel" && mf->objects()[i].name == "Paint1") model = {mf, i};
	}
	gltf::write(ed::export_object(w, model), dir / "jaguar.glb");
	ed::ImportPlan plan = robot_plan(dir / "jaguar.glb", base, "Jaguar", "Jaguar Again");
	printf("round trip (Jaguar export -> import onto Jaguar):\n");
	print_mapping(plan);
	print_issues(plan);
	CHECK(!plan.errors());
	CHECK(!plan.har.mirror && plan.har.turn == 0);
	for (auto &b : plan.har.bones) CHECK(!b.required || b.source == ed::MapSource::Name);
	auto rig = plan.har.rig;
	ed::RobotBuild b = ed::build_robot(plan);
	// Same skin, binds, rest and sequences as the base.
	CHECK(b.lods[0].vertices.size() == rig->mesh.vertices.size());
	CHECK(b.lods[0].faces.size() == rig->mesh.faces.size());
	float vmax = 0, bmax = 0, rmax = 0;
	for (size_t v = 0; v < std::min(b.lods[0].vertices.size(), rig->mesh.vertices.size()); ++v) vmax = std::max(vmax, dist(b.lods[0].vertices[v], rig->mesh.vertices[v]));
	for (size_t i = 0; i < b.binds.size(); ++i) {
		bmax = std::max(bmax, dist(b.binds[i].t, rig->skin[i].bind.t));
		for (int k = 0; k < 9; ++k) bmax = std::max(bmax, std::fabs(b.binds[i].r[size_t(k)] - rig->skin[i].bind.r[size_t(k)]));
	}
	std::string rworst;
	for (size_t c = 0; c < rig->bank.channels.size(); ++c) {
		float d = dist(b.bank.channels[c].rest_pos, rig->bank.channels[c].rest_pos);
		if (d > rmax) rmax = d, rworst = rig->bank.channels[c].name;
	}
	printf("  LOD0 %zu vertices (max moved %.4f), binds max diff %.5f, channel rest max diff %.5f (%s), scale %.4f\n", b.lods[0].vertices.size(), vmax, bmax, rmax, rworst.c_str(), b.scale);
	CHECK(vmax < 0.01f && bmax < 1e-3f && rmax < 0.01f && std::fabs(b.scale - 1) < 1e-3f);
	// Posed the same: every sequence at a few times against the base skin.
	ed::RobotLod base_lod;
	base_lod.vertices = rig->mesh.vertices;
	std::vector<de::Transform> base_binds;
	for (auto &bone : rig->skin) base_lod.weights.push_back(bone.weights), base_binds.push_back(bone.bind);
	float worst = 0;
	auto names = bone_names(*rig);
	for (size_t q = 0; q < rig->bank.sequences.size(); q += 7) {
		for (float t : {0.0f, 0.5f, 1.0f}) {
			auto a = skin(base_lod, base_binds, names, rig->bank, rig->bank.sequences[q], t);
			auto c = skin(b.lods[0], b.binds, names, b.bank, b.bank.sequences[q], t);
			for (size_t v = 0; v < a.size() && v < c.size(); ++v) worst = std::max(worst, dist(a[v], c[v]));
		}
	}
	printf("  posed against the base: max %.3f units apart\n", worst);
	CHECK(worst < 0.6f);
	// Imported, saved, read back.
	auto project = ed::Project::create(base, dir / "HarRoundTrip.DEObjects");
	ed::ImportResult r = ed::import_robot(*project, plan);
	auto hars = ed::addon_hars(*project);
	CHECK(hars.size() == 1);
	ed::HarObjects o = ed::har_objects(*project, r.root);
	ed::HarData hd = ed::read_har(*project, o);
	CHECK(hd.name == "Jaguar Again" && hd.spheres.size() == rig->spheres.size());
	for (auto &i : ed::validate(*project)) {
		if (i.severity == ed::Issue::Error) fprintf(stderr, "  validation: %s\n", i.text.c_str());
		CHECK(i.severity != ed::Issue::Error);
	}
	project->save();
	check_engine_reads(data, dir / "HarRoundTrip.DEObjects", "Jaguar Again", b.lods[0].faces.size());
	if (!keep.empty()) fs::copy_file(dir / "HarRoundTrip.DEObjects", keep / "HarRoundTrip.DEObjects", fs::copy_options::overwrite_existing);
	fs::remove_all(dir);
	printf("  ok\n");
}

static void test_generated(const fs::path &data, ed::BaseData &base, Naming naming, const char *label, const std::string &robot, const fs::path &keep) {
	fs::path dir = scratch();
	gltf::write(humanoid(naming, naming == Naming::Mixamo ? 1.0f : 1.3f, naming == Naming::Mixamo ? 1.0f : 0.85f, true), dir / "rig.glb");
	std::string name = naming == Naming::Mixamo ? "Mixamo Bot" : "Box Bot";
	ed::ImportPlan plan = robot_plan(dir / "rig.glb", base, robot, name);
	printf("%s onto %s:\n", label, robot.c_str());
	print_mapping(plan);
	print_issues(plan);
	CHECK(!plan.errors());
	for (auto &b : plan.har.bones) {
		if (!b.required) continue;
		CHECK(b.joint >= 0);
		if (b.joint < 0) continue;
		std::string jn = plan.scene.nodes[size_t(b.joint)].name;
		// The expected joint for each rig bone, by the generator's layout.
		static const std::map<std::string, std::string> mixamo = {
		    {"Pelvis", "Hips"},       {"Chest", "Spine2"},         {"Head", "Head"},           {"LeftUpperArm", "LeftArm"},  {"LeftLowerArm", "LeftForeArm"},
		    {"LeftHand", "LeftHand"}, {"RightUpperArm", "RightArm"}, {"RightLowerArm", "RightForeArm"}, {"RightHand", "RightHand"}, {"LeftUpperLeg", "LeftUpLeg"},
		    {"LeftLowerLeg", "LeftLeg"}, {"LeftFoot", "LeftFoot"}, {"RightUpperLeg", "RightUpLeg"}, {"RightLowerLeg", "RightLeg"}, {"RightFoot", "RightFoot"}};
		static const std::map<std::string, std::string> numbered = {
		    {"Pelvis", "joint0"},        {"Chest", "joint3"},          {"Head", "joint5"},          {"LeftUpperArm", "joint8"},  {"LeftLowerArm", "joint9"},
		    {"LeftHand", "joint10"},     {"RightUpperArm", "joint19"}, {"RightLowerArm", "joint20"}, {"RightHand", "joint21"}, {"LeftUpperLeg", "joint13"},
		    {"LeftLowerLeg", "joint14"}, {"LeftFoot", "joint15"},      {"RightUpperLeg", "joint24"}, {"RightLowerLeg", "joint25"}, {"RightFoot", "joint26"}};
		const auto &want = naming == Naming::Mixamo ? mixamo : numbered;
		auto it = want.find(b.bone);
		if (it == want.end()) continue;
		bool ok = naming == Naming::Mixamo ? jn == "mixamorig:" + it->second : jn == it->second;
		if (!ok) fprintf(stderr, "  %s mapped to %s, expected %s\n", b.bone.c_str(), jn.c_str(), it->second.c_str());
		CHECK(ok);
	}
	CHECK(plan.har.mirror);  // the glTF convention's left is the game's mirror image
	size_t new_spheres = 0;
	if (naming == Naming::Unknown) {
		CHECK(plan.har.spheres.size() == 3);
		for (auto &sp : plan.har.spheres) {
			if (sp.sphere == "Antenna") CHECK(!sp.known && !sp.note.empty());
			else {
				CHECK(sp.known && sp.copies >= 0 && !sp.bone.empty());
				++new_spheres;
			}
		}
	}
	int supplied = 0;
	for (auto &m : plan.har.sequences) supplied += m.animation >= 0;
	// A base without the file's sequence name: the animation is named by hand (the Animations page).
	std::string manual;
	if (supplied == 0) {
		for (auto &m : plan.har.sequences) {
			if (manual.empty() && m.sequence.find("Win") != std::string::npos) m.animation = 0, m.source = ed::MapSource::Manual, manual = m.sequence, ++supplied;
		}
		ed::revalidate(plan, &base);
		printf("  Winning1 named %s by hand\n", manual.c_str());
	}
	CHECK(supplied == 1);
	ed::RobotBuild b = ed::build_robot(plan);
	std::set<std::string> from_file;
	for (auto &m : plan.har.sequences) {
		if (m.animation >= 0) from_file.insert(m.sequence);
	}
	PoseStats ps = pose_stats(b, *plan.har.rig, from_file);
	printf("  scale %.2f, LODs %zu / %zu / %zu triangles, %zu sequences (%zu from the file); worst edge stretch %.2f (%s), reach %.1f units, limbs within %.0f deg of the base, their meshes within %.0f deg of the limbs (base %.0f)\n", b.scale, b.lods[0].faces.size(),
	       b.lods.size() > 1 ? b.lods[1].faces.size() : 0, b.lods.size() > 2 ? b.lods[2].faces.size() : 0, b.bank.sequences.size(), b.supplied, ps.stretch, ps.worst.c_str(), ps.reach, std::acos(std::clamp(ps.direction, -1.0f, 1.0f)) * 57.3f, std::acos(std::clamp(ps.mesh_direction, -1.0f, 1.0f)) * 57.3f, std::acos(std::clamp(ps.base_mesh_direction, -1.0f, 1.0f)) * 57.3f);
	ContactStats cs = contact_stats(b, *plan.har.rig, from_file);
	printf("  contacts: %zu sequences corrected; feet down in %zu samples, %zu off the floor (worst %.2f, %s, out of reach); planted in %zu, %zu sliding (worst %.2f, %s); throw grip %zu samples, off by %.2f\n", b.contacts,
	       cs.samples, cs.off_floor, cs.floor, cs.floor_worst.c_str(), cs.planted, cs.sliding, cs.slide, cs.slide_worst.c_str(), cs.grips, cs.grip);
	CHECK(b.contacts > 0);
	CHECK(cs.off_floor * 10 < cs.samples);  // out of reach: thrown, falling, legs at full stretch
	CHECK(cs.sliding * 50 < cs.planted);
	CHECK(cs.grip < plan.har.rig->height * 0.05f);
	CHECK(b.supplied == 1);
	CHECK(ps.stretch < 2.0f);
	CHECK(ps.direction > 0.99f);
	CHECK(std::acos(std::clamp(ps.mesh_direction, -1.0f, 1.0f)) <= std::acos(std::clamp(ps.base_mesh_direction, -1.0f, 1.0f)) + 0.5f);
	CHECK(ps.reach < 60);
	auto project = ed::Project::create(base, dir / "HarGenerated.DEObjects");
	ed::ImportResult r = ed::import_robot(*project, plan);
	CHECK(ed::addon_hars(*project).size() == 1);
	{
		ed::HarData hd = ed::read_har(*project, ed::har_objects(*project, r.root));
		CHECK(hd.spheres.size() == plan.har.rig->spheres.size() + new_spheres);
		size_t found = 0;
		for (auto &sp : hd.spheres) found += (sp.name == "GetHitAffectorForearm" && sp.bone == "LeftLowerArm") || (sp.name == "RightPunchMediumHitDetector3" && sp.bone == "RightHand");
		CHECK(found == new_spheres);
	}
	for (auto &i : ed::validate(*project)) {
		if (i.severity == ed::Issue::Error) fprintf(stderr, "  validation: %s\n", i.text.c_str());
		CHECK(i.severity != ed::Issue::Error);
	}
	// The mapping survives a re-import through the sidecar JSON.
	json::Value saved = ed::robot_mapping_json(plan);
	ed::ImportPlan again = robot_plan(dir / "rig.glb", base, robot, name);
	ed::apply_robot_mapping(again, saved);
	CHECK(again.har.mirror == plan.har.mirror && again.har.turn == plan.har.turn);
	for (auto &m : again.har.sequences) {
		if (!manual.empty() && m.sequence == manual) CHECK(m.animation == 0 && m.source == ed::MapSource::Manual);
	}
	// A second paint job: a copy of the imported model with its own colours.
	{
		ed::HarObjects o = ed::har_objects(*project, r.root);
		uint32_t job = ed::create_paint_job(*project, o, "Night");
		o = ed::har_objects(*project, r.root);
		CHECK(o.model_descriptions.size() == 2);
		int first = ed::paint_job_model(*project, uint32_t(o.model_descriptions[0])), second = ed::paint_job_model(*project, job);
		CHECK(first >= 0 && second >= 0 && first != second);
		auto images = ed::paint_images(*project, job);
		bool colours = false;
		for (auto &img : images) {
			if (img.name != "RGBStatic") continue;
			int w = 0, h = 0, type = 0;
			ed::image_header(project->file().objects[size_t(img.object)], &type, &w, &h);
			ed::set_image_pixels(*project, uint32_t(img.object), w, h, std::vector<uint8_t>(size_t(w) * size_t(h) * 4, 40), "night.png");
			colours = true;
		}
		CHECK(colours || plan.har.paint != ed::HarPlan::Paint::Colours);
		// Choose Creature's panel: the base robot's until changed, then the add-on's own copy.
		ed::HarPresentation before = ed::read_presentation(*project, o);
		CHECK(before.found && !before.own && !before.text.empty() && before.width == 512 && before.height == 128);
		ed::write_presentation(*project, o, "BOX BOT", "A robot of boxes.");
		ed::set_presentation_picture(*project, o, 4, 1, std::vector<uint8_t>(16, 200), "test.png");
		ed::HarPresentation after = ed::read_presentation(*project, o);
		CHECK(after.own && after.title == "BOX BOT" && after.text == "A robot of boxes." && after.picture >= 0 && after.width == 512);
	}
	project->save();
	check_engine_reads(data, dir / "HarGenerated.DEObjects", name, b.lods[0].faces.size(), 2);
	if (!keep.empty()) {
		std::string stem = naming == Naming::Mixamo ? "HarMixamo" : "HarBoxBot";
		fs::copy_file(dir / "HarGenerated.DEObjects", keep / (stem + ".DEObjects"), fs::copy_options::overwrite_existing);
		fs::copy_file(dir / "rig.glb", keep / (stem + ".glb"), fs::copy_options::overwrite_existing);
	}
	(void)r;
	fs::remove_all(dir);
	printf("  ok\n");
}

int main(int argc, char **argv) {
	if (argc > 2 && !strcmp(argv[1], "--samples")) {
		fs::path dir = argv[2];
		fs::create_directories(dir);
		gltf::write(humanoid(Naming::Unknown, 1.3f, 0.85f, true), dir / "humanoid.glb");
		gltf::write(humanoid(Naming::Mixamo, 1.0f, 1.0f, true), dir / "mixamo.glb");
		printf("wrote %s and %s\n", (dir / "humanoid.glb").string().c_str(), (dir / "mixamo.glb").string().c_str());
		return 0;
	}
	if (argc < 2) {
		fprintf(stderr, "usage: %s <Data dir> [--keep <dir>] | --samples <dir>\n", argv[0]);
		return 2;
	}
	fs::path data = argv[1];
	if (!fs::exists(data)) {
		printf("no game data at %s: skipping\n", data.string().c_str());
		return 0;
	}
	fs::path keep;
	if (argc > 3 && !strcmp(argv[2], "--keep")) keep = argv[3], fs::create_directories(keep);
	ed::BaseData base(data);
	test_round_trip(data, base, keep);
	test_generated(data, base, Naming::Unknown, "generated humanoid (unknown joint names, long arms, short legs)", "Jaguar", keep);
	test_generated(data, base, Naming::Mixamo, "Mixamo-named rig (centimetre armature)", "Katana", keep);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all HAR import checks passed\n");
	return 0;
}
