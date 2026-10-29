// Actor and coordinate-system transforms (Core DE_CActor Move*/Rotate*/LocalOrientation callers
// 0x7008d2a0..0x7008f350, DE_CCoordinateSystem, TCoord3D Rotate*), the degree math helpers and
// SlideTo. Rotation rows are the local X/Y/Z axes in parent space (de::Transform::r).
//
// LocalOrientation() hands the script the actor's own orientation (DE_CActor +0x4c): here a
// block per actor that is refreshed on every LocalOrientation() call and copied back into the
// actor whenever a coordinate-system native changes it.
#include <algorithm>
#include <cmath>
#include <cstring>

#include <map>

#include "de/anim.h"
#include "game/runtime.h"

namespace game {

namespace {

constexpr float kDeg = 0.017453292f;

using Vec = std::array<float, 3>;
using Rows = std::array<float, 9>;

// TCoord3D::RotateX/Y/Z: rotate a vector about the parent axis.
Vec rot(const Vec &v, int axis, float a) {
	float c = std::cos(a), s = std::sin(a);
	switch (axis) {
	case 0: return {v[0], v[1] * c - v[2] * s, v[1] * s + v[2] * c};
	case 1: return {v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c};
	default: return {v[0] * c - v[1] * s, v[0] * s + v[1] * c, v[2]};
	}
}
}  // namespace
void rotate_rows(Rows &r, int axis, float a);
namespace {
Vec sub3(const Vec &a, const Vec &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
// DE_CCoordinateSystem::Set(direction) (0x70025fe0): Z the normalised direction, Y the world up
// made perpendicular to it (world Z when parallel), X = Y x Z.
Rows rows_from_direction(Vec z) {
	float l = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
	if (l > 0) for (auto &k : z) k /= l;
	float d = z[1];
	Vec y{-z[0] * d, 1 - z[1] * d, -z[2] * d};
	if (d >= 1 || d <= -1) y = {0, 0, d};
	float ly = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
	if (ly > 0) for (auto &k : y) k /= ly;
	Vec x{y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0]};
	return {x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]};
}
// DE_CCoordinateSystem::Decompose (0x70027230): the X, Y, Z angles (radians, within +-pi).
Vec decompose(const Rows &r) {
	float a, b, c;
	if (r[0] == 0 && r[1] == 0) {
		a = std::atan2(-r[3], -r[6]), b = 1.5707963f, c = 0;
	} else {
		a = std::atan2(r[5], r[8]), b = std::asin(std::clamp(-r[2], -1.0f, 1.0f)), c = std::atan2(r[1], r[0]);
	}
	Vec out{a, b, c};
	for (auto &k : out) {
		if (k < -3.14159265f) k += 6.2831853f;
		if (k > 3.14159265f) k -= 6.2831853f;
	}
	return out;
}
// RotateLocal* (DE_CCoordinateSystem::RotateLocalX 0x700263e0 and siblings): the rotation about
// the actor's own axis, row_i' = sum_k R[i][k] row_k with R = [1 0 0; 0 c -s; 0 s c] for X (the
// transpose of the parent-axis rotation's rows: positive X pitches the nose up).
void rotate_rows_local(Rows &r, int axis, float a) {
	Rows R{1, 0, 0, 0, 1, 0, 0, 0, 1};
	rotate_rows(R, axis, -a);
	Rows out{};
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			float s = 0;
			for (int k = 0; k < 3; ++k) s += R[size_t(i * 3 + k)] * r[size_t(k * 3 + j)];
			out[size_t(i * 3 + j)] = s;
		}
	}
	r = out;
}

Rows read_rows(vm::Memory &m, uint32_t a) {
	Rows r{};
	for (int i = 0; i < 9; ++i) r[size_t(i)] = m.get<float>(a + uint32_t(i) * 4);
	return r;
}
void write_rows(vm::Memory &m, uint32_t a, const Rows &r) {
	for (int i = 0; i < 9; ++i) m.set<float>(a + uint32_t(i) * 4, r[size_t(i)]);
}

}  // namespace

// DE_CCoordinateSystem::RotateX/Y/Z: every axis rotated about the parent axis.
void rotate_rows(Rows &r, int axis, float a) {
	for (int i = 0; i < 3; ++i) {
		Vec v = rot({r[size_t(i * 3)], r[size_t(i * 3 + 1)], r[size_t(i * 3 + 2)]}, axis, a);
		for (int k = 0; k < 3; ++k) r[size_t(i * 3 + k)] = v[size_t(k)];
	}
}

void anim_rotate(GameObject &o, int axis, float a) {
	rotate_rows(o.anim_rot, axis, a);
}

// Rotation matrices (rows) to quaternions and back, for DE_CQuaternion::SphericalLinearInterpolate.
static std::array<float, 4> to_quat(const std::array<float, 9> &m) {
	float tr = m[0] + m[4] + m[8];
	std::array<float, 4> q;  // x y z w
	if (tr > 0) {
		float s = std::sqrt(tr + 1) * 2;
		q = {(m[5] - m[7]) / s, (m[6] - m[2]) / s, (m[1] - m[3]) / s, 0.25f * s};
	} else if (m[0] > m[4] && m[0] > m[8]) {
		float s = std::sqrt(1 + m[0] - m[4] - m[8]) * 2;
		q = {0.25f * s, (m[1] + m[3]) / s, (m[6] + m[2]) / s, (m[5] - m[7]) / s};
	} else if (m[4] > m[8]) {
		float s = std::sqrt(1 + m[4] - m[0] - m[8]) * 2;
		q = {(m[1] + m[3]) / s, 0.25f * s, (m[5] + m[7]) / s, (m[6] - m[2]) / s};
	} else {
		float s = std::sqrt(1 + m[8] - m[0] - m[4]) * 2;
		q = {(m[6] + m[2]) / s, (m[5] + m[7]) / s, 0.25f * s, (m[1] - m[3]) / s};
	}
	return q;
}

static std::array<float, 9> from_quat(std::array<float, 4> q) {
	float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	for (auto &v : q) v /= l > 0 ? l : 1;
	float x = q[0], y = q[1], z = q[2], w = q[3];
	return {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
	        2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
	        2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
}

std::array<float, 9> slerp_rotation(const std::array<float, 9> &a, const std::array<float, 9> &b, float t) {
	auto qa = to_quat(a), qb = to_quat(b);
	float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
	if (d < 0) {
		for (auto &v : qb) v = -v;
		d = -d;
	}
	std::array<float, 4> q;
	if (d > 0.9995f) {
		for (int k = 0; k < 4; ++k) q[size_t(k)] = qa[size_t(k)] + (qb[size_t(k)] - qa[size_t(k)]) * t;
	} else {
		float th = std::acos(d), s = std::sin(th);
		float ka = std::sin((1 - t) * th) / s, kb = std::sin(t * th) / s;
		for (int k = 0; k < 4; ++k) q[size_t(k)] = qa[size_t(k)] * ka + qb[size_t(k)] * kb;
	}
	return from_quat(q);
}

// The path of an AnimationStreamPlayer object, read on first use.
static const de::AnimPath *path_of(const GameObject *o) {
	static std::map<std::pair<const de::ObjectsFile *, uint32_t>, de::AnimPath> cache;
	if (!o || !o->file || o->class_path != "AnimationStreamPlayer") return nullptr;
	auto key = std::make_pair(o->file, o->index);
	auto it = cache.find(key);
	if (it == cache.end()) {
		de::AnimPath p;
		try {
			p = de::read_anim_path(*o->file, o->file->objects()[o->index]);
		} catch (const std::exception &) {
		}
		it = cache.emplace(key, std::move(p)).first;
	}
	return &it->second;
}

// DE_CCoordinateSystem::Set(vector) made local to the parent: the actor's local orientation
// becomes a frame whose Z axis is the (world) direction `z`.
static void point_z(GameObject *actor, Vec z) {
	float l = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
	if (l <= 0) return;
	z = {z[0] / l, z[1] / l, z[2] / l};
	Vec up = std::fabs(z[1]) > 0.99f ? Vec{1, 0, 0} : Vec{0, 1, 0};
	Vec x{up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
	float xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
	x = {x[0] / xl, x[1] / xl, x[2] / xl};
	Vec y{z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
	if (actor->parent) {
		de::Transform pw = actor->parent->world();
		x = pw.inverse_rotate(x), y = pw.inverse_rotate(y), z = pw.inverse_rotate(z);
	}
	actor->local.r = {x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]};
}

// DE_CAnimationStreamPlayer::SetTime / SetTimeAndPointActor (Core 0x701c1e30 / 0x701c20a0): the
// actor's local orientation from the orientation keys (slerp) and its local position from the
// position keys (linear). PointAlongPath then turns its Z axis towards the next position key
// while t is before it, with DE_CActor::PointZ: from the actor's world position, to the key's
// (path-space) position taken as a world position, made local to the parent.
static void set_path_time(World &w, const GameObject *player, GameObject *actor, float t, bool point) {
	(void)w;
	const de::AnimPath *p = path_of(player);
	if (!p || !actor) return;
	de::Quat q;
	if (p->rotation(t, q)) actor->local.r = de::transform_from({0, 0, 0}, q).r;
	std::array<float, 3> pos;
	size_t next = 0;
	actor->path_moved = true;
	if (!p->position(t, pos, &next)) return;
	actor->local.t = pos;
	if (!point || !(t < p->pos[next].t)) return;
	auto from = actor->world().t;
	point_z(actor, {p->pos[next].p[0] - from[0], p->pos[next].p[1] - from[1], p->pos[next].p[2] - from[2]});
}

void register_transform_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };
	// DE_CActor::PointZByDirection (Core 0x700910c0): the local orientation becomes the frame whose
	// Z axis is the world direction.
	reg("CActor::PointZByDirection(class CVector)", [](const NativeContext &c) {
		if (GameObject *o = c.self(); o && c.arg<uint32_t>(0)) point_z(o, read_vec(c.mem(), c.arg<uint32_t>(0)));
	});
	// PointZCaller (Core 0x7008eeb0) -> DE_CActor::PointZ(point) (0x700911d0): Z towards the
	// target's focal point (vtable +0x154).
	reg("CActor::PointZ(class CActor)", [](const NativeContext &c) {
		GameObject *o = c.self(), *t = c.arg_object(0);
		if (!o || !t) return;
		auto f = focal_point(*t), p = o->world().t;
		point_z(o, {f[0] - p[0], f[1] - p[1], f[2] - p[2]});
	});

	// --- AnimationStreamPlayer paths (moving arena props, the cars, the environment camera) -----
	reg("CAnimationStreamPlayer::SetTime(class CActor,float)", [](const NativeContext &c) {
		set_path_time(c.world, c.self(), c.arg_object(0), c.arg<float>(1), false);
	});
	reg("CAnimationStreamPlayer::SetTimeAndPointAlongPath(class CActor,float)", [](const NativeContext &c) {
		set_path_time(c.world, c.self(), c.arg_object(0), c.arg<float>(1), true);
	});
	reg("CAnimationStreamPlayer::float GetTimeForDistance(float,float)", [](const NativeContext &c) {
		const de::AnimPath *p = path_of(c.self());
		c.ret_f32(p ? p->time_for_distance(c.arg<float>(0), c.arg<float>(1)) : 0);
	});
	reg("CObject::bool IsAnimationStreamPlayer(class CObject)", [](const NativeContext &c) {
		GameObject *o = c.arg_object(0);
		c.ret_bool(o && o->class_path == "AnimationStreamPlayer");
	});

	// --- The actor's own orientation ------------------------------------------------------------
	reg("CActor::class CCoordinateSystem LocalOrientation()", [](const NativeContext &c) {
		GameObject *o = c.self();
		if (!o) {
			c.ret_u32(0);
			return;
		}
		if (!o->local_orientation_block) {
			o->local_orientation_block = c.mem().alloc(36);
			c.world.orientation_owners[o->local_orientation_block] = o;
		}
		write_rows(c.mem(), o->local_orientation_block, o->local.r);
		c.ret_u32(o->local_orientation_block);
	});
	// A coordinate-system native changed `block`: if it is an actor's own orientation, apply it.
	auto changed = [](const NativeContext &c, uint32_t block) {
		auto it = c.world.orientation_owners.find(block);
		if (it != c.world.orientation_owners.end() && c.world.object(it->second->handle) == it->second) {
			it->second->local.r = read_rows(c.mem(), block);
		}
	};
	const std::string cs = "CObject::CCoordinateSystem::";
	reg(cs + "Reset()", [changed](const NativeContext &c) {
		if (uint32_t a = c.self_handle()) {
			write_rows(c.mem(), a, {1, 0, 0, 0, 1, 0, 0, 0, 1});
			changed(c, a);
		}
	});
	reg(cs + "(opp =)(class CCoordinateSystem)", [changed](const NativeContext &c) {
		uint32_t a = c.self_handle(), b = c.arg<uint32_t>(0);
		if (a && b) {
			write_rows(c.mem(), a, read_rows(c.mem(), b));
			changed(c, a);
		}
	});
	// CoordinateSystemPointZCaller (0x7014b450): Set(normalised direction): Z along it, Y the
	// world up made perpendicular (world Z when parallel), X = Y x Z.
	reg(cs + "PointZ(class CVector)", [changed](const NativeContext &c) {
		uint32_t a = c.self_handle(), v = c.arg<uint32_t>(0);
		if (!a || !v) return;
		Vec z = read_vec(c.mem(), v);
		if (z[0] == 0 && z[1] == 0 && z[2] == 0) return;
		write_rows(c.mem(), a, rows_from_direction(z));
		changed(c, a);
	});
	// CoordinateSystemSetCaller (0x7014b0e0): a copy of the other system, identity for none.
	reg(cs + "Set(class CCoordinateSystem)", [changed](const NativeContext &c) {
		uint32_t a = c.self_handle(), b = c.arg<uint32_t>(0);
		if (!a) return;
		write_rows(c.mem(), a, b ? read_rows(c.mem(), b) : Rows{1, 0, 0, 0, 1, 0, 0, 0, 1});
		changed(c, a);
	});
	// DE_CCoordinateSystem::Rotate(other): every axis turned by the other system (this in its frame).
	reg(cs + "Rotate(class CCoordinateSystem)", [changed](const NativeContext &c) {
		uint32_t a = c.self_handle(), b = c.arg<uint32_t>(0);
		if (!a || !b) return;
		de::Transform o;
		o.r = read_rows(c.mem(), b);
		Rows r = read_rows(c.mem(), a);
		for (int k = 0; k < 3; ++k) {
			auto v = o.rotate({r[size_t(k * 3)], r[size_t(k * 3 + 1)], r[size_t(k * 3 + 2)]});
			for (int j = 0; j < 3; ++j) r[size_t(k * 3 + j)] = v[size_t(j)];
		}
		write_rows(c.mem(), a, r);
		changed(c, a);
	});
	// DE_TCoord3D::Rotate(cs): from the system's frame into its parent's.
	reg("CObject::CVector::Rotate(class CCoordinateSystem)", [](const NativeContext &c) {
		uint32_t v = c.self_handle(), b = c.arg<uint32_t>(0);
		if (!v || !b) return;
		de::Transform o;
		o.r = read_rows(c.mem(), b);
		write_vec(c.mem(), v, o.rotate(read_vec(c.mem(), v)));
	});
	// MoveVectorCaller (0x7008d1c0): by a vector in the parent's frame.
	reg("CActor::Move(class CVector)", [](const NativeContext &c) {
		GameObject *o = c.self();
		if (!o) return;
		auto d = read_vec(c.mem(), c.arg<uint32_t>(0));
		for (int k = 0; k < 3; ++k) o->local.t[size_t(k)] += d[size_t(k)];
	});
	// DE_CActor::PointZByRotationDeg(target, x, y, z) (0x70091380): the target's direction in the
	// actor's world frame as X/Y/Z angles (Set + Decompose), each clamped to +-its limit (0: none),
	// turning the local orientation about the parent's X, then Y, then Z. Despite the name the
	// limits are compared with the radian angles, as the original does.
	reg("CActor::PointZByRotationDeg(class CVector,float,float,float)", [](const NativeContext &c) {
		GameObject *o = c.self();
		uint32_t t = c.arg<uint32_t>(0);
		if (!o || !t) return;
		de::Transform w = o->world();
		Vec d = w.inverse_rotate(sub3(read_vec(c.mem(), t), w.t));
		if (d[0] == 0 && d[1] == 0 && d[2] == 0) return;
		Vec ang = decompose(rows_from_direction(d));
		for (int k = 0; k < 3; ++k) {
			float lim = c.arg<float>(size_t(k) + 1);
			float &x = ang[size_t(k)];
			if (lim == 0) x = 0;
			else if (std::fabs(x) > lim) x = x >= 0 ? lim : -lim;
		}
		for (int k = 0; k < 3; ++k) rotate_rows(o->local.r, k, ang[size_t(k)]);
	});
	// MoveCaller (0x7008d220): by (x, y, z) in the parent's frame.
	reg("CActor::Move(float,float,float)", [](const NativeContext &c) {
		if (GameObject *o = c.self()) {
			for (int k = 0; k < 3; ++k) o->local.t[size_t(k)] += c.arg<float>(size_t(k));
		}
	});
	const char *axes[] = {"X", "Y", "Z"};
	for (int axis = 0; axis < 3; ++axis) {
		std::string ax = axes[axis];
		reg(cs + "Rotate" + ax + "Deg(float)", [axis, changed](const NativeContext &c) {
			if (uint32_t a = c.self_handle()) {
				Rows r = read_rows(c.mem(), a);
				rotate_rows(r, axis, c.arg<float>(0) * kDeg);
				write_rows(c.mem(), a, r);
				changed(c, a);
			}
		});
		reg(cs + "RotateLocal" + ax + "Deg(float)", [axis, changed](const NativeContext &c) {
			if (uint32_t a = c.self_handle()) {
				Rows r = read_rows(c.mem(), a);
				rotate_rows_local(r, axis, c.arg<float>(0) * kDeg);
				write_rows(c.mem(), a, r);
				changed(c, a);
			}
		});
		// The actor's own rotations.
		reg("CActor::Rotate" + ax + "Deg(float)", [axis](const NativeContext &c) {
			if (GameObject *o = c.self()) rotate_rows(o->local.r, axis, c.arg<float>(0) * kDeg);
		});
		reg("CActor::RotateLocal" + ax + "Deg(float)", [axis](const NativeContext &c) {
			if (GameObject *o = c.self()) rotate_rows_local(o->local.r, axis, c.arg<float>(0) * kDeg);
		});
		// RotateLocalXCaller (0x7008dbd0) and siblings: the same in radians.
		reg("CActor::RotateLocal" + ax + "(float)", [axis](const NativeContext &c) {
			if (GameObject *o = c.self()) rotate_rows_local(o->local.r, axis, c.arg<float>(0));
		});
		// AnimRotate{X,Y,Z}DegCaller (OMF 0x350208d0..): turn this frame's body rotation (+0x458)
		// about the fighter's axis; the radian forms (0x35020960..) take the angle as is.
		reg("COMF2Fighter::AnimRotate" + ax + "Deg(float)", [axis](const NativeContext &c) {
			if (GameObject *o = c.self()) anim_rotate(*o, axis, c.arg<float>(0) * kDeg);
		});
		reg("COMF2Fighter::AnimRotate" + ax + "(float)", [axis](const NativeContext &c) {
			if (GameObject *o = c.self()) anim_rotate(*o, axis, c.arg<float>(0));
		});
		reg("CActor::MoveLocal" + ax + "(float)", [axis](const NativeContext &c) {
			if (GameObject *o = c.self()) {
				Vec v{0, 0, 0};
				v[size_t(axis)] = c.arg<float>(0);
				Vec d = o->local.rotate(v);
				for (int k = 0; k < 3; ++k) o->local.t[size_t(k)] += d[size_t(k)];
			}
		});
		// TCoord3D::Rotate*: a vector about the parent axis.
		reg("CObject::CVector::Rotate" + ax + "Deg(float)", [axis](const NativeContext &c) {
			if (uint32_t a = c.self_handle()) write_vec(c.mem(), a, rot(read_vec(c.mem(), a), axis, c.arg<float>(0) * kDeg));
		});
	}
	// RotationXDeg etc.: the angle of a rotation about that axis alone.
	reg(cs + "float RotationXDeg()", [](const NativeContext &c) {
		uint32_t a = c.self_handle();
		Rows r = a ? read_rows(c.mem(), a) : Rows{1, 0, 0, 0, 1, 0, 0, 0, 1};
		c.ret_f32(std::atan2(-r[7], r[8]) / kDeg);
	});
	reg(cs + "float RotationYDeg()", [](const NativeContext &c) {
		uint32_t a = c.self_handle();
		Rows r = a ? read_rows(c.mem(), a) : Rows{1, 0, 0, 0, 1, 0, 0, 0, 1};
		c.ret_f32(std::atan2(-r[2], r[0]) / kDeg);
	});
	reg(cs + "float RotationZDeg()", [](const NativeContext &c) {
		uint32_t a = c.self_handle();
		Rows r = a ? read_rows(c.mem(), a) : Rows{1, 0, 0, 0, 1, 0, 0, 0, 1};
		c.ret_f32(std::atan2(r[1], r[0]) / kDeg);
	});
	reg("CActor::MoveLocal(float,float,float)", [](const NativeContext &c) {
		if (GameObject *o = c.self()) {
			Vec d = o->local.rotate({c.arg<float>(0), c.arg<float>(1), c.arg<float>(2)});
			for (int k = 0; k < 3; ++k) o->local.t[size_t(k)] += d[size_t(k)];
		}
	});
	reg("CActor::MoveLocal(class CVector)", [](const NativeContext &c) {
		if (GameObject *o = c.self()) {
			Vec d = o->local.rotate(read_vec(c.mem(), c.arg<uint32_t>(0)));
			for (int k = 0; k < 3; ++k) o->local.t[size_t(k)] += d[size_t(k)];
		}
	});
	// DE_CActor::SlideTo (0x70091840): a step 1/f of the way to the point (all of it for f <= 1.5).
	reg("CActor::SlideTo(class CVector,float)", [](const NativeContext &c) {
		GameObject *o = c.self();
		if (!o) return;
		Vec p = read_vec(c.mem(), c.arg<uint32_t>(0));
		float f = c.arg<float>(1);
		static const bool debug = getenv("OPENBG_DEBUG_SLIDE") != nullptr;
		if (debug) fprintf(stderr, "[slide %u] %s from %.1f %.1f %.1f to %.1f %.1f %.1f in %.2f\n", c.world.frame(), o->name.c_str(), o->local.t[0], o->local.t[1], o->local.t[2], p[0], p[1], p[2], f);
		for (int k = 0; k < 3; ++k) {
			o->local.t[size_t(k)] = f <= 1.5f ? p[size_t(k)] : o->local.t[size_t(k)] - (o->local.t[size_t(k)] - p[size_t(k)]) / f;
		}
	});
	// DE_CActor::RotateTo (0x70091b00): slerp 1/f of the way to the orientation (all of it for
	// f < 1.5).
	reg("CActor::RotateTo(class CCoordinateSystem,float)", [](const NativeContext &c) {
		GameObject *o = c.self();
		uint32_t cs = c.arg<uint32_t>(0);
		if (!o || !cs) return;
		std::array<float, 9> to;
		for (int k = 0; k < 9; ++k) to[size_t(k)] = c.mem().get<float>(cs + uint32_t(k) * 4);
		float f = c.arg<float>(1);
		o->local.r = f < 1.5f ? to : slerp_rotation(o->local.r, to, 1.0f / f);
	});
	// CObject SlideTo (0x700460b0): from `cur` towards `target` by fraction t (clamped 0..1).
	reg("CObject::float SlideTo(float,float,float)", [](const NativeContext &c) {
		float cur = c.arg<float>(0), target = c.arg<float>(1), t = c.arg<float>(2);
		c.ret_f32(t >= 1 ? target : t <= 0 ? cur : (target - cur) * t + cur);
	});
	reg("CObject::float SinDeg(float)", [](const NativeContext &c) { c.ret_f32(std::sin(c.arg<float>(0) * kDeg)); });
	reg("CObject::float CosDeg(float)", [](const NativeContext &c) { c.ret_f32(std::cos(c.arg<float>(0) * kDeg)); });
	reg("CObject::float ATan2Deg(float,float)", [](const NativeContext &c) { c.ret_f32(std::atan2(c.arg<float>(0), c.arg<float>(1)) / kDeg); });
	// Scene time in ms with the frame's fraction (60 frames per second).
	// The scene's renderer (one, the app's view): ConvertFromWorldToScreen turns a world point into
	// screen fractions (y up) with the depth in z (DE_CRenderer caller 0x701833b0); without a view
	// it gives zeros, which the HUD treats as "behind the camera".
	reg("CScene::class CRenderer CurrentRenderer()", [](const NativeContext &c) { c.ret_object(c.world.create("Renderer", "Renderer")); });
	reg("CScene::int NumAttachedRenderer()", [](const NativeContext &c) { c.ret_u32(c.world.world_to_screen ? 1 : 0); });
	reg("CScene::class CRenderer AttachedRenderer(int)", [](const NativeContext &c) {
		c.ret_object(c.arg<int32_t>(0) == 0 ? c.world.create("Renderer", "Renderer") : nullptr);
	});
	// CameraActorCaller (Core 0x70183530): the actor the renderer views from (CRenderer::SetCamera);
	// before any, the CameraActor under the focused fighter (BFighterCameraMover moves it).
	reg("CRenderer::class CActor CameraActor()", [](const NativeContext &c) {
		if (GameObject *cam = c.world.renderer_camera_actor()) {
			c.ret_object(cam);
			return;
		}
		std::function<GameObject *(GameObject *)> find = [&](GameObject *o) -> GameObject * {
			if (o->name == "CameraActor") return o;
			for (GameObject *ch : o->children)
				if (GameObject *f = find(ch)) return f;
			return nullptr;
		};
		c.ret_object(c.world.focus ? find(const_cast<GameObject *>(c.world.focus)) : nullptr);
	});
	reg("CRenderer::ConvertFromWorldToScreen(class CVector)", [](const NativeContext &c) {
		uint32_t v = c.arg<uint32_t>(0);
		if (!v) return;
		write_vec(c.mem(), v, c.world.world_to_screen ? c.world.world_to_screen(read_vec(c.mem(), v)) : std::array<float, 3>{0, 0, 0});
	});
	reg("CScene::dword PreciseTime()", [](const NativeContext &c) { c.ret_u32(c.world.scene_ms_of(c.owner())); });
	reg("CActor::class CVector FocalPoint()", [](const NativeContext &c) {
		GameObject *o = c.self();
		if (!o) {
			c.ret_u32(0);
			return;
		}
		uint32_t v = c.mem().alloc(12);
		write_vec(c.mem(), v, focal_point(*o));
		c.ret_u32(v);
	});
}

}  // namespace game
