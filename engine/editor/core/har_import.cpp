#include "har_import.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>

#include "arena.h"
#include "content.h"
#include "de/contents.h"
#include "har.h"
#include "image_io.h"
#include "mesh_simplify.h"
#include "model_build.h"
#include "model_export.h"

namespace ed {

using V3 = std::array<float, 3>;
using UV3 = std::array<std::array<float, 2>, 3>;

namespace {

V3 add(const V3 &a, const V3 &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 sub(const V3 &a, const V3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 mul(const V3 &a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const V3 &a, const V3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3 &a, const V3 &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float length(const V3 &a) { return std::sqrt(dot(a, a)); }
V3 normalize(const V3 &a) {
	float l = length(a);
	return l > 1e-9f ? mul(a, 1 / l) : V3{0, 0, 0};
}
V3 flat(const V3 &a) { return {a[0], 0, a[2]}; }

std::string lower(std::string s) {
	for (auto &c : s) c = char(tolower(uint8_t(c)));
	return s;
}

// Matches transform_from: the rows of t.r are the columns of the rotation matrix.
de::Quat quat_of(const de::Transform &t) {
	const float m00 = t.r[0], m01 = t.r[3], m02 = t.r[6], m10 = t.r[1], m11 = t.r[4], m12 = t.r[7], m20 = t.r[2], m21 = t.r[5], m22 = t.r[8];
	de::Quat q;
	float tr = m00 + m11 + m22;
	if (tr > 0) {
		float s = std::sqrt(tr + 1) * 2;
		q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s / 4};
	} else if (m00 > m11 && m00 > m22) {
		float s = std::sqrt(1 + m00 - m11 - m22) * 2;
		q = {s / 4, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
	} else if (m11 > m22) {
		float s = std::sqrt(1 + m11 - m00 - m22) * 2;
		q = {(m01 + m10) / s, s / 4, (m12 + m21) / s, (m02 - m20) / s};
	} else {
		float s = std::sqrt(1 + m22 - m00 - m11) * 2;
		q = {(m02 + m20) / s, (m12 + m21) / s, s / 4, (m10 - m01) / s};
	}
	float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	for (auto &x : q) x /= l > 0 ? l : 1;
	return q;
}

// Some channels' rest quaternions are not unit (their transforms scale), so no transpose here.
de::Transform affine_inverse(const de::Transform &t) {
	const auto &r = t.r;
	float det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) + r[2] * (r[3] * r[7] - r[4] * r[6]);
	if (std::fabs(det) < 1e-12f) return t.inverse();
	de::Transform o;
	o.r = {(r[4] * r[8] - r[5] * r[7]) / det, (r[2] * r[7] - r[1] * r[8]) / det, (r[1] * r[5] - r[2] * r[4]) / det,
	       (r[5] * r[6] - r[3] * r[8]) / det, (r[0] * r[8] - r[2] * r[6]) / det, (r[2] * r[3] - r[0] * r[5]) / det,
	       (r[3] * r[7] - r[4] * r[6]) / det, (r[1] * r[6] - r[0] * r[7]) / det, (r[0] * r[4] - r[1] * r[3]) / det};
	auto p = o.rotate(t.t);
	o.t = {-p[0], -p[1], -p[2]};
	return o;
}

de::Transform rotation_only(de::Transform t) {
	t.t = {0, 0, 0};
	return t;
}

// An orthonormal frame (rows) from a primary direction and a secondary one across it.
de::Transform frame(const V3 &primary, const V3 &secondary) {
	V3 x = normalize(primary);
	V3 y = sub(secondary, mul(x, dot(secondary, x)));
	y = normalize(y);
	if (length(y) < 0.5f) {
		V3 any = std::fabs(x[1]) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0};
		y = normalize(sub(any, mul(x, dot(any, x))));
	}
	V3 z = cross(x, y);
	de::Transform t;
	for (int k = 0; k < 3; ++k) t.r[size_t(k)] = x[size_t(k)], t.r[size_t(3 + k)] = y[size_t(k)], t.r[size_t(6 + k)] = z[size_t(k)];
	return t;
}

// The shortest rotation taking direction a to b.
de::Transform rotation_between(const V3 &a_in, const V3 &b_in) {
	V3 a = normalize(a_in), b = normalize(b_in);
	if (length(a) < 0.5f || length(b) < 0.5f) return {};
	float d = dot(a, b);
	V3 c = cross(a, b);
	de::Quat q;
	if (d < -0.9999f) {
		V3 axis = normalize(cross(a, std::fabs(a[0]) < 0.9f ? V3{1, 0, 0} : V3{0, 1, 0}));
		q = {axis[0], axis[1], axis[2], 0};
	} else {
		q = {c[0], c[1], c[2], 1 + d};
		float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
		for (auto &x : q) x /= l;
	}
	return de::transform_from({0, 0, 0}, q);
}

gltf::Mat4 up_fix(bool z_up) { return z_up ? gltf::Mat4{1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1} : gltf::identity(); }

gltf::Mat4 rotate_y(int degrees) {
	float a = float(degrees) * 3.14159265f / 180, c = std::cos(a), s = std::sin(a);
	if (degrees % 90 == 0) c = std::round(c), s = std::round(s);
	return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
}

float det3(const gltf::Mat4 &m) { return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]); }

// ---------------------------------------------------------------------------------------------
// Names

struct NameKey {
	int side = 0;  // 1 left, 2 right
	std::string core;
};

// "mixamorig:LeftUpLeg", "DEF-upper_arm.L", "Bip01 L Thigh", "upperarm_l": the side and the rest.
NameKey name_key(const std::string &in) {
	std::string s = in;
	if (auto p = s.find_last_of(":|"); p != std::string::npos && p + 1 < s.size()) s = s.substr(p + 1);
	std::vector<std::string> tok;
	std::string cur;
	auto flush = [&] {
		if (!cur.empty()) tok.push_back(cur);
		cur.clear();
	};
	for (size_t i = 0; i < s.size(); ++i) {
		unsigned char c = uint8_t(s[i]);
		if (!isalnum(c)) {
			flush();
			continue;
		}
		if (!cur.empty() && isupper(c) && i > 0 && islower(uint8_t(s[i - 1]))) flush();
		cur += char(tolower(c));
	}
	flush();
	static const std::set<std::string> prefixes = {"def", "org", "mch", "bip01", "bip001", "bip", "bn", "jnt", "sk", "rig", "mixamorig", "armature", "skeleton"};
	static const std::set<std::string> suffixes = {"jnt", "joint", "bn", "def", "bind"};
	while (tok.size() > 1 && prefixes.count(tok.front())) tok.erase(tok.begin());
	while (tok.size() > 1 && suffixes.count(tok.back())) tok.pop_back();
	NameKey k;
	for (size_t i = 0; i < tok.size(); ++i) {
		const std::string &t = tok[i];
		int side = t == "l" || t == "left" ? 1 : t == "r" || t == "right" ? 2 : 0;
		if (side && tok.size() > 1) {
			k.side = side;
			tok.erase(tok.begin() + long(i));
			break;
		}
	}
	for (auto &t : tok) k.core += t;
	return k;
}

std::string anim_key(const std::string &name) {
	std::string s = name;
	if (auto p = s.find_last_of('|'); p != std::string::npos) s = s.substr(p + 1);
	std::string out;
	for (char c : s) {
		if (isalnum(uint8_t(c))) out += char(tolower(uint8_t(c)));
	}
	return out;
}

struct Preset {
	const char *name;
	std::vector<std::pair<std::string, std::vector<std::string>>> bones;  // rig core -> file cores, best first
};

const std::vector<Preset> &presets() {
	static const std::vector<Preset> p = {
	    {"Mixamo",
	     {{"pelvis", {"hips"}}, {"chest", {"spine2"}}, {"head", {"head"}}, {"upperarm", {"arm"}}, {"lowerarm", {"forearm"}}, {"hand", {"hand"}}, {"upperleg", {"upleg"}}, {"lowerleg", {"leg"}}, {"foot", {"foot"}}}},
	    {"Blender Rigify",
	     {{"pelvis", {"spine", "hips"}},
	      {"chest", {"spine003", "chest"}},
	      {"head", {"spine006", "head"}},
	      {"upperarm", {"upperarm"}},
	      {"lowerarm", {"forearm"}},
	      {"hand", {"hand"}},
	      {"upperleg", {"thigh"}},
	      {"lowerleg", {"shin"}},
	      {"foot", {"foot"}}}},
	    {"Unreal",
	     {{"pelvis", {"pelvis"}},
	      {"chest", {"spine05", "spine03"}},
	      {"head", {"head"}},
	      {"upperarm", {"upperarm"}},
	      {"lowerarm", {"lowerarm"}},
	      {"hand", {"hand"}},
	      {"upperleg", {"thigh"}},
	      {"lowerleg", {"calf"}},
	      {"foot", {"foot"}}}},
	    {"3ds Max Biped",
	     {{"pelvis", {"pelvis"}},
	      {"chest", {"spine3", "spine2", "spine1"}},
	      {"head", {"head"}},
	      {"upperarm", {"upperarm"}},
	      {"lowerarm", {"forearm"}},
	      {"hand", {"hand"}},
	      {"upperleg", {"thigh"}},
	      {"lowerleg", {"calf"}},
	      {"foot", {"foot"}}}},
	    {"Generic",
	     {{"pelvis", {"pelvis", "hips", "hip", "root"}},
	      {"chest", {"chest", "upperchest", "spine3", "spine2", "torso", "spine1", "spine"}},
	      {"head", {"head"}},
	      {"upperarm", {"upperarm", "uparm", "arm"}},
	      {"lowerarm", {"lowerarm", "forearm", "elbow"}},
	      {"hand", {"hand", "wrist"}},
	      {"upperleg", {"upperleg", "thigh", "upleg"}},
	      {"lowerleg", {"lowerleg", "shin", "calf", "knee", "leg"}},
	      {"foot", {"foot", "ankle"}}}},
	};
	return p;
}

// ---------------------------------------------------------------------------------------------
// Reading the base robot

std::string read_text(de::Contents &c) {
	auto b = c.block();
	size_t n = b.size();
	while (n && b[n - 1] == 0) --n;
	return std::string(b.begin(), b.begin() + long(n));
}

// The bank's channel BOOLs and whatever follows the channels in its chunk, which read_anim_bank skips.
void bank_extras(const de::ObjectsFile &f, const de::ObjectInfo &o, std::vector<std::array<uint8_t, 2>> *flags) {
	de::Contents c(f.object_data(o).contents);
	de::Chunk ch = de::begin_chunk(c);
	read_text(c);
	int32_t ns = c.i32();
	for (int32_t s = 0; s < ns; ++s) {
		read_text(c);
		int32_t nst = c.i32();
		for (int32_t k = 0; k < nst; ++k) {
			c.u16();
			de::Chunk sc = de::begin_chunk(c);
			c.seek(sc.end);
		}
	}
	int32_t nc = c.i32();
	for (int32_t k = 0; k < nc; ++k) {
		read_text(c);
		uint8_t a = c.u8(), b = c.u8();
		flags->push_back({a, b});
		c.u16();
		for (int i = 0; i < 7; ++i) c.f32();
	}
	(void)ch;
}

const de::ObjectInfo *child_of_class(const de::ObjectsFile &f, uint32_t parent, const std::string &cls) {
	for (uint32_t c : de::ObjectsFile::children(f.object_data(f.objects()[parent]))) {
		if (f.objects()[c].class_path == cls) return &f.objects()[c];
	}
	return nullptr;
}

bool sphere_radii(const de::ObjectsFile &f, const de::ObjectInfo &o, float *hotspot, float *falloff) {
	try {
		de::Contents c(f.object_data(o).contents);
		for (int k = 0; k < 2; ++k) c.seek(de::begin_chunk(c).end);
		de::Chunk ch = de::begin_chunk(c);
		if (ch.version != 2) return false;
		*hotspot = c.f32();
		*falloff = c.f32();
		return true;
	} catch (const std::exception &) {
		return false;
	}
}

// Faces and vertices of a Mesh\Standard from its counts chunk.
std::pair<size_t, size_t> mesh_counts(const deo::Object &o) {
	Chunks c = split_chunks(o.contents);
	if (c.chunks.size() < 2 || c.chunks[1].body.size() < 8) return {0, 0};
	uint32_t v, f;
	memcpy(&v, c.chunks[1].body.data(), 4);
	memcpy(&f, c.chunks[1].body.data() + 4, 4);
	return {f, v};
}

bool contains(const Bytes &hay, const uint8_t *needle, size_t n) {
	if (hay.size() < n) return false;
	return std::search(hay.begin(), hay.end(), needle, needle + n) != hay.end();
}

std::shared_ptr<RobotRig> read_rig(const BaseData &base, const std::string &robot) {
	auto rig = std::make_shared<RobotRig>();
	rig->robot = robot;
	const BaseObject *desc = nullptr, *actor = nullptr;
	for (auto *x : base.of_class("Description\\Creature\\Fighter")) {
		if (x->name != robot) continue;
		const BaseObject *a = base.by_guid(description_resource(base.model(x->file).objects[x->index]));
		if (a) desc = x, actor = a;
	}
	if (!desc) throw std::runtime_error("no base robot named " + robot);
	rig->fighter_stem = base.files()[actor->file].stem;
	// The default model: the FighterModel the description names (the first one otherwise).
	const deo::Object &dobj = base.model(desc->file).objects[desc->index];
	const BaseObject *model = nullptr;
	for (auto *m : base.of_class("FighterModel")) {
		if (contains(dobj.contents, m->guid.b, 16) || contains(dobj.references, m->guid.b, 16)) {
			model = m;
			break;
		}
	}
	if (!model) {
		for (auto *md : base.of_class("Description\\FighterModelDescription")) {
			const deo::Object &mo = base.model(md->file).objects[md->index];
			if (!contains(mo.contents, desc->guid.b, 16)) continue;
			model = base.by_guid(description_resource(mo));
			if (model && model->class_path == "FighterModel") break;
			model = nullptr;
		}
	}
	if (!model) throw std::runtime_error(robot + " has no FighterModel");
	rig->model_stem = base.files()[model->file].stem;
	rig->model_name = model->name;
	std::set<std::string> keep{rig->fighter_stem, rig->model_stem};
	de::World w(base.dir(), [&](const std::string &s) { return keep.count(s) > 0; });
	const de::ObjectsFile *ff = w.file(rig->fighter_stem), *mf = w.file(rig->model_stem);
	if (!ff || !mf) throw std::runtime_error("cannot read " + rig->fighter_stem + " / " + rig->model_stem);
	const de::ObjectInfo *bank = nullptr;
	for (auto &o : ff->objects()) {
		if (o.class_path == "AnimationSequenceGroup") {
			bank = &o;
			break;
		}
	}
	if (!bank) throw std::runtime_error(robot + " has no animation bank");
	rig->bank = de::read_anim_bank(*ff, *bank);
	rig->bank_name = bank->name;
	bank_extras(*ff, *bank, &rig->channel_flags);
	if (rig->channel_flags.size() != rig->bank.channels.size()) throw std::runtime_error(robot + "'s animation bank did not read back");
	uint32_t actor_index = actor->index;
	const de::ObjectInfo *deform = child_of_class(*ff, actor_index, "ActorDeformInfo");
	if (!deform) {
		for (auto &o : ff->objects()) {
			if (o.class_path == "ActorDeformInfo") {
				deform = &o;
				break;
			}
		}
	}
	if (!deform) throw std::runtime_error(robot + " has no deform info");
	rig->deform_name = deform->name;
	// LODs: the model's meshes, most faces first.
	uint32_t model_index = model->index;
	std::vector<std::pair<size_t, uint32_t>> meshes;
	for (uint32_t c : de::ObjectsFile::children(mf->object_data(mf->objects()[model_index]))) {
		if (mf->objects()[c].class_path != "Mesh\\Standard") continue;
		de::Mesh m = de::read_mesh(*mf, mf->objects()[c]);
		meshes.push_back({m.faces.size(), c});
		rig->lods.push_back({mf->objects()[c].name, m.faces.size(), m.vertices.size()});
	}
	if (meshes.empty()) throw std::runtime_error(robot + "'s model has no meshes");
	std::sort(rig->lods.begin(), rig->lods.end(), [](auto &a, auto &b) { return a.faces > b.faces; });
	std::sort(meshes.begin(), meshes.end(), [](auto &a, auto &b) { return a.first > b.first; });
	rig->mesh = de::read_mesh(*mf, mf->objects()[meshes[0].second]);
	rig->skin = de::read_skin(*ff, *deform, *mf, meshes[0].second);
	// Each channel's bind: the deformer's, else its rest under its parent's bind (Body at the origin).
	size_t nc = rig->bank.channels.size();
	rig->bind.assign(nc, {});
	rig->deformer.assign(nc, -1);
	for (size_t b = 0; b < rig->skin.size(); ++b) {
		int c = rig->bank.channel(rig->skin[b].name);
		if (c >= 0) rig->deformer[size_t(c)] = int(b);
	}
	const int body = rig->bank.channel("Body");
	std::vector<bool> done(nc, false);
	for (size_t pass = 0; pass < nc; ++pass) {
		bool progress = false;
		for (size_t c = 0; c < nc; ++c) {
			if (done[c]) continue;
			int p = int(c) == body ? -1 : rig->bank.channels[c].parent;
			if (p >= 0 && (size_t(p) >= nc || !done[size_t(p)])) {
				if (size_t(p) >= nc) p = -1;
				else continue;
			}
			if (rig->deformer[c] >= 0) rig->bind[c] = rig->skin[size_t(rig->deformer[c])].bind;
			else if (int(c) == body) rig->bind[c] = {};
			else {
				de::Transform local = de::transform_from(rig->bank.channels[c].rest_pos, rig->bank.channels[c].rest_rot);
				rig->bind[c] = p >= 0 ? rig->bind[size_t(p)].then(local) : local;
			}
			done[c] = progress = true;
		}
		if (!progress) break;
	}
	float lo = 1e30f, hi = -1e30f;
	for (auto &b : rig->skin) lo = std::min(lo, b.bind.t[1]), hi = std::max(hi, b.bind.t[1]);
	rig->height = hi > lo ? hi - lo : 30;
	auto bone_pos = [&](const char *n, V3 *out) {
		int c = rig->bank.channel(n);
		if (c < 0) return false;
		*out = rig->bind[size_t(c)].t;
		return true;
	};
	V3 l, r;
	if ((bone_pos("LeftUpperArm", &l) && bone_pos("RightUpperArm", &r)) || (bone_pos("LeftUpperLeg", &l) && bone_pos("RightUpperLeg", &r))) {
		V3 d = normalize(flat(sub(l, r)));
		if (length(d) > 0.5f) rig->left = d;
	}
	// Facing: the feet's vertices sit ahead of the ankles.
	V3 ahead{0, 0, 0};
	for (const char *foot : {"LeftFoot", "RightFoot"}) {
		int b = -1;
		for (size_t k = 0; k < rig->skin.size(); ++k) {
			if (rig->skin[k].name == foot) b = int(k);
		}
		if (b < 0) continue;
		V3 sum{0, 0, 0};
		float n = 0;
		for (size_t v = 0; v < rig->mesh.vertices.size(); ++v) {
			const auto &wts = rig->skin[size_t(b)].weights;
			if (v < wts.size() && wts[v] > 200) sum = add(sum, rig->mesh.vertices[v]), n += 1;
		}
		if (n > 0) ahead = add(ahead, flat(sub(mul(sum, 1 / n), rig->skin[size_t(b)].bind.t)));
	}
	if (length(ahead) > 1e-3f) rig->forward = normalize(ahead);
	// Spheres under the fighter, in hierarchy order.
	std::function<void(uint32_t, const std::string &)> walk = [&](uint32_t i, const std::string &parent) {
		for (uint32_t c : de::ObjectsFile::children(ff->object_data(ff->objects()[i]))) {
			const auto &o = ff->objects()[c];
			if (o.class_path == "Actor\\Affector\\Sphere") {
				RobotRig::Sphere s;
				s.name = o.name;
				s.parent = parent;
				int ch = rig->bank.channel(parent);
				s.channel = ff->objects()[i].class_path == "Actor" ? ch : -1;
				try {
					s.local = de::read_actor(*ff, o).local;
				} catch (const std::exception &) {
				}
				sphere_radii(*ff, o, &s.hotspot, &s.falloff);
				rig->spheres.push_back(s);
			}
			walk(c, o.name);
		}
	};
	walk(actor_index, ff->objects()[actor_index].name);
	// "seq<Name>" globals of its behaviors that name a sequence of the bank.
	const deo::File &fd = base.model(actor->file);
	std::set<std::string> names;
	for (auto &o : fd.objects) {
		if (o.info.class_text() != "Behavior") continue;
		std::string src = behavior_source(o);
		for (size_t at = src.find("seq"); at != std::string::npos; at = src.find("seq", at + 3)) {
			if (at > 0 && (isalnum(uint8_t(src[at - 1])) || src[at - 1] == '_')) continue;
			size_t e = at + 3;
			while (e < src.size() && (isalnum(uint8_t(src[e])) || src[e] == '_')) ++e;
			std::string n = src.substr(at + 3, e - at - 3);
			if (!n.empty() && rig->bank.find(n)) names.insert(n);
		}
	}
	rig->script_sequences.assign(names.begin(), names.end());
	return rig;
}

// ---------------------------------------------------------------------------------------------
// The file

std::vector<int> skin_joints(const ImportPlan &plan) {
	const auto &s = plan.scene;
	if (plan.har.skin < 0 || size_t(plan.har.skin) >= s.skins.size()) return {};
	return s.skins[size_t(plan.har.skin)].joints;
}

bool is_ancestor(const gltf::Scene &s, int a, int n) {
	for (int p = n; p >= 0; p = s.nodes[size_t(p)].parent) {
		if (p == a) return true;
	}
	return false;
}

int common_ancestor(const gltf::Scene &s, int a, int b) {
	for (int p = a; p >= 0; p = s.nodes[size_t(p)].parent) {
		if (is_ancestor(s, p, b)) return p;
	}
	return -1;
}

// Nodes from `from` (exclusive) down to `to` (inclusive).
std::vector<int> path_down(const gltf::Scene &s, int from, int to) {
	std::vector<int> out;
	for (int p = to; p >= 0 && p != from; p = s.nodes[size_t(p)].parent) out.push_back(p);
	std::reverse(out.begin(), out.end());
	return out;
}

// Joints carrying weight in the chosen skin's meshes.
std::set<int> weighted_joints(const ImportPlan &plan) {
	std::set<int> out;
	const auto &s = plan.scene;
	auto joints = skin_joints(plan);
	for (auto &n : s.nodes) {
		if (n.mesh < 0 || n.skin != plan.har.skin) continue;
		for (auto &p : s.meshes[size_t(n.mesh)].primitives) {
			for (size_t v = 0; v < p.joints.size() && v < p.weights.size(); ++v) {
				for (int k = 0; k < 4; ++k) {
					if (p.weights[v][size_t(k)] > 0.01f && p.joints[v][size_t(k)] < joints.size()) out.insert(joints[p.joints[v][size_t(k)]]);
				}
			}
		}
	}
	return out;
}

BoneMapping *row(HarPlan &h, const std::string &bone) {
	for (auto &b : h.bones) {
		if (b.bone == bone) return &b;
	}
	return nullptr;
}

const BoneMapping *row(const HarPlan &h, const std::string &bone) {
	for (auto &b : h.bones) {
		if (b.bone == bone) return &b;
	}
	return nullptr;
}

// The skeleton's shape: the feet are the lowest joints, the pelvis joins them, the hands are the
// widest joints above, the chest joins them; each limb's two long segments are its upper and
// lower bones.
void guess_bones(ImportPlan &plan, std::set<int> &used) {
	HarPlan &h = plan.har;
	const auto &s = plan.scene;
	auto joints = skin_joints(plan);
	if (joints.size() < 3) return;
	gltf::Mat4 fix = up_fix(plan.settings.z_up);
	std::map<int, V3> pos;
	for (int j : joints) {
		auto p = gltf::transform_point(gltf::multiply(fix, joint_bind(plan, j)), {0, 0, 0});
		pos[j] = {p[0], p[1], p[2]};
	}
	std::set<int> weighted = weighted_joints(plan);
	float lo = 1e30f, hi = -1e30f, cx = 0;
	for (auto &[j, p] : pos) lo = std::min(lo, p[1]), hi = std::max(hi, p[1]), cx += p[0];
	cx /= float(pos.size());
	const float height = std::max(hi - lo, 1e-4f);
	auto set = [&](const std::string &bone, int joint) {
		BoneMapping *r = row(h, bone);
		if (!r || r->joint >= 0 || joint < 0 || used.count(joint)) return;
		r->joint = joint;
		r->source = MapSource::Guess;
		r->confidence = 0.5f;
		used.insert(joint);
	};
	auto mapped = [&](const std::string &bone) {
		const BoneMapping *r = row(h, bone);
		return r ? r->joint : -1;
	};
	auto in_skin = [&](int n) { return pos.count(n) > 0; };
	auto chain = [&](const std::vector<int> &path_in, int *a, int *b, int *c) {
		std::vector<int> path;
		for (int n : path_in) {
			if (in_skin(n)) path.push_back(n);
		}
		*a = *b = *c = -1;
		if (path.size() < 3) return;
		float best = -1;
		for (size_t i = 0; i + 2 < path.size(); ++i) {
			float l = length(sub(pos[path[i + 1]], pos[path[i]])) + length(sub(pos[path[i + 2]], pos[path[i + 1]]));
			if (l > best + height * 0.01f) best = l, *a = path[i], *b = path[i + 1], *c = path[i + 2];
		}
	};
	// Feet: the lowest joint, and the lowest on the other side.
	std::vector<int> by_y(joints.begin(), joints.end());
	std::sort(by_y.begin(), by_y.end(), [&](int a, int b) { return pos[a][1] < pos[b][1]; });
	int f1 = by_y[0], f2 = -1;
	for (int j : by_y) {
		if ((pos[j][0] - cx) * (pos[f1][0] - cx) < 0 && std::fabs(pos[j][0] - cx) > height * 0.02f) {
			f2 = j;
			break;
		}
	}
	int pelvis = mapped("Pelvis");
	if (f2 >= 0) {
		int lca = common_ancestor(s, f1, f2);
		if (pelvis < 0 && in_skin(lca)) set("Pelvis", lca), pelvis = mapped("Pelvis");
	}
	if (pelvis < 0) return;
	// Facing from the toes (what lies past the foot joint), else the glTF convention (+Z).
	V3 forward{0, 0, 1};
	std::vector<std::vector<int>> legs;
	for (int f : {f1, f2}) {
		if (f < 0 || !is_ancestor(s, pelvis, f)) continue;
		legs.push_back(path_down(s, pelvis, f));
	}
	V3 toes{0, 0, 0};
	for (auto &leg : legs) {
		int a, b, c;
		chain(leg, &a, &b, &c);
		if (c >= 0 && leg.back() != c) toes = add(toes, flat(sub(pos[leg.back()], pos[c])));
	}
	if (length(toes) > height * 0.01f) forward = normalize(toes);
	V3 left_dir = {forward[2], 0, -forward[0]};
	auto side_of = [&](int n) { return dot(sub(pos[n], pos[pelvis]), left_dir) > 0 ? "Left" : "Right"; };
	for (auto &leg : legs) {
		int a, b, c;
		chain(leg, &a, &b, &c);
		if (a < 0) continue;
		std::string side = side_of(leg.back());
		set(side + "UpperLeg", a), set(side + "LowerLeg", b), set(side + "Foot", c);
	}
	// Hands: the widest joints not in the legs.
	auto in_legs = [&](int n) {
		for (auto &leg : legs) {
			if (!leg.empty() && is_ancestor(s, leg.front(), n)) return true;
		}
		return false;
	};
	int wl = -1, wr = -1;
	for (int j : joints) {
		if (j == pelvis || in_legs(j) || !is_ancestor(s, pelvis, j)) continue;
		float side = dot(sub(pos[j], pos[pelvis]), left_dir);
		if (side > 0 && (wl < 0 || side > dot(sub(pos[wl], pos[pelvis]), left_dir))) wl = j;
		if (side < 0 && (wr < 0 || side < dot(sub(pos[wr], pos[pelvis]), left_dir))) wr = j;
	}
	if (wl < 0 || wr < 0) return;
	int chest = mapped("Chest");
	if (chest < 0) {
		int lca = common_ancestor(s, wl, wr);
		if (in_skin(lca) && lca != pelvis) set("Chest", lca), chest = mapped("Chest");
	}
	if (chest < 0) return;
	for (int tip : {wl, wr}) {
		int a, b, c;
		chain(path_down(s, chest, tip), &a, &b, &c);
		if (a < 0) continue;
		std::string side = side_of(tip);
		set(side + "UpperArm", a), set(side + "LowerArm", b), set(side + "Hand", c);
	}
	// Head: the last weighted joint on the way to the highest joint outside the arms.
	auto in_arm = [&](int n) {
		for (const char *b : {"LeftUpperArm", "RightUpperArm"}) {
			int a = mapped(b);
			if (a >= 0 && is_ancestor(s, a, n)) return true;
		}
		return false;
	};
	int top = -1;
	for (int j : joints) {
		if (j == chest || !is_ancestor(s, chest, j) || in_arm(j)) continue;
		if (top < 0 || pos[j][1] > pos[top][1]) top = j;
	}
	if (top >= 0) {
		int head = -1;
		for (int n : path_down(s, chest, top)) {
			if (in_skin(n) && (weighted.count(n) || head < 0)) head = n;
		}
		set("Head", head);
	}
}

gltf::Vec3 node_origin(const gltf::Mat4 &m) { return gltf::transform_point(m, {0, 0, 0}); }

// The glTF animation's pose at `time`: every node's local transform.
struct NodePose {
	std::vector<gltf::Vec3> t, s;
	std::vector<gltf::Vec4> r;
};

void sample_animation(const gltf::Scene &sc, const gltf::Animation &a, float time, NodePose &pose) {
	size_t n = sc.nodes.size();
	pose.t.resize(n), pose.r.resize(n), pose.s.resize(n);
	for (size_t i = 0; i < n; ++i) pose.t[i] = sc.nodes[i].translation, pose.r[i] = sc.nodes[i].rotation, pose.s[i] = sc.nodes[i].scale;
	for (const auto &ch : a.channels) {
		if (ch.node < 0 || size_t(ch.node) >= n || ch.sampler < 0 || size_t(ch.sampler) >= a.samplers.size()) continue;
		if (ch.path == gltf::Animation::Path::Weights) continue;
		const auto &smp = a.samplers[size_t(ch.sampler)];
		const size_t comps = ch.path == gltf::Animation::Path::Rotation ? 4 : 3;
		const bool cubic = smp.interpolation == gltf::Animation::Interpolation::CubicSpline;
		const size_t stride = comps * (cubic ? 3 : 1), off = cubic ? comps : 0;
		const size_t keys = smp.input.size();
		if (!keys || smp.output.size() < keys * stride) continue;
		float out[4] = {0, 0, 0, 0};
		auto value = [&](size_t k, size_t c) { return smp.output[k * stride + off + c]; };
		if (time <= smp.input[0] || keys == 1) {
			for (size_t c = 0; c < comps; ++c) out[c] = value(0, c);
		} else if (time >= smp.input[keys - 1]) {
			for (size_t c = 0; c < comps; ++c) out[c] = value(keys - 1, c);
		} else {
			size_t k = size_t(std::upper_bound(smp.input.begin(), smp.input.end(), time) - smp.input.begin()) - 1;
			float t0 = smp.input[k], t1 = smp.input[k + 1], dt = t1 - t0, u = dt > 0 ? (time - t0) / dt : 0;
			if (smp.interpolation == gltf::Animation::Interpolation::Step) {
				for (size_t c = 0; c < comps; ++c) out[c] = value(k, c);
			} else if (cubic) {
				float u2 = u * u, u3 = u2 * u;
				for (size_t c = 0; c < comps; ++c) {
					float p0 = value(k, c), p1 = value(k + 1, c);
					float m0 = smp.output[k * stride + 2 * comps + c] * dt, m1 = smp.output[(k + 1) * stride + c] * dt;
					out[c] = (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1;
				}
			} else if (comps == 4) {
				de::Quat qa{value(k, 0), value(k, 1), value(k, 2), value(k, 3)}, qb{value(k + 1, 0), value(k + 1, 1), value(k + 1, 2), value(k + 1, 3)};
				de::Quat q = de::slerp_quat(qa, qb, u);
				for (size_t c = 0; c < 4; ++c) out[c] = q[c];
			} else {
				for (size_t c = 0; c < comps; ++c) out[c] = value(k, c) + (value(k + 1, c) - value(k, c)) * u;
			}
		}
		if (comps == 4) {
			float l = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
			for (auto &x : out) x /= l > 0 ? l : 1;
			pose.r[size_t(ch.node)] = {out[0], out[1], out[2], out[3]};
		} else if (ch.path == gltf::Animation::Path::Translation) {
			pose.t[size_t(ch.node)] = {out[0], out[1], out[2]};
		} else {
			pose.s[size_t(ch.node)] = {out[0], out[1], out[2]};
		}
	}
}

gltf::Mat4 posed_world(const gltf::Scene &sc, const NodePose &pose, int node, std::map<int, gltf::Mat4> &memo) {
	if (auto it = memo.find(node); it != memo.end()) return it->second;
	gltf::Mat4 local = gltf::compose(pose.t[size_t(node)], pose.r[size_t(node)], pose.s[size_t(node)]);
	int p = sc.nodes[size_t(node)].parent;
	gltf::Mat4 w = p >= 0 ? gltf::multiply(posed_world(sc, pose, p, memo), local) : local;
	memo[node] = w;
	return w;
}

bool lod_of(const std::string &name, int *k) {
	std::string l = lower(name);
	size_t at = l.rfind("lod");
	if (at == std::string::npos || at + 3 >= l.size() || !isdigit(uint8_t(l[at + 3]))) return false;
	if (at + 4 != l.size()) return false;
	*k = l[at + 3] - '0';
	return *k >= 0 && *k <= 2;
}

}  // namespace

const char *map_source_name(MapSource s) {
	switch (s) {
	case MapSource::Name: return "name";
	case MapSource::Preset: return "preset";
	case MapSource::Guess: return "guess";
	case MapSource::Manual: return "manual";
	default: return "missing";
	}
}

std::shared_ptr<const RobotRig> load_rig(const BaseData &base, const std::string &robot) {
	static std::mutex m;
	static std::map<std::pair<const BaseData *, std::string>, std::shared_ptr<const RobotRig>> cache;
	std::lock_guard<std::mutex> lock(m);
	auto key = std::make_pair(&base, robot);
	if (auto it = cache.find(key); it != cache.end()) return it->second;
	auto rig = read_rig(base, robot);
	cache[key] = rig;
	return rig;
}

const std::vector<std::string> &rig_presets() {
	static const std::vector<std::string> names = [] {
		std::vector<std::string> v{"Auto"};
		for (auto &p : presets()) v.push_back(p.name);
		return v;
	}();
	return names;
}

gltf::Mat4 joint_bind(const ImportPlan &plan, int node) {
	const auto &s = plan.scene;
	if (plan.har.skin >= 0 && size_t(plan.har.skin) < s.skins.size()) {
		const auto &sk = s.skins[size_t(plan.har.skin)];
		for (size_t k = 0; k < sk.joints.size(); ++k) {
			if (sk.joints[k] == node && k < sk.inverse_binds.size()) return gltf::inverse(sk.inverse_binds[k]);
		}
	}
	return s.world(node);
}

namespace {

// Mesh pieces of a LOD: the primitive and how its positions reach the file's space.
struct RobotPiece {
	const gltf::Primitive *prim = nullptr;
	gltf::Mat4 m = gltf::identity();
	int node = -1;
	bool skinned = false;
};

std::vector<RobotPiece> lod_pieces(const ImportPlan &plan, int lod) {
	std::vector<RobotPiece> out;
	const auto &s = plan.scene;
	for (int n : plan.har.lods[size_t(lod)]) {
		const auto &node = s.nodes[size_t(n)];
		if (node.mesh < 0) continue;
		bool skinned = node.skin >= 0 && node.skin == plan.har.skin;
		for (auto &p : s.meshes[size_t(node.mesh)].primitives) out.push_back({&p, skinned ? gltf::identity() : s.world(n), n, skinned && !p.joints.empty()});
	}
	return out;
}

}  // namespace

gltf::Mat4 robot_fix(const ImportPlan &plan) {
	const HarPlan &h = plan.har;
	gltf::Mat4 m = up_fix(plan.settings.z_up);
	if (h.mirror) m = gltf::multiply(gltf::Mat4{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, m);
	if (h.turn % 360) m = gltf::multiply(rotate_y(h.turn), m);
	if (h.ground) {
		float lo = 1e30f;
		for (auto &pc : lod_pieces(plan, 0)) {
			gltf::Mat4 mm = gltf::multiply(m, pc.m);
			for (auto &p : pc.prim->positions) lo = std::min(lo, gltf::transform_point(mm, p)[1]);
		}
		if (lo < 1e29f) {
			gltf::Mat4 t = gltf::identity();
			t[13] = -lo;
			m = gltf::multiply(t, m);
		}
	}
	return m;
}

void map_bones(ImportPlan &plan) {
	HarPlan &h = plan.har;
	if (!h.rig) return;
	const RobotRig &rig = *h.rig;
	const auto &s = plan.scene;
	std::vector<BoneMapping> rows;
	for (auto &b : rig.skin) {
		BoneMapping r;
		r.bone = b.name;
		r.required = true;
		rows.push_back(r);
	}
	// Optional, by exact name: the head anchor, and the root whose motion the game leaves out.
	for (const char *opt : {"HeadAnchor", "Body"}) {
		if (rig.channel(opt) < 0) continue;
		BoneMapping r;
		r.bone = opt;
		r.required = false;
		rows.push_back(r);
	}
	std::set<int> used;
	for (auto &r : rows) {
		const BoneMapping *old = row(h, r.bone);
		if (old && old->source == MapSource::Manual && old->joint >= 0 && size_t(old->joint) < s.nodes.size()) {
			r = *old;
			used.insert(r.joint);
		}
	}
	h.bones = rows;
	std::vector<int> cand = skin_joints(plan);
	std::vector<int> all(s.nodes.size());
	for (size_t i = 0; i < all.size(); ++i) all[i] = int(i);
	std::map<int, NameKey> keys;
	for (size_t i = 0; i < s.nodes.size(); ++i) keys[int(i)] = name_key(s.nodes[i].name);
	auto take = [&](BoneMapping &r, int j, MapSource src, float conf) {
		r.joint = j, r.source = src, r.confidence = conf;
		used.insert(j);
	};
	// Exact names (any node for the optional bones).
	for (auto &r : h.bones) {
		if (r.joint >= 0) continue;
		NameKey rk = name_key(r.bone);
		int found = -1, hits = 0;
		for (int j : r.required ? cand : all) {
			if (used.count(j) || (!r.required && s.nodes[size_t(j)].mesh >= 0)) continue;
			if (s.nodes[size_t(j)].name == r.bone) {
				found = j, hits = 1;
				break;
			}
			if (keys[j].side == rk.side && keys[j].core == rk.core) found = j, ++hits;
		}
		if (found >= 0 && hits == 1) take(r, found, MapSource::Name, 1.0f);
	}
	auto apply = [&](const Preset &p, bool dry, float conf) {
		int n = 0;
		std::set<int> taken = used;
		for (auto &r : h.bones) {
			if (r.joint >= 0 || !r.required) continue;
			NameKey rk = name_key(r.bone);
			for (auto &[core, syns] : p.bones) {
				if (core != rk.core) continue;
				int found = -1;
				for (const std::string &syn : syns) {
					for (int j : cand) {
						if (!taken.count(j) && keys[j].core == syn && keys[j].side == rk.side) {
							found = j;
							break;
						}
					}
					if (found >= 0) break;
				}
				if (found < 0) continue;
				++n;
				taken.insert(found);
				if (!dry) take(r, found, MapSource::Preset, conf);
			}
		}
		return n;
	};
	h.detected.clear();
	if (h.preset == "Auto") {
		int best = 0;
		const Preset *pick = nullptr;
		for (auto &p : presets()) {
			int n = apply(p, true, 0);
			if (n > best) best = n, pick = &p;
		}
		if (pick) {
			h.detected = pick->name;
			apply(*pick, false, 0.85f);
		}
		apply(presets().back(), false, 0.7f);
	} else {
		for (auto &p : presets()) {
			if (h.preset == p.name) apply(p, false, 0.85f);
		}
	}
	guess_bones(plan, used);
}

void detect_orientation(ImportPlan &plan) {
	HarPlan &h = plan.har;
	if (!h.rig) return;
	const auto &s = plan.scene;
	gltf::Mat4 fix = up_fix(plan.settings.z_up);
	auto at = [&](const std::string &bone, V3 *out) {
		const BoneMapping *r = row(h, bone);
		if (!r || r->joint < 0) return false;
		auto p = node_origin(gltf::multiply(fix, joint_bind(plan, r->joint)));
		*out = {p[0], p[1], p[2]};
		return true;
	};
	V3 l, r;
	if (!((at("LeftUpperArm", &l) && at("RightUpperArm", &r)) || (at("LeftUpperLeg", &l) && at("RightUpperLeg", &r)))) return;
	V3 left = normalize(flat(sub(l, r)));
	if (length(left) < 0.5f) return;
	const V3 up{0, 1, 0};
	V3 ahead{0, 0, 0};
	auto joints = skin_joints(plan);
	for (const char *foot : {"LeftFoot", "RightFoot"}) {
		const BoneMapping *fr = row(h, foot);
		V3 fp;
		if (!fr || fr->joint < 0 || !at(foot, &fp)) continue;
		// As the rig's: the vertices the foot carries, else the joints past it (toes).
		V3 sum{0, 0, 0};
		float n = 0;
		int k = -1;
		for (size_t i = 0; i < joints.size(); ++i) {
			if (joints[i] == fr->joint) k = int(i);
		}
		for (auto &pc : lod_pieces(plan, 0)) {
			if (!pc.skinned || k < 0) continue;
			for (size_t v = 0; v < pc.prim->positions.size() && v < pc.prim->joints.size(); ++v) {
				for (int q = 0; q < 4; ++q) {
					if (pc.prim->joints[v][size_t(q)] == k && pc.prim->weights[v][size_t(q)] > 0.8f) {
						auto p = gltf::transform_point(fix, pc.prim->positions[v]);
						sum = add(sum, {p[0], p[1], p[2]}), n += 1;
					}
				}
			}
		}
		std::function<void(int)> walk = [&](int node) {
			for (int c : s.nodes[size_t(node)].children) {
				auto p = node_origin(gltf::multiply(fix, joint_bind(plan, c)));
				sum = add(sum, {p[0], p[1], p[2]}), n += 1;
				walk(c);
			}
		};
		if (n == 0) walk(fr->joint);
		if (n > 0) ahead = add(ahead, flat(sub(mul(sum, 1 / n), fp)));
	}
	V3 forward = length(ahead) > 1e-4f ? normalize(ahead) : normalize(cross(left, up));
	// The rig's facing in glTF terms (z negated).
	const RobotRig &rig = *h.rig;
	V3 bf{rig.forward[0], 0, -rig.forward[2]}, bl{rig.left[0], 0, -rig.left[2]};
	auto chirality = [&](const V3 &f, const V3 &lft) { return dot(cross(f, up), lft) > 0; };
	h.mirror = chirality(forward, left) != chirality(bf, bl);
	if (h.mirror) forward[0] = -forward[0];
	float theta = std::atan2(forward[2] * bf[0] - forward[0] * bf[2], forward[0] * bf[0] + forward[2] * bf[2]);
	int turn = int(std::lround(theta * 180 / 3.14159265f / 90)) * 90;
	h.turn = ((turn % 360) + 360) % 360;
}

void analyse_robot(ImportPlan &plan, const BaseData &base) {
	HarPlan &h = plan.har;
	h.notes.clear();
	h.rig.reset();
	try {
		h.rig = load_rig(base, h.base_robot);
	} catch (const std::exception &e) {
		h.notes.push_back(std::string("error: ") + e.what());
	}
	const auto &s = plan.scene;
	// The skin most triangles use.
	std::map<int, size_t> tris;
	for (auto &n : s.nodes) {
		if (n.mesh < 0 || n.skin < 0) continue;
		for (auto &p : s.meshes[size_t(n.mesh)].primitives) tris[n.skin] += p.triangles();
	}
	h.skin = -1;
	size_t best = 0;
	for (auto &[k, v] : tris) {
		if (v > best) best = v, h.skin = k;
	}
	// LODs: nodes named LOD0..LOD2 (or ending so); every other mesh is LOD0.
	for (auto &l : h.lods) l.clear();
	bool tagged = false;
	for (auto &it : plan.items) {
		int k;
		if (it.role != Role::Ignore && lod_of(it.name, &k)) tagged = true;
	}
	for (auto &it : plan.items) {
		if (it.role == Role::Ignore || s.nodes[size_t(it.node)].mesh < 0) continue;
		int k = 0;
		if (!(tagged && lod_of(it.name, &k))) k = 0;
		h.lods[size_t(k)].push_back(it.node);
	}
	// Our own exports are in the game's convention already.
	if (plan.from_editor) h.ground = false, h.auto_orient = false, h.mirror = false, h.turn = 0;
	h.paint = HarPlan::Paint::Materials;
	for (auto &pc : lod_pieces(plan, 0)) {
		int m = pc.prim->material;
		if (m >= 0 && (s.materials[size_t(m)].base_texture || (s.materials[size_t(m)].extras.is_object() && s.materials[size_t(m)].extras.find("de_stages")))) h.paint = HarPlan::Paint::Colours;
	}
	if (!h.rig) return;
	map_bones(plan);
	if (h.auto_orient) detect_orientation(plan);
	h.sequences.clear();
	std::map<std::string, int> anims;
	for (size_t a = 0; a < s.animations.size(); ++a) anims.emplace(anim_key(s.animations[a].name), int(a));
	for (auto &seq : h.rig->bank.sequences) {
		SequenceMapping m;
		m.sequence = seq.name;
		if (auto it = anims.find(anim_key(seq.name)); it != anims.end()) m.animation = it->second, m.source = MapSource::Name;
		h.sequences.push_back(m);
	}
	h.spheres.clear();
	for (size_t i = 0; i < s.nodes.size(); ++i) {
		const std::string &n = s.nodes[i].name;
		if (lower(n).rfind("sphere:", 0) != 0) continue;
		SphereSource sp;
		sp.sphere = n.substr(7);
		sp.node = int(i);
		for (int p = s.nodes[i].parent; p >= 0 && sp.bone.empty(); p = s.nodes[size_t(p)].parent) {
			for (auto &b : h.bones) {
				if (b.joint == p && b.required) sp.bone = b.bone;
			}
		}
		for (auto &rs : h.rig->spheres) sp.known = sp.known || rs.name == sp.sphere;
		h.spheres.push_back(sp);
	}
	if (!h.detected.empty()) h.notes.push_back("Rig looks like " + h.detected);
}

void robot_issues(const ImportPlan &plan, std::vector<Issue> &out) {
	const HarPlan &h = plan.har;
	auto add = [&](Issue::Severity sev, const std::string &t) { out.push_back({sev, 0, -1, t, 0}); };
	for (auto &n : h.notes) {
		if (n.rfind("error: ", 0) == 0) add(Issue::Error, "Base robot " + h.base_robot + ": " + n.substr(7));
	}
	if (!h.rig) {
		if (h.notes.empty()) add(Issue::Error, "Choose a base robot");
		return;
	}
	const auto &s = plan.scene;
	if (h.skin < 0) {
		add(Issue::Error, "The file has no skin (a mesh bound to joints): import it as a prop, or skin it to an armature first");
		return;
	}
	std::map<int, std::string> by_joint;
	for (auto &b : h.bones) {
		if (b.joint < 0) {
			if (b.required) add(Issue::Error, "Bone " + b.bone + " is not mapped: pick a joint on the Bones page");
			continue;
		}
		if (by_joint.count(b.joint)) add(Issue::Error, "Joint " + s.nodes[size_t(b.joint)].name + " is mapped to both " + by_joint[b.joint] + " and " + b.bone);
		by_joint[b.joint] = b.bone;
	}
	const RobotRig &rig = *h.rig;
	for (auto &b : h.bones) {
		int c = rig.channel(b.bone);
		if (b.joint < 0 || c < 0) continue;
		for (int p = rig.bank.channels[size_t(c)].parent; p >= 0; p = rig.bank.channels[size_t(p)].parent) {
			const BoneMapping *pr = row(h, rig.bank.channels[size_t(p)].name);
			if (!pr) continue;
			if (pr->joint >= 0 && !is_ancestor(s, pr->joint, b.joint))
				add(Issue::Warning, b.bone + " (" + s.nodes[size_t(b.joint)].name + ") is not under " + pr->bone + " (" + s.nodes[size_t(pr->joint)].name + ") in the file");
			break;
		}
	}
	size_t verts = 0, faces = 0;
	for (auto &pc : lod_pieces(plan, 0)) verts += pc.prim->positions.size(), faces += pc.prim->triangles();
	if (h.lods[0].empty()) add(Issue::Error, "No mesh for LOD0");
	if (faces > kMaxMeshFaces || verts > kMaxMeshVertices) add(Issue::Warning, "LOD0 has " + std::to_string(faces) + " triangles: past a mesh's 65,535, it is decimated to fit");
	else if (faces > 12000 && !h.decimate_lod0) add(Issue::Warning, "LOD0 has " + std::to_string(faces) + " triangles (the base robot's has " + std::to_string(rig.lods[0].faces) + "); the game skins it on the CPU");
	std::vector<std::string> unused;
	std::set<int> used_anims;
	for (auto &m : h.sequences) {
		if (m.animation >= 0) used_anims.insert(m.animation);
	}
	for (size_t a = 0; a < s.animations.size(); ++a) {
		if (!used_anims.count(int(a))) unused.push_back(s.animations[a].name);
	}
	if (!unused.empty() && !plan.from_editor) {
		std::string list;
		for (size_t i = 0; i < unused.size() && i < 4; ++i) list += (i ? ", " : "") + unused[i];
		if (unused.size() > 4) list += ", ...";
		add(Issue::Warning, std::to_string(unused.size()) + " animation(s) match no sequence of " + h.base_robot + " and are left out (" + list + "); name them as a sequence on the Animations page");
	}
	std::set<std::string> have;
	for (auto &m : h.sequences) have.insert(m.sequence);
	for (auto &n : rig.script_sequences) {
		if (!have.count(n)) add(Issue::Error, "The scripts use sequence " + n + ", which the bank lacks");
	}
	for (auto &sp : h.spheres) {
		if (!sp.known) add(Issue::Warning, "Sphere:" + sp.sphere + " names no hitbox of " + h.base_robot + " and is left out");
		else if (sp.bone.empty()) add(Issue::Warning, "Sphere:" + sp.sphere + " is not under a mapped joint; the base robot's sphere is kept");
	}
	// Size against the base robot.
	float lo = 1e30f, hi = -1e30f;
	gltf::Mat4 fix = robot_fix(plan);
	for (auto &b : h.bones) {
		if (b.joint < 0 || !b.required) continue;
		float y = gltf::transform_point(gltf::multiply(fix, joint_bind(plan, b.joint)), {0, 0, 0})[1] * plan.settings.units_per_metre;
		lo = std::min(lo, y), hi = std::max(hi, y);
	}
	if (hi > lo && rig.height > 0) {
		float r = (hi - lo) / rig.height;
		if (r < 0.35f || r > 3) {
			char b[200];
			snprintf(b, sizeof b, "The skeleton is %.3g times %s's size: check Units per metre (Match %s's size sets it)", r, h.base_robot.c_str(), h.base_robot.c_str());
			add(Issue::Warning, b);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Building

namespace {

struct Tile {
	int material = -1;
	int image = -1;  // glTF image, or -1 for a flat colour
	gltf::Vec4 colour{1, 1, 1, 1};
	int col = 0, row = 0;
};

struct Atlas {
	int size = 0, grid = 1, tile = 0;
	bool identity = false;  // one texture: its UVs as they are
	std::map<int, size_t> tile_of;  // material -> tile
	std::vector<Tile> tiles;
};

std::vector<uint8_t> image_rgba(const gltf::Scene &s, int image, int w, int h) {
	if (image < 0 || size_t(image) >= s.images.size() || s.images[size_t(image)].rgba.empty()) return std::vector<uint8_t>(size_t(w) * size_t(h) * 4, 255);
	const auto &im = s.images[size_t(image)];
	if (im.width == w && im.height == h) return im.rgba;
	return resize_rgba(im.width, im.height, im.rgba, w, h);
}

int find_image(const gltf::Scene &s, const std::vector<std::string> &keys, const std::string &extra, const std::set<int> &materials) {
	for (int m : materials) {
		if (m < 0) continue;
		const auto &ex = s.materials[size_t(m)].extras;
		if (const json::Value *v = ex.is_object() ? ex.find(extra) : nullptr; v && v->is_number() && v->as_int() >= 0 && size_t(v->as_int()) < s.images.size()) return int(v->as_int());
	}
	for (size_t i = 0; i < s.images.size(); ++i) {
		std::string n = lower(s.images[i].name + " " + s.images[i].uri);
		std::string squeezed;
		for (char c : n) {
			if (isalnum(uint8_t(c))) squeezed += c;
		}
		for (auto &k : keys) {
			if (squeezed.find(k) != std::string::npos) return int(i);
		}
	}
	return -1;
}

const json::Value *stages_of(const gltf::Scene &s, int material) {
	if (material < 0 || size_t(material) >= s.materials.size()) return nullptr;
	const auto &ex = s.materials[size_t(material)].extras;
	const json::Value *st = ex.is_object() ? ex.find("de_stages") : nullptr;
	return st && st->is_array() && st->size() ? st : nullptr;
}

struct LodData {
	std::vector<V3> verts;
	std::vector<std::vector<uint8_t>> skin;  // per vertex, per skin bone
	std::vector<std::array<uint32_t, 3>> faces;
	std::vector<std::vector<UV3>> uvs;  // per layer, per face
	std::vector<std::array<V3, 3>> normals;
	std::vector<uint32_t> flags;  // from de_face_flags when exact
	bool exact = false;
};

RobotLod finish_lod(LodData &d) {
	RobotLod out;
	std::vector<int> remap(d.verts.size(), -1);
	for (auto &f : d.faces) {
		for (uint32_t v : f) {
			if (remap[v] < 0) {
				remap[v] = int(out.vertices.size());
				out.vertices.push_back(d.verts[v]);
			}
		}
	}
	if (d.exact) {
		// Keep the source numbering (an export's _DE_VERTEX).
		out.vertices = d.verts;
		for (size_t v = 0; v < remap.size(); ++v) remap[v] = int(v);
	}
	size_t nb = d.skin.empty() ? 0 : d.skin[0].size();
	out.weights.assign(nb, std::vector<uint8_t>(out.vertices.size(), 0));
	for (size_t v = 0; v < d.verts.size(); ++v) {
		if (remap[v] < 0) continue;
		for (size_t b = 0; b < nb; ++b) out.weights[b][size_t(remap[v])] = d.skin[v][b];
	}
	for (size_t f = 0; f < d.faces.size(); ++f) {
		de::Mesh::Face face{};
		for (int k = 0; k < 3; ++k) face.v[size_t(k)] = uint16_t(remap[d.faces[f][size_t(k)]]);
		out.faces.push_back(face);
	}
	out.uvs = d.uvs;
	build::mesh_adjacency(out.faces);
	if (d.exact && d.flags.size() == out.faces.size()) {
		for (size_t f = 0; f < out.faces.size(); ++f) out.faces[f].flags = d.flags[f];
	} else if (d.normals.size() == out.faces.size()) {
		build::mesh_smoothing(out.faces, d.normals);
	} else {
		// Corner normals from the faces within 60 degrees of each other around a vertex.
		std::vector<V3> fn(out.faces.size());
		std::vector<std::vector<uint32_t>> around(out.vertices.size());
		for (size_t f = 0; f < out.faces.size(); ++f) {
			const auto &F = out.faces[f];
			V3 n = cross(sub(out.vertices[F.v[0]], out.vertices[F.v[1]]), sub(out.vertices[F.v[2]], out.vertices[F.v[1]]));
			fn[f] = normalize(mul(n, -1));
			for (uint16_t v : F.v) around[v].push_back(uint32_t(f));
		}
		std::vector<std::array<V3, 3>> cn(out.faces.size());
		for (size_t f = 0; f < out.faces.size(); ++f) {
			for (int k = 0; k < 3; ++k) {
				V3 sum{0, 0, 0};
				for (uint32_t g : around[out.faces[f].v[size_t(k)]]) {
					if (dot(fn[f], fn[g]) > 0.5f) sum = add(sum, fn[g]);
				}
				V3 n = normalize(sum);
				// Quantised so faces agreeing on a vertex agree exactly.
				for (auto &c : n) c = std::round(c * 64) / 64;
				cn[f][size_t(k)] = n;
			}
		}
		build::mesh_smoothing(out.faces, cn);
	}
	return out;
}

LodData decimate(const LodData &src, size_t target) {
	SimplifyMesh sm;
	sm.vertices = src.verts;
	sm.faces = src.faces;
	sm.uvs = src.uvs;
	sm.skin.resize(src.skin.size());
	for (size_t v = 0; v < src.skin.size(); ++v) {
		for (uint8_t w : src.skin[v]) sm.skin[v].push_back(float(w) / 255.0f);
	}
	simplify(sm, target);
	LodData out;
	out.verts = src.verts;
	out.skin = src.skin;
	out.faces = sm.faces;
	out.uvs = sm.uvs;
	return out;
}

}  // namespace

RobotBuild build_robot(const ImportPlan &plan, bool lod0_only) {
	const HarPlan &h = plan.har;
	if (!h.rig) throw std::runtime_error("no base robot");
	const RobotRig &rig = *h.rig;
	const gltf::Scene &s = plan.scene;
	const float u = plan.settings.units_per_metre;
	const gltf::Mat4 F = robot_fix(plan);
	RobotBuild out;
	const size_t nc = rig.bank.channels.size(), nb = rig.skin.size();
	auto de_point = [&](const gltf::Mat4 &m) { return from_gltf(node_origin(gltf::multiply(F, m)), u); };
	// The mapped joints.
	std::vector<int> joint_of(nc, -1);
	for (auto &b : h.bones) {
		int c = rig.channel(b.bone);
		if (c >= 0 && b.joint >= 0) joint_of[size_t(c)] = b.joint;
	}
	for (size_t c = 0; c < nc; ++c) {
		if (rig.required(int(c)) && joint_of[c] < 0) throw std::runtime_error("bone " + rig.bank.channels[c].name + " is not mapped");
	}
	std::vector<V3> jn(nc), jb(nc);
	for (size_t c = 0; c < nc; ++c) {
		jb[c] = rig.bind[c].t;
		if (joint_of[c] >= 0) jn[c] = de_point(joint_bind(plan, joint_of[c]));
	}
	// Overall scale: the skin bones' vertical span.
	{
		float lo = 1e30f, hi = -1e30f;
		for (size_t c = 0; c < nc; ++c) {
			if (rig.required(int(c))) lo = std::min(lo, jn[c][1]), hi = std::max(hi, jn[c][1]);
		}
		out.scale = hi > lo && rig.height > 0 ? (hi - lo) / rig.height : 1;
	}
	const float sc = out.scale;
	std::vector<std::vector<int>> children(nc);
	for (size_t c = 0; c < nc; ++c) {
		int p = rig.bank.channels[c].parent;
		if (p >= 0 && size_t(p) < nc) children[size_t(p)].push_back(int(c));
	}
	auto sided = [&](int c, const char *side) { return rig.bank.channels[size_t(c)].name.rfind(side, 0) == 0; };
	// Each skin bone's turn: its base direction (to its central child, across its left / right
	// children) onto the new one. Leaves take their parent's.
	std::vector<de::Transform> turn(nc);
	std::vector<bool> has_turn(nc, false);
	for (size_t c = 0; c < nc; ++c) {
		if (!rig.required(int(c))) continue;
		int primary = -1, l = -1, r = -1;
		for (int k : children[c]) {
			if (!rig.required(k)) continue;
			if (sided(k, "Left")) l = l < 0 ? k : l;
			else if (sided(k, "Right")) r = r < 0 ? k : r;
			else if (primary < 0) primary = k;
		}
		if (primary < 0) {
			for (int k : children[c]) {
				if (rig.required(k) && primary < 0 && !(l >= 0 && r >= 0)) primary = k;
			}
		}
		if (primary < 0) continue;
		V3 db = sub(jb[size_t(primary)], jb[c]), dn = sub(jn[size_t(primary)], jn[c]);
		if (l >= 0 && r >= 0) turn[c] = frame(dn, sub(jn[size_t(l)], jn[size_t(r)])).then(frame(db, sub(jb[size_t(l)], jb[size_t(r)])).inverse());
		else turn[c] = rotation_between(db, dn);
		has_turn[c] = true;
	}
	for (size_t pass = 0; pass < nc; ++pass) {
		bool changed = false;
		for (size_t c = 0; c < nc; ++c) {
			int p = rig.bank.channels[c].parent;
			if (rig.required(int(c)) && !has_turn[c] && p >= 0 && size_t(p) < nc && has_turn[size_t(p)]) turn[c] = turn[size_t(p)], has_turn[c] = changed = true;
		}
		if (!changed) break;
	}
	// The new bind frame of every channel: skin bones from the file, the rest following their parent.
	const int body = rig.channel("Body"), head = rig.channel("Head"), anchor = rig.channel("HeadAnchor");
	std::vector<de::Transform> bn(nc);
	std::vector<bool> done(nc, false);
	for (size_t pass = 0; pass < nc; ++pass) {
		bool progress = false;
		for (size_t c = 0; c < nc; ++c) {
			if (done[c]) continue;
			int p = int(c) == body ? -1 : rig.bank.channels[c].parent;
			if (p >= 0 && size_t(p) < nc && !done[size_t(p)]) continue;
			if (p >= 0 && size_t(p) >= nc) p = -1;
			if (rig.required(int(c))) {
				bn[c] = rotation_only(turn[c]).then(rotation_only(rig.bind[c]));
				bn[c].t = jn[c];
			} else if (p < 0) {
				bn[c] = rig.bind[c];
				bn[c].t = mul(rig.bind[c].t, sc);
			} else {
				de::Transform local = affine_inverse(rig.bind[size_t(p)]).then(rig.bind[c]);
				local.t = mul(local.t, sc);
				bn[c] = bn[size_t(p)].then(local);
			}
			if (int(c) == anchor) {
				if (joint_of[c] >= 0 && body >= 0 && joint_of[size_t(body)] >= 0) bn[c].t = bn[size_t(body)].then(from_gltf(gltf::multiply(F, joint_bind(plan, joint_of[size_t(body)])), u).inverse()).apply(jn[c]);
				else if (joint_of[c] >= 0) bn[c].t = jn[c];
				else if (head >= 0) bn[c].t = add(bn[size_t(head)].t, mul(sub(rig.bind[c].t, rig.bind[size_t(head)].t), sc));
			}
			done[c] = progress = true;
		}
		if (!progress) break;
	}
	for (size_t b = 0; b < nb; ++b) {
		int c = rig.channel(rig.skin[b].name);
		out.binds.push_back(c >= 0 ? bn[size_t(c)] : rig.skin[b].bind);
	}
	// Channel offsets stay in their parent channel's frame, scaled by the bone's change of length,
	// so every limb points where the base's does.
	std::vector<float> k(nc, sc);
	for (size_t c = 0; c < nc; ++c) {
		int p = rig.bank.channels[c].parent;
		if (!rig.required(int(c))) continue;
		if (p >= 0 && size_t(p) < nc && rig.required(p)) {
			float l0 = length(sub(jb[c], jb[size_t(p)]));
			if (l0 > 1e-3f) k[c] = length(sub(jn[c], jn[size_t(p)])) / l0;
		} else if (jb[c][1] > 1e-3f && jn[c][1] > 1e-3f) {
			k[c] = jn[c][1] / jb[c][1];
		}
	}
	auto map_pos = [&](size_t c, const V3 &p) { return mul(p, k[c]); };
	out.bank = rig.bank;
	for (size_t c = 0; c < nc; ++c) out.bank.channels[c].rest_pos = map_pos(c, rig.bank.channels[c].rest_pos);
	auto retarget = [&](const de::AnimSequence &seq) {
		de::AnimSequence o = seq;
		for (auto &st : o.streams) {
			if (st.channel >= nc) continue;
			for (auto &k : st.pos) k.p = map_pos(st.channel, k.p);
		}
		return o;
	};
	for (size_t q = 0; q < out.bank.sequences.size(); ++q) out.bank.sequences[q] = retarget(rig.bank.sequences[q]);
	// File animations named as sequences: each mapped bone follows its joint.
	const auto joints = skin_joints(plan);
	std::map<int, size_t> skin_index;
	for (size_t k = 0; k < joints.size(); ++k) skin_index[joints[k]] = k;
	const gltf::Mat4 Finv = gltf::inverse(F);
	for (const SequenceMapping &m : h.sequences) {
		if (m.animation < 0 || size_t(m.animation) >= s.animations.size()) continue;
		de::AnimSequence *seq = nullptr;
		for (auto &q : out.bank.sequences) {
			if (q.name == m.sequence) seq = &q;
		}
		if (!seq) continue;
		const gltf::Animation &a = s.animations[size_t(m.animation)];
		bool normalised = a.extras.is_object() && a.extras.find("de_normalised") && a.extras.find("de_normalised")->is_bool() && a.extras.find("de_normalised")->as_bool();
		float duration = std::max(a.duration(), 1e-3f);
		int keys = std::clamp(int(std::lround((normalised ? 1.0f : duration) * 30)) + 1, 2, 241);
		// Channels the file drives: mapped skin bones (and HeadAnchor when mapped).
		std::vector<size_t> driven;
		for (size_t c = 0; c < nc; ++c) {
			if (joint_of[c] >= 0 && (rig.required(int(c)) || int(c) == anchor)) driven.push_back(c);
		}
		std::map<size_t, de::AnimSequence::Stream> streams;
		for (size_t c : driven) streams[c].channel = uint16_t(c);
		NodePose pose;
		std::vector<de::Quat> last(nc, {0, 0, 0, 0});
		for (int k = 0; k < keys; ++k) {
			float tn = float(k) / float(keys - 1);
			sample_animation(s, a, normalised ? tn : tn * duration, pose);
			std::map<int, gltf::Mat4> memo;
			std::vector<de::Transform> world(nc);
			std::vector<bool> have(nc, false);
			auto base_pose = out.bank.pose(*seq, tn);
			auto follow = [&](size_t c) {
				int j = joint_of[c];
				gltf::Mat4 ibm = gltf::inverse(joint_bind(plan, j));
				gltf::Mat4 G = gltf::multiply(gltf::multiply(F, gltf::multiply(posed_world(s, pose, j, memo), ibm)), Finv);
				return from_gltf(G, u).then(bn[c]);
			};
			for (size_t c : driven) world[c] = follow(c), have[c] = true;
			for (size_t c : driven) {
				int p = int(c) == body ? -1 : rig.bank.channels[c].parent;
				de::Transform parent;
				// A mapped root (an export's Body) carries the move's placement, which the game leaves out.
				if (p >= 0 && size_t(p) < nc) parent = have[size_t(p)] ? world[size_t(p)] : joint_of[size_t(p)] >= 0 ? follow(size_t(p)) : (p == body ? de::Transform{} : base_pose[size_t(p)]);
				de::Transform local = affine_inverse(parent).then(world[c]);
				de::Quat q = quat_of(local);
				if (q[0] * last[c][0] + q[1] * last[c][1] + q[2] * last[c][2] + q[3] * last[c][3] < 0)
					for (auto &x : q) x = -x;
				last[c] = q;
				streams[c].pos.push_back({tn, local.t});
				streams[c].rot.push_back({tn, q});
			}
		}
		// Moves that walk away from their start are played in place: the game's physics moves the
		// fighter, so the root bone loses its horizontal drift.
		for (auto &[c, st] : streams) {
			int p = rig.bank.channels[c].parent;
			if (body < 0 || p != body || joint_of[size_t(body)] >= 0 || st.pos.size() < 2) continue;
			V3 drift = flat(sub(st.pos.back().p, st.pos.front().p));
			if (length(drift) < rig.height * 0.05f) continue;
			for (auto &k : st.pos) k.p = sub(k.p, mul(drift, k.t));
		}
		seq->streams.erase(std::remove_if(seq->streams.begin(), seq->streams.end(), [&](const de::AnimSequence::Stream &st) { return streams.count(st.channel) > 0; }), seq->streams.end());
		for (auto &[c, st] : streams) seq->streams.push_back(std::move(st));
		std::sort(seq->streams.begin(), seq->streams.end(), [](auto &x, auto &y) { return x.channel < y.channel; });
		++out.supplied;
	}
	out.retargeted = out.bank.sequences.size() - out.supplied;
	out.rest = out.bank.pose(de::AnimSequence{}, 0);
	// Materials: an export's fighter stages come back as they were; otherwise the materials'
	// colours go into one atlas.
	std::set<int> materials;
	for (int k = 0; k < 3; ++k) {
		for (auto &pc : lod_pieces(plan, k)) materials.insert(pc.prim->material);
	}
	const json::Value *stages = nullptr;
	for (int m : materials) {
		if (!stages) stages = stages_of(s, m);
	}
	Atlas atlas;
	auto image_size = [&](int img, int *w, int *hh) {
		*w = *hh = 4;
		if (img >= 0 && size_t(img) < s.images.size() && s.images[size_t(img)].width > 0) {
			*w = build::pow2_at_most(s.images[size_t(img)].width, kMaxTexture);
			*hh = build::pow2_at_most(s.images[size_t(img)].height, kMaxTexture);
		}
	};
	auto set_image = [&](RobotImage &dst, int img, int w, int hh) {
		dst.width = w, dst.height = hh;
		dst.rgba = image_rgba(s, img, w, hh);
		dst.given = img >= 0;
	};
	auto blank = [](RobotImage &dst, int w, int hh, uint8_t v) {
		dst.width = w, dst.height = hh;
		dst.rgba.assign(size_t(w) * size_t(hh) * 4, v);
		for (size_t i = 3; i < dst.rgba.size(); i += 4) dst.rgba[i] = 255;
	};
	if (stages) {
		int mask_img = -1, refl_img = -1;
		int plain = 0;
		for (const json::Value &st : stages->items()) {
			std::string name = st.find("name") && st.find("name")->is_string() ? st.find("name")->as_string() : "";
			std::string cls = st.find("class") && st.find("class")->is_string() ? st.find("class")->as_string() : "TextureManager";
			int img = st.find("image") && st.find("image")->is_number() ? int(st.find("image")->as_int()) : -1;
			int set = st.find("uv_set") && st.find("uv_set")->is_number() ? int(st.find("uv_set")->as_int()) : -1;
			out.stage_layer[name] = cls == "TextureManager" ? set : -1;
			if (cls != "TextureManager") continue;
			if (plain == 0) mask_img = img;
			else if (plain == 1) refl_img = img;
			++plain;
		}
		int w, hh;
		image_size(mask_img, &w, &hh);
		set_image(out.mask, mask_img, w, hh);
		int rw, rh;
		image_size(refl_img, &rw, &rh);
		if (refl_img >= 0) set_image(out.reflection, refl_img, rw, rh);
		else blank(out.reflection, w, hh, 0);
	} else {
		// Tiles: each material's texture or colour.
		std::vector<int> mats(materials.begin(), materials.end());
		std::set<int> textures;
		for (int m : mats) {
			if (m >= 0 && s.materials[size_t(m)].base_texture) textures.insert(s.materials[size_t(m)].base_texture.image);
		}
		bool one = textures.size() == 1;
		for (int m : mats) {
			if (m < 0 || !s.materials[size_t(m)].base_texture) one = false;
		}
		if (one) {
			atlas.identity = true;
			int img = *textures.begin(), w, hh;
			image_size(img, &w, &hh);
			atlas.size = atlas.tile = std::max(w, hh);
			Tile t;
			t.image = img;
			for (int m : mats) atlas.tile_of[m] = 0, t.material = m, t.colour = s.materials[size_t(m)].base_colour;
			atlas.tiles.push_back(t);
		} else {
			int n = std::max<int>(1, int(mats.size()));
			while (atlas.grid * atlas.grid < n) atlas.grid *= 2;
			atlas.size = textures.empty() ? std::max(64, 16 * atlas.grid) : kMaxTexture;
			atlas.tile = atlas.size / atlas.grid;
			for (size_t i = 0; i < mats.size(); ++i) {
				Tile t;
				t.material = mats[i];
				if (mats[i] >= 0) {
					t.colour = s.materials[size_t(mats[i])].base_colour;
					if (s.materials[size_t(mats[i])].base_texture) t.image = s.materials[size_t(mats[i])].base_texture.image;
				}
				t.col = int(i) % atlas.grid, t.row = int(i) / atlas.grid;
				atlas.tile_of[mats[i]] = atlas.tiles.size();
				atlas.tiles.push_back(t);
			}
		}
		const int A = atlas.size;
		std::vector<uint8_t> colours(size_t(A) * size_t(A) * 4, 0), mask(size_t(A) * size_t(A) * 4, 0);
		for (size_t i = 3; i < mask.size(); i += 4) mask[i] = colours[i] = 255;
		for (size_t ti = 0; ti < atlas.tiles.size(); ++ti) {
			const Tile &t = atlas.tiles[ti];
			std::vector<uint8_t> px = t.image >= 0 ? image_rgba(s, t.image, atlas.tile, atlas.tile) : std::vector<uint8_t>(size_t(atlas.tile) * size_t(atlas.tile) * 4, 255);
			const int channel = int(ti % 3);
			for (int y = 0; y < atlas.tile; ++y) {
				for (int x = 0; x < atlas.tile; ++x) {
					size_t src = (size_t(y) * size_t(atlas.tile) + size_t(x)) * 4;
					size_t dst = ((size_t(t.row) * size_t(atlas.tile) + size_t(y)) * size_t(A) + size_t(t.col) * size_t(atlas.tile) + size_t(x)) * 4;
					float lum = 0;
					for (int k = 0; k < 3; ++k) {
						float v = float(px[src + size_t(k)]) * t.colour[size_t(k)];
						colours[dst + size_t(k)] = uint8_t(std::clamp(v, 0.0f, 255.0f));
						lum += v * (k == 0 ? 0.3f : k == 1 ? 0.59f : 0.11f);
					}
					mask[dst + size_t(channel)] = uint8_t(std::clamp(lum, 0.0f, 255.0f));
				}
			}
		}
		int given_mask = find_image(s, {"paintmask", "rgbmain"}, "de_paint_mask", materials);
		if (given_mask >= 0 && atlas.identity) {
			set_image(out.mask, given_mask, A, A);
		} else {
			out.mask.width = out.mask.height = A;
			out.mask.rgba = mask;
			if (h.paint == HarPlan::Paint::Colours) std::fill(out.mask.rgba.begin(), out.mask.rgba.end(), 0);
			for (size_t i = 3; i < out.mask.rgba.size(); i += 4) out.mask.rgba[i] = 255;
		}
		if (h.paint == HarPlan::Paint::Colours) {
			out.colours.width = out.colours.height = A;
			out.colours.rgba = colours;
		}
		int refl = find_image(s, {"reflectionmask", "reflection"}, "de_reflection_mask", materials);
		if (refl >= 0 && atlas.identity) set_image(out.reflection, refl, A, A);
		else blank(out.reflection, A, A, 0);
	}
	int bump = find_image(s, {"bump"}, "de_bump", materials);
	if (bump >= 0) {
		int w, hh;
		image_size(bump, &w, &hh);
		set_image(out.bump, bump, w, hh);
	}
	if (s.extras.is_object()) {
		if (const json::Value *mi = s.extras.find("de_model_images"); mi && mi->is_object()) {
			if (const json::Value *b = mi->find("Bump"); b && b->is_number() && size_t(b->as_int()) < s.images.size()) {
				int w, hh;
				image_size(int(b->as_int()), &w, &hh);
				set_image(out.bump, int(b->as_int()), w, hh);
			}
			if (const json::Value *b = mi->find("RGBScaled"); b && b->is_number() && size_t(b->as_int()) < s.images.size() && stages) {
				int w, hh;
				image_size(int(b->as_int()), &w, &hh);
				set_image(out.scaled, int(b->as_int()), w, hh);
			}
		}
	}
	// The mesh of each LOD: welded by position and skin, per-face UVs.
	auto bone_of = [&](int node) -> int {
		for (int n = node; n >= 0; n = s.nodes[size_t(n)].parent) {
			for (size_t c = 0; c < nc; ++c) {
				if (joint_of[c] == n && rig.deformer[c] >= 0) return rig.deformer[c];
			}
		}
		int pc = rig.channel("Pelvis");
		return pc >= 0 && rig.deformer[size_t(pc)] >= 0 ? rig.deformer[size_t(pc)] : 0;
	};
	std::map<int, int> bone_memo;
	auto bone_cached = [&](int node) {
		auto it = bone_memo.find(node);
		if (it != bone_memo.end()) return it->second;
		return bone_memo[node] = bone_of(node);
	};
	auto atlas_uv = [&](int material, const gltf::Primitive &p, uint32_t gi, const std::array<float, 2> &shift) -> std::array<float, 2> {
		auto ti = atlas.tile_of.find(material);
		if (ti == atlas.tile_of.end() || atlas.tiles.empty()) return {0.5f, 0.5f};
		const Tile &t = atlas.tiles[ti->second];
		int set = material >= 0 ? s.materials[size_t(material)].base_texture.uv_set : 0;
		std::array<float, 2> uv{0.5f, 0.5f};
		bool textured = t.image >= 0;
		if (textured && size_t(set) < p.uvs.size() && gi < p.uvs[size_t(set)].size()) uv = {p.uvs[size_t(set)][gi][0], p.uvs[size_t(set)][gi][1]};
		if (atlas.identity) return uv;
		if (!textured) uv = {0.5f, 0.5f};
		else {
			const float inset = 0.5f / float(atlas.tile);
			uv = {std::clamp(uv[0] - shift[0], inset, 1 - inset), std::clamp(uv[1] - shift[1], inset, 1 - inset)};
		}
		return {(float(t.col) + uv[0]) / float(atlas.grid), (float(t.row) + uv[1]) / float(atlas.grid)};
	};
	size_t layers = 1;
	if (stages) {
		for (auto &[n, l] : out.stage_layer) layers = std::max(layers, size_t(std::max(l, 0)) + 1);
	}
	auto build_lod = [&](int lod) {
		LodData d;
		d.uvs.resize(layers);
		auto pieces = lod_pieces(plan, lod);
		// Exact rebuild of an export: its vertex numbering, face order and smoothing groups.
		const gltf::Mesh *source = nullptr;
		if (plan.from_editor && plan.har.lods[size_t(lod)].size() == 1) {
			int n = plan.har.lods[size_t(lod)][0];
			source = &s.meshes[size_t(s.nodes[size_t(n)].mesh)];
			for (auto &pc : pieces) {
				auto src = pc.prim->custom.find("_DE_VERTEX");
				const json::Value *faces = pc.prim->extras.is_object() ? pc.prim->extras.find("de_faces") : nullptr;
				if (src == pc.prim->custom.end() || src->second.size() != pc.prim->positions.size() || !faces || !faces->is_array() || faces->size() * 3 != pc.prim->indices.size()) source = nullptr;
			}
		}
		d.exact = source != nullptr;
		std::map<std::vector<int32_t>, uint32_t> weld;
		struct Tri {
			std::array<uint32_t, 3> v;
			std::vector<UV3> uv;
			std::array<V3, 3> n;
			int face = -1;
		};
		std::vector<Tri> tris;
		for (auto &pc : pieces) {
			const auto &P = *pc.prim;
			const gltf::Mat4 M = gltf::multiply(F, pc.m);
			const bool mirrored = det3(M) < 0;
			const gltf::Mat4 NM = gltf::inverse(M);
			// Per glTF vertex: DE position and quantised skin.
			std::vector<uint32_t> index(P.positions.size());
			auto src = P.custom.find("_DE_VERTEX");
			for (size_t v = 0; v < P.positions.size(); ++v) {
				V3 p = from_gltf(gltf::transform_point(M, P.positions[v]), u);
				std::vector<float> w(nb, 0.0f);
				if (pc.skinned && v < P.joints.size() && v < P.weights.size()) {
					for (int k = 0; k < 4; ++k) {
						uint16_t jk = P.joints[v][size_t(k)];
						float wt = P.weights[v][size_t(k)];
						if (wt <= 0 || jk >= joints.size()) continue;
						w[size_t(bone_cached(joints[jk]))] += wt;
					}
				}
				float sum = 0;
				for (float x : w) sum += x;
				if (sum <= 0) w[size_t(bone_cached(pc.skinned ? -1 : pc.node))] = 1, sum = 1;
				std::vector<uint8_t> q(nb, 0);
				for (size_t b = 0; b < nb; ++b) q[b] = uint8_t(std::clamp(int(std::lround(w[b] / sum * 255)), 0, 255));
				if (d.exact) {
					uint32_t dv = uint32_t(src->second[v]);
					if (d.verts.size() <= dv) d.verts.resize(dv + 1, p), d.skin.resize(dv + 1, q);
					d.verts[dv] = p, d.skin[dv] = q;
					index[v] = dv;
					continue;
				}
				std::vector<int32_t> key{int32_t(std::lround(p[0] * 1000)), int32_t(std::lround(p[1] * 1000)), int32_t(std::lround(p[2] * 1000))};
				for (uint8_t x : q) key.push_back(x);
				auto [it, fresh] = weld.try_emplace(key, uint32_t(d.verts.size()));
				if (fresh) d.verts.push_back(p), d.skin.push_back(q);
				index[v] = it->second;
			}
			const json::Value *faces = P.extras.is_object() ? P.extras.find("de_faces") : nullptr;
			for (size_t t = 0; t + 2 < P.indices.size(); t += 3) {
				const int order[3] = {0, mirrored ? 1 : 2, mirrored ? 2 : 1};
				Tri tr;
				std::array<uint32_t, 3> gi;
				for (int k = 0; k < 3; ++k) gi[size_t(k)] = P.indices[t + size_t(order[k])], tr.v[size_t(k)] = index[gi[size_t(k)]];
				if (!d.exact && (tr.v[0] == tr.v[1] || tr.v[1] == tr.v[2] || tr.v[0] == tr.v[2])) continue;
				if (d.exact && faces) tr.face = int(faces->items()[t / 3].as_int());
				tr.uv.resize(layers);
				if (stages) {
					for (size_t l = 0; l < layers; ++l) {
						for (int k = 0; k < 3; ++k) {
							if (l < P.uvs.size() && gi[size_t(k)] < P.uvs[l].size()) tr.uv[l][size_t(k)] = {P.uvs[l][gi[size_t(k)]][0], P.uvs[l][gi[size_t(k)]][1]};
						}
					}
				} else {
					// A triangle's UVs move by whole tiles into 0..1 before they go into the atlas.
					std::array<float, 2> shift{0, 0};
					int set = P.material >= 0 ? s.materials[size_t(P.material)].base_texture.uv_set : 0;
					if (!atlas.identity && size_t(set) < P.uvs.size()) {
						float mu = 1e30f, mv = 1e30f;
						for (int k = 0; k < 3; ++k) {
							if (gi[size_t(k)] < P.uvs[size_t(set)].size()) mu = std::min(mu, P.uvs[size_t(set)][gi[size_t(k)]][0]), mv = std::min(mv, P.uvs[size_t(set)][gi[size_t(k)]][1]);
						}
						if (mu < 1e29f) shift = {std::floor(mu), std::floor(mv)};
					}
					for (int k = 0; k < 3; ++k) tr.uv[0][size_t(k)] = atlas_uv(P.material, P, gi[size_t(k)], shift);
				}
				for (int k = 0; k < 3; ++k) {
					V3 n{0, 0, 0};
					if (gi[size_t(k)] < P.normals.size()) {
						const auto &q = P.normals[gi[size_t(k)]];
						gltf::Vec3 tn{NM[0] * q[0] + NM[1] * q[1] + NM[2] * q[2], NM[4] * q[0] + NM[5] * q[1] + NM[6] * q[2], NM[8] * q[0] + NM[9] * q[1] + NM[10] * q[2]};
						n = normalize(V3{tn[0], tn[1], -tn[2]});
					}
					tr.n[size_t(k)] = n;
				}
				tris.push_back(std::move(tr));
			}
		}
		if (d.exact) {
			std::vector<Tri> ordered(tris.size());
			std::vector<bool> filled(tris.size(), false);
			for (auto &t : tris) {
				if (t.face < 0 || size_t(t.face) >= ordered.size() || filled[size_t(t.face)]) {
					d.exact = false;
					break;
				}
				filled[size_t(t.face)] = true;
				ordered[size_t(t.face)] = t;
			}
			if (d.exact) tris = std::move(ordered);
			const json::Value *ff = source->extras.is_object() ? source->extras.find("de_face_flags") : nullptr;
			if (d.exact && ff && ff->is_array() && ff->size() == tris.size()) {
				for (auto &f : ff->items()) d.flags.push_back(uint32_t(f.as_int()));
			}
		}
		bool normals = true;
		for (auto &t : tris) {
			d.faces.push_back(t.v);
			for (size_t l = 0; l < layers; ++l) d.uvs[l].push_back(t.uv[l]);
			for (auto &n : t.n) normals = normals && length(n) > 0.5f;
			d.normals.push_back(t.n);
		}
		if (!normals) d.normals.clear();
		return d;
	};
	const size_t lod_count = lod0_only ? 1 : std::min<size_t>(3, rig.lods.size());
	LodData lod0 = build_lod(0);
	if (lod0.faces.empty()) throw std::runtime_error("LOD0 has no triangles");
	size_t target0 = lod0.faces.size();
	if (h.decimate_lod0) target0 = std::min(target0, rig.lods[0].faces);
	if (lod0.verts.size() > kMaxMeshVertices || target0 > 65000) target0 = std::min<size_t>(target0, 60000);
	auto fits = [](const LodData &d) {
		std::set<uint32_t> used;
		for (auto &f : d.faces) used.insert(f.begin(), f.end());
		return used.size() <= kMaxMeshVertices && d.faces.size() <= kMaxMeshFaces;
	};
	if (target0 < lod0.faces.size() || !fits(lod0)) {
		lod0 = decimate(lod0, target0);
		while (!fits(lod0)) lod0 = decimate(lod0, lod0.faces.size() * 3 / 4);
		out.notes.push_back("LOD0 decimated to " + std::to_string(lod0.faces.size()) + " triangles");
	}
	out.lods.push_back(finish_lod(lod0));
	for (size_t k = 1; k < lod_count; ++k) {
		if (!h.lods[k].empty()) {
			LodData d = build_lod(int(k));
			if (!fits(d)) d = decimate(d, 60000);
			out.lods.push_back(finish_lod(d));
			continue;
		}
		size_t target = std::min(rig.lods[k].faces, lod0.faces.size());
		LodData d = decimate(lod0, target);
		out.notes.push_back("LOD" + std::to_string(k) + " generated: " + std::to_string(d.faces.size()) + " triangles");
		out.lods.push_back(finish_lod(d));
	}
	// Spheres: authored ones where the file has them, else the base's, scaled with their bone.
	auto bone_scale = [&](int c) {
		for (int k : children[size_t(c)]) {
			if (!rig.required(k)) continue;
			float l0 = length(sub(jb[size_t(k)], jb[size_t(c)]));
			if (l0 > 1e-3f) return length(sub(jn[size_t(k)], jn[size_t(c)])) / l0;
		}
		return sc;
	};
	for (size_t i = 0; i < rig.spheres.size(); ++i) {
		const RobotRig::Sphere &rs = rig.spheres[i];
		RobotBuild::Sphere o;
		o.index = i;
		o.local = rs.local;
		o.hotspot = rs.hotspot * sc;
		o.falloff = rs.falloff * sc;
		const SphereSource *file = nullptr;
		for (auto &sp : h.spheres) {
			if (sp.sphere == rs.name && sp.known && !sp.bone.empty()) file = &sp;
		}
		if (file) {
			int c = rig.channel(file->bone);
			gltf::Mat4 wm = gltf::multiply(F, s.world(file->node));
			V3 p = from_gltf(node_origin(wm), u);
			gltf::Vec3 t, sz;
			gltf::Vec4 r;
			gltf::decompose(wm, &t, &r, &sz);
			float scale = std::max({std::fabs(sz[0]), std::fabs(sz[1]), std::fabs(sz[2])}) * u;
			o.bone = file->bone;
			o.local.t = bn[size_t(c)].inverse().apply(p);
			o.local.r = de::Transform{}.r;
			o.hotspot = rs.falloff > 0 ? scale * rs.hotspot / rs.falloff : 0;
			o.falloff = scale;
		} else if (rs.channel >= 0 && rig.required(rs.channel)) {
			o.local.t = mul(rs.local.t, bone_scale(rs.channel));
		} else {
			o.local.t = mul(rs.local.t, sc);
		}
		out.spheres.push_back(o);
	}
	return out;
}

// ---------------------------------------------------------------------------------------------
// Writing

namespace {

Bytes gb(const Guid &g) { return Bytes(g.b, g.b + 16); }

void replace_all(Bytes &b, const Bytes &from, const Bytes &to) {
	if (from.empty() || b.size() < from.size()) return;
	for (size_t i = 0; i + from.size() <= b.size();) {
		if (std::equal(from.begin(), from.end(), b.begin() + long(i))) {
			std::copy(to.begin(), to.end(), b.begin() + long(i));
			i += from.size();
		} else {
			++i;
		}
	}
}

void touched(deo::Object &o) {
	o.packed.clear();
	o.packed_as_is = false;
}

void put(Bytes &b, size_t at, float f) { memcpy(b.data() + at, &f, 4); }

Bytes bank_contents(const std::string &name, const de::AnimBank &bank, const std::vector<std::array<uint8_t, 2>> &flags) {
	deo::Writer w;
	size_t at = w.begin_chunk(4);
	w.block(build::stored(name));
	w.u32(uint32_t(bank.sequences.size()));
	for (auto &seq : bank.sequences) {
		w.block(build::stored(seq.name));
		w.u32(uint32_t(seq.streams.size()));
		for (auto &st : seq.streams) {
			w.u16(st.channel);
			size_t sat = w.begin_chunk(1);
			w.u32(uint32_t(st.pos.size()));
			for (auto &k : st.pos) {
				build::put_f32(w, k.t);
				for (float v : k.p) build::put_f32(w, v);
			}
			w.u32(uint32_t(st.rot.size()));
			for (auto &k : st.rot) {
				build::put_f32(w, k.t);
				for (float v : k.q) build::put_f32(w, v);
			}
			w.end_chunk(sat);
		}
	}
	w.u32(uint32_t(bank.channels.size()));
	for (size_t c = 0; c < bank.channels.size(); ++c) {
		const auto &ch = bank.channels[c];
		w.block(build::stored(ch.name));
		w.u8(c < flags.size() ? flags[c][0] : 0);
		w.u8(c < flags.size() ? flags[c][1] : 0);
		w.u16(uint16_t(ch.parent));
		for (float v : ch.rest_pos) build::put_f32(w, v);
		for (float v : ch.rest_rot) build::put_f32(w, v);
	}
	w.end_chunk(at);
	return w.data();
}

// An image's pixels in place, in the channels its header lists (du/dv bump channels flat, or from
// the image's slopes when it is a height map).
void replace_image(Project &p, uint32_t index, const RobotImage &img, bool bump_from_height) {
	deo::Object &o = p.file().objects.at(index);
	Chunks ic = split_chunks(o.contents);
	if (ic.chunks.empty()) return;
	deo::Reader r(ic.chunks[0].body);
	r.block();
	size_t at = r.pos();
	if (ic.chunks[0].body.size() < at + 30) return;
	int32_t wh[2] = {img.width, img.height};
	memcpy(ic.chunks[0].body.data() + at + 4, wh, 8);
	uint32_t mask;
	memcpy(&mask, ic.chunks[0].body.data() + at + 4 + 8 + 1 + 4 + 1, 4);
	if (!mask) mask = 7;
	o.contents = join_chunks(ic);
	touched(o);
	std::vector<deo::Ex> ex;
	for (auto &e : o.ex) {
		bool pixels = e.key.size() == 2 && e.key[0] == 0;
		std::string key = text_of(e.key);
		if (!pixels && key != "ImportPath" && key != "OBJImportFilename") ex.push_back(e);
	}
	const size_t n = size_t(img.width) * size_t(img.height);
	auto lum = [&](size_t i) { return (img.rgba[i * 4] * 30 + img.rgba[i * 4 + 1] * 59 + img.rgba[i * 4 + 2] * 11) / 100; };
	deo::Writer plain;
	for (int ch = 0; ch < 8; ++ch) {
		if (!(mask & (1u << ch))) continue;
		plain.u32(uint32_t(ch));
		for (size_t i = 0; i < n; ++i) {
			uint8_t v = 0;
			if (ch < 4) v = img.rgba[i * 4 + size_t(ch)];
			else if (ch == 4) v = uint8_t(lum(i));
			else if (ch == 6 || ch == 7) {
				v = 0x7f;
				if (bump_from_height) {
					size_t x = i % size_t(img.width), y = i / size_t(img.width);
					size_t a = ch == 6 ? y * size_t(img.width) + (x + 1) % size_t(img.width) : ((y + 1) % size_t(img.height)) * size_t(img.width) + x;
					v = uint8_t(std::clamp(0x7f + (int(lum(a)) - int(lum(i))) / 2, 0, 255));
				}
			}
			plain.u8(v);
		}
	}
	plain.u32(0xFFFFFFFFu);
	deo::Writer stream;
	stream.u8(0);
	stream.block(plain.data());
	deo::Writer payload;
	payload.u8(2);
	payload.block(stream.data());
	deo::Ex px;
	px.key = {0, 0};
	px.data = payload.data();
	ex.insert(ex.begin(), px);
	o.ex = std::move(ex);
}

void set_ex(deo::Object &o, const std::string &key, const Bytes &data) {
	for (auto &e : o.ex) {
		if (text_of(e.key) == key) {
			e.data = data;
			return;
		}
	}
	deo::Ex e;
	e.key = text_bytes(key);
	e.data = data;
	o.ex.push_back(e);
}

void drop_ex(deo::Object &o, const std::string &key) {
	o.ex.erase(std::remove_if(o.ex.begin(), o.ex.end(), [&](const deo::Ex &e) { return text_of(e.key) == key; }), o.ex.end());
}

// A texture manager's face count (contents [name][u32]) and its per-face UVs.
void write_manager(Project &p, uint32_t tm, const RobotLod &lod, int layer) {
	deo::Object &o = p.file().objects[tm];
	const bool reflection = o.info.class_text() == "TextureManager\\Reflection";
	const bool with = !reflection && layer >= 0 && size_t(layer) < lod.uvs.size() && lod.uvs[size_t(layer)].size() == lod.faces.size();
	uint32_t n = with ? uint32_t(lod.faces.size()) : 0;
	Chunks c = split_chunks(o.contents);
	if (!c.chunks.empty()) {
		deo::Reader r(c.chunks[0].body);
		r.block();
		size_t at = r.pos();
		if (c.chunks[0].body.size() >= at + 4) memcpy(c.chunks[0].body.data() + at, &n, 4);
		o.contents = join_chunks(c);
	}
	touched(o);
	if (reflection) {
		drop_ex(o, "ObjExtCon");
		return;
	}
	deo::Writer e;
	size_t at = e.begin_chunk(1);
	e.u32(n);
	for (uint32_t f = 0; f < n; ++f) {
		e.u32(f);
		for (auto &uv : lod.uvs[size_t(layer)][f]) build::put_f32(e, uv[0]), build::put_f32(e, uv[1]);
	}
	e.end_chunk(at);
	set_ex(o, "ObjExtCon", e.data());
}

void write_weights(Project &p, uint32_t channel, const std::vector<uint8_t> &w) {
	deo::Writer b;
	b.u32(1);
	b.u32(uint32_t(w.size()));
	b.bytes(Bytes(w.begin(), w.end()));
	deo::Object &o = p.file().objects[channel];
	Chunks c = split_chunks(o.contents);
	if (c.chunks.size() < 2) return;
	c.chunks[1].body = b.data();
	c.chunks[1].size = {};
	o.contents = join_chunks(c);
	touched(o);
}

void write_mesh(Project &p, uint32_t mesh, const RobotLod &lod, const RobotBuild &b, const RobotRig &rig) {
	{
		deo::Object &o = p.file().objects[mesh];
		Chunks c = split_chunks(o.contents);
		if (c.chunks.size() >= 2 && c.chunks[1].body.size() >= 8) {
			uint32_t v = uint32_t(lod.vertices.size()), f = uint32_t(lod.faces.size());
			memcpy(c.chunks[1].body.data(), &v, 4);
			memcpy(c.chunks[1].body.data() + 4, &f, 4);
			o.contents = join_chunks(c);
		}
		touched(o);
		set_ex(o, "ObjExtCon", build::mesh_geometry(lod.vertices, lod.faces));
	}
	bool first = true;
	std::map<std::string, uint32_t> channels;
	for (uint32_t c : p.children(mesh)) {
		const deo::Object &co = p.file().objects[c];
		std::string cls = co.info.class_text();
		if (cls == "TextureProcess\\MultiStage") {
			for (uint32_t tm : p.children(c)) {
				std::string tcls = p.file().objects[tm].info.class_text();
				if (tcls.rfind("TextureManager", 0) != 0) continue;
				std::string name = p.file().objects[tm].info.name_text();
				int layer = 0;
				if (auto it = b.stage_layer.find(name); it != b.stage_layer.end()) layer = it->second;
				write_manager(p, tm, lod, first ? layer : -1);
			}
			first = false;
		} else if (cls == "DataChannel\\UnitFloats") {
			channels[co.info.name_text()] = c;
		}
	}
	std::set<uint32_t> written;
	for (size_t i = 0; i < rig.skin.size(); ++i) {
		std::string name = "Deform" + rig.skin[i].name + "VertexData";
		auto it = channels.find(name);
		uint32_t ch;
		if (it != channels.end()) {
			ch = it->second;
		} else if (!channels.empty()) {
			deo::Object copy = p.file().objects[channels.begin()->second];
			copy.info.guid = fresh_guid();
			copy.hierarchy = {1, 1, 0};
			p.file().objects.push_back(copy);
			ch = uint32_t(p.file().objects.size() - 1);
			build::rename(p, ch, name);
			build::add_child(p, mesh, ch);
		} else {
			continue;
		}
		write_weights(p, ch, i < lod.weights.size() ? lod.weights[i] : std::vector<uint8_t>(lod.vertices.size(), 0));
		written.insert(ch);
	}
	for (auto &[n, ch] : channels) {
		if (!written.count(ch)) write_weights(p, ch, std::vector<uint8_t>(lod.vertices.size(), 0));
	}
}

size_t model_fighter_at(const deo::Object &m) {
	Chunks c = split_chunks(m.contents);
	if (c.chunks.size() < 2) return SIZE_MAX;
	deo::Reader r(m.contents);
	size_t e;
	r.chunk(&e);
	r.seek(e);
	r.chunk(&e);
	size_t off = r.pos();
	return e - off < 16 ? SIZE_MAX : off;
}

}  // namespace

ImportResult import_robot(Project &p, const ImportPlan &plan) {
	if (plan.errors()) throw deo::Error("the import has errors; see the list");
	const HarPlan &h = plan.har;
	if (!h.rig) throw deo::Error("no base robot");
	const RobotRig &rig = *h.rig;
	RobotBuild b = build_robot(plan);
	ImportResult res;
	const std::string name = plan.settings.name.empty() ? "Imported Robot" : plan.settings.name;
	const size_t first_new = p.file().objects.size();
	uint32_t desc = create_har_variant(p, rig.robot, name);
	const size_t end = p.file().objects.size();
	HarObjects o = har_objects(p, desc);
	if (o.fighter < 0) throw deo::Error("the HAR variant has no fighter");
	auto &objs = p.file().objects;
	auto in_block = [&](size_t i) { return i >= first_new && i < end; };
	int bank = -1, deform = -1;
	for (size_t i = first_new; i < end; ++i) {
		std::string cls = objs[i].info.class_text();
		if (bank < 0 && cls == "AnimationSequenceGroup" && objs[i].info.name_text() == rig.bank_name) bank = int(i);
	}
	for (uint32_t c : p.children(uint32_t(o.fighter))) {
		if (deform < 0 && objs[c].info.class_text() == "ActorDeformInfo" && objs[c].info.name_text() == rig.deform_name) deform = int(c);
	}
	if (bank < 0 || deform < 0) throw deo::Error("the copy has no animation bank or deform info");
	{
		deo::Object &bo = objs[size_t(bank)];
		Chunks c = split_chunks(bo.contents);
		Bytes chunk = bank_contents(rig.bank_name, b.bank, rig.channel_flags);
		Chunks fresh = split_chunks(chunk);
		if (c.chunks.empty() || fresh.chunks.empty()) throw deo::Error("unexpected animation bank layout");
		c.chunks[0] = fresh.chunks[0];
		bo.contents = join_chunks(c);
		touched(bo);
	}
	{
		deo::Object &d = objs[size_t(deform)];
		Chunks c = split_chunks(d.contents);
		if (c.chunks.empty()) throw deo::Error("unexpected deform info layout");
		Bytes &body = c.chunks[0].body;
		deo::Reader r(body);
		r.block();
		for (size_t i = 0; i < b.binds.size() && r.remaining(); ++i) {
			if (!r.u8()) break;
			size_t ce;
			r.chunk(&ce);
			size_t at = r.pos();
			if (ce - at < 48) break;
			for (int k = 0; k < 3; ++k) put(body, at + size_t(k) * 4, b.binds[i].t[size_t(k)]);
			for (int k = 0; k < 9; ++k) put(body, at + 12 + size_t(k) * 4, b.binds[i].r[size_t(k)]);
			r.seek(ce);
		}
		d.contents = join_chunks(c);
		touched(d);
	}
	// Bone actors at the new rest; the spheres moved and scaled.
	std::vector<int> parents = object_parents(p.file());
	std::function<void(uint32_t, const de::Transform &)> place = [&](uint32_t i, const de::Transform &world) {
		for (uint32_t c : p.children(i)) {
			if (objs[c].info.class_text() != "Actor") continue;
			int ch = rig.channel(objs[c].info.name_text());
			de::Transform w = world.then(actor_local(objs[c]));
			if (ch >= 0) {
				w = b.rest[size_t(ch)];
				set_actor_local(p, c, affine_inverse(world).then(w));
			}
			place(c, w);
		}
	};
	place(uint32_t(o.fighter), de::Transform{});
	std::vector<uint32_t> spheres;
	std::function<void(uint32_t)> collect = [&](uint32_t i) {
		for (uint32_t c : p.children(i)) {
			if (objs[c].info.class_text() == "Actor\\Affector\\Sphere") spheres.push_back(c);
			collect(c);
		}
	};
	collect(uint32_t(o.fighter));
	if (spheres.size() == rig.spheres.size()) {
		std::map<std::string, uint32_t> bones;
		std::function<void(uint32_t)> find_bones = [&](uint32_t i) {
			for (uint32_t c : p.children(i)) {
				if (objs[c].info.class_text() == "Actor" && rig.channel(objs[c].info.name_text()) >= 0) bones.emplace(objs[c].info.name_text(), c);
				find_bones(c);
			}
		};
		find_bones(uint32_t(o.fighter));
		for (const auto &sp : b.spheres) {
			uint32_t obj = spheres[sp.index];
			if (objs[obj].info.name_text() != rig.spheres[sp.index].name) continue;
			if (!sp.bone.empty() && bones.count(sp.bone) && parents[obj] >= 0 && uint32_t(parents[obj]) != bones[sp.bone]) {
				uint32_t old = uint32_t(parents[obj]);
				auto kids = p.children(old);
				kids.erase(std::remove(kids.begin(), kids.end(), obj), kids.end());
				build::set_children(p, old, kids);
				build::add_child(p, bones[sp.bone], obj);
			}
			set_actor_local(p, obj, sp.local);
			Chunks cs = split_chunks(objs[obj].contents);
			if (cs.chunks.size() >= 3 && cs.chunks[2].body.size() >= 8) {
				put(cs.chunks[2].body, 0, sp.hotspot);
				put(cs.chunks[2].body, 4, sp.falloff);
				objs[obj].contents = join_chunks(cs);
				touched(objs[obj]);
			}
		}
	} else {
		res.notes.push_back("the copy's spheres differ from " + rig.robot + "'s; they are left as they were");
	}
	// The model: a copy of the base's default one, rewritten.
	const BaseData &base = p.base();
	const BaseData::FileIndex *mfile = base.file(rig.model_stem);
	if (!mfile) throw deo::Error("no model file " + rig.model_stem);
	size_t mfi = size_t(mfile - base.files().data());
	int base_model = -1;
	for (auto &bo : mfile->objects) {
		if (bo.class_path == "FighterModel" && bo.name == rig.model_name) base_model = int(bo.index);
	}
	if (base_model < 0) throw deo::Error("no model " + rig.model_name);
	std::map<uint32_t, uint32_t> map;
	const size_t model_first = objs.size();
	uint32_t model = p.copy_from_base(mfi, {uint32_t(base_model)}, &map, true).at(0);
	{
		const deo::File &src = base.model(mfi);
		for (auto &[from, to] : map) {
			Bytes og = gb(src.objects[from].info.guid), ng = gb(objs[to].info.guid);
			Bytes fp = gb(src.master.guid), tp = gb(p.agent());
			fp.insert(fp.end(), og.begin(), og.end());
			tp.insert(tp.end(), ng.begin(), ng.end());
			for (size_t i = model_first; i < objs.size(); ++i) replace_all(objs[i].contents, fp, tp);
		}
	}
	build::rename(p, model, name + " Model");
	// The fighter description and its first paint job name the new model; the base's other paint
	// jobs leave this fighter (their meshes do not fit its rig).
	{
		Bytes tp = gb(p.agent());
		Bytes ng = gb(objs[model].info.guid);
		tp.insert(tp.end(), ng.begin(), ng.end());
		std::vector<uint32_t> targets{desc};
		if (!o.model_descriptions.empty()) targets.push_back(uint32_t(o.model_descriptions[0]));
		for (auto &bo : mfile->objects) {
			if (bo.class_path != "FighterModel") continue;
			Bytes fp = gb(mfile->agent);
			fp.insert(fp.end(), bo.guid.b, bo.guid.b + 16);
			for (uint32_t t : targets) replace_all(objs[t].contents, fp, tp), touched(objs[t]);
		}
		for (size_t k = 1; k < o.model_descriptions.size(); ++k) {
			deo::Object &md = objs[size_t(o.model_descriptions[k])];
			size_t at = model_fighter_at(md);
			if (at != SIZE_MAX) std::fill(md.contents.begin() + long(at), md.contents.begin() + long(at) + 16, 0), touched(md);
		}
	}
	std::vector<std::pair<size_t, uint32_t>> meshes;
	for (uint32_t c : p.children(model)) {
		if (objs[c].info.class_text() == "Mesh\\Standard") meshes.push_back({mesh_counts(objs[c]).first, c});
	}
	std::sort(meshes.begin(), meshes.end(), [](auto &x, auto &y) { return x.first > y.first; });
	for (size_t k = 0; k < meshes.size(); ++k) {
		const RobotLod &lod = b.lods[std::min(k, b.lods.size() - 1)];
		write_mesh(p, meshes[k].second, lod, b, rig);
		++res.meshes;
	}
	// Images: the paint mask, reflection mask and bump in place; RGBScaled blank; RGBStatic added.
	std::vector<uint32_t> todo = p.children(model);
	bool have_static = false;
	while (!todo.empty()) {
		uint32_t i = todo.back();
		todo.pop_back();
		for (uint32_t c : p.children(i)) todo.push_back(c);
		if (objs[i].info.class_text() != "Image\\Standard") continue;
		std::string n = objs[i].info.name_text();
		if (n.rfind("RGBMain", 0) == 0) replace_image(p, i, b.mask, false), ++res.images;
		else if (n.rfind("ReflectionMask", 0) == 0) replace_image(p, i, b.reflection, false), ++res.images;
		else if (n.rfind("RGBScaled", 0) == 0 && b.scaled.given) {
			replace_image(p, i, b.scaled, false), ++res.images;
		} else if (n.rfind("RGBScaled", 0) == 0) {
			RobotImage black;
			black.width = b.mask.width, black.height = b.mask.height;
			black.rgba.assign(size_t(black.width) * size_t(black.height) * 4, 0);
			replace_image(p, i, black, false), ++res.images;
		} else if (n.rfind("RGBStatic", 0) == 0 && !b.colours.rgba.empty()) {
			replace_image(p, i, b.colours, false), have_static = true, ++res.images;
		} else if (n == "Bump") {
			RobotImage img = b.bump;
			if (img.rgba.empty()) {
				img.width = img.height = 4;
				img.rgba.assign(64, 128);
			}
			replace_image(p, i, img, img.given), ++res.images;
		}
	}
	if (!b.colours.rgba.empty() && !have_static) {
		uint32_t img = build::add_image(p, "RGBStatic", b.colours.width, b.colours.height, b.colours.rgba);
		build::add_child(p, model, img);
		++res.images;
	}
	for (size_t i = first_new; i < objs.size(); ++i) res.made.push_back(uint32_t(i));
	(void)in_block;
	res.root = desc;
	res.notes.insert(res.notes.end(), b.notes.begin(), b.notes.end());
	res.notes.push_back(std::to_string(b.supplied) + " sequence(s) from the file, " + std::to_string(b.retargeted) + " retargeted from " + rig.robot);
	p.touch();
	return res;
}

// ---------------------------------------------------------------------------------------------
// The mapping kept with the source

json::Value robot_mapping_json(const ImportPlan &plan) {
	const HarPlan &h = plan.har;
	const auto &s = plan.scene;
	json::Value v = json::Value::object();
	v["base_robot"] = h.base_robot;
	v["preset"] = h.preset;
	v["units_per_metre"] = double(plan.settings.units_per_metre);
	v["z_up"] = plan.settings.z_up;
	v["auto_orient"] = h.auto_orient;
	v["mirror"] = h.mirror;
	v["turn"] = h.turn;
	v["ground"] = h.ground;
	v["paint"] = h.paint == HarPlan::Paint::Colours ? "colours" : "materials";
	json::Value bones = json::Value::object();
	for (auto &b : h.bones) {
		if (b.joint < 0 || b.source != MapSource::Manual) continue;
		bones[b.bone] = s.nodes[size_t(b.joint)].name;
	}
	v["bones"] = std::move(bones);
	json::Value seqs = json::Value::object();
	for (auto &m : h.sequences) {
		if (m.source != MapSource::Manual) continue;
		seqs[m.sequence] = m.animation >= 0 ? json::Value(s.animations[size_t(m.animation)].name) : json::Value(nullptr);
	}
	v["sequences"] = std::move(seqs);
	return v;
}

void apply_robot_mapping(ImportPlan &plan, const json::Value &v) {
	if (!v.is_object()) return;
	HarPlan &h = plan.har;
	const auto &s = plan.scene;
	auto str = [&](const char *k, std::string *out) {
		if (const json::Value *x = v.find(k); x && x->is_string()) *out = x->as_string();
	};
	auto flag = [&](const char *k, bool *out) {
		if (const json::Value *x = v.find(k); x && x->is_bool()) *out = x->as_bool();
	};
	str("preset", &h.preset);
	flag("auto_orient", &h.auto_orient);
	flag("mirror", &h.mirror);
	flag("ground", &h.ground);
	flag("z_up", &plan.settings.z_up);
	if (const json::Value *x = v.find("turn"); x && x->is_number()) h.turn = int(x->as_int());
	if (const json::Value *x = v.find("units_per_metre"); x && x->is_number() && x->as_real() > 0 && !plan.from_editor) plan.settings.units_per_metre = float(x->as_real());
	if (const json::Value *x = v.find("paint"); x && x->is_string()) h.paint = x->as_string() == "materials" ? HarPlan::Paint::Materials : HarPlan::Paint::Colours;
	if (const json::Value *bones = v.find("bones"); bones && bones->is_object()) {
		for (auto &[bone, joint] : bones->members()) {
			if (!joint.is_string()) continue;
			for (auto &b : h.bones) {
				if (b.bone != bone) continue;
				for (size_t i = 0; i < s.nodes.size(); ++i) {
					if (s.nodes[i].name == joint.as_string()) b.joint = int(i), b.source = MapSource::Manual, b.confidence = 1;
				}
			}
		}
	}
	if (const json::Value *seqs = v.find("sequences"); seqs && seqs->is_object()) {
		for (auto &[seq, anim] : seqs->members()) {
			for (auto &m : h.sequences) {
				if (m.sequence != seq) continue;
				m.source = MapSource::Manual;
				m.animation = -1;
				for (size_t a = 0; anim.is_string() && a < s.animations.size(); ++a) {
					if (s.animations[a].name == anim.as_string()) m.animation = int(a);
				}
			}
		}
	}
}

std::filesystem::path robot_mapping_path(const Project &p, const std::string &name) {
	std::string safe = name;
	for (auto &c : safe) {
		if (strchr("\\/:*?\"<>|", c)) c = '_';
	}
	return sidecar_dir(p) / "models" / safe / "mapping.json";
}

}  // namespace ed
