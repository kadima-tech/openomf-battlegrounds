#include "de/anim.h"

#include <algorithm>
#include <cmath>

namespace de {

namespace {

std::string text(Contents &c) {
	auto b = c.block();
	size_t n = b.size();
	while (n && b[n - 1] == 0) {
		--n;
	}
	return std::string(b.begin(), b.begin() + n);
}

Quat read_quat(Contents &c) { return {c.f32(), c.f32(), c.f32(), c.f32()}; }

Quat slerp(Quat a, const Quat &b, float t) {
	float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	if (d < 0) {
		for (auto &x : a) {
			x = -x;
		}
		d = -d;
	}
	float wa, wb;
	if (d > 0.9995f) {
		wa = 1 - t;
		wb = t;
	} else {
		float th = std::acos(d), s = std::sin(th);
		wa = std::sin((1 - t) * th) / s;
		wb = std::sin(t * th) / s;
	}
	Quat r;
	float len = 0;
	for (int i = 0; i < 4; ++i) {
		r[i] = a[i] * wa + b[i] * wb;
		len += r[i] * r[i];
	}
	len = std::sqrt(len);
	for (auto &x : r) {
		x /= len;
	}
	return r;
}

template <typename Key, typename F>
void sample(const std::vector<Key> &keys, float t, F &&out) {
	if (keys.empty()) {
		return;
	}
	if (t <= keys.front().t) {
		out(keys.front(), keys.front(), 0.0f);
		return;
	}
	for (size_t i = 1; i < keys.size(); ++i) {
		if (t <= keys[i].t) {
			float span = keys[i].t - keys[i - 1].t;
			out(keys[i - 1], keys[i], span > 0 ? (t - keys[i - 1].t) / span : 0.0f);
			return;
		}
	}
	out(keys.back(), keys.back(), 0.0f);
}

}  // namespace

Quat slerp_quat(const Quat &a, const Quat &b, float t) { return slerp(a, b, t); }

Transform transform_from(const std::array<float, 3> &pos, const Quat &q) {
	float x = q[0], y = q[1], z = q[2], w = q[3];
	// Standard rotation matrix R (maps local to parent); our rows are R's columns.
	float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
	                 {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
	                 {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
	Transform t;
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			t.r[i * 3 + j] = R[j][i];
		}
	}
	t.t = pos;
	return t;
}

const AnimSequence *AnimBank::find(const std::string &n) const {
	for (auto &s : sequences) {
		if (s.name == n) {
			return &s;
		}
	}
	return nullptr;
}

int AnimBank::channel(const std::string &n) const {
	for (size_t i = 0; i < channels.size(); ++i) {
		if (channels[i].name == n) {
			return int(i);
		}
	}
	return -1;
}

bool AnimBank::local(const AnimSequence &seq, size_t ch, float t, Transform &out) const {
	if (ch >= channels.size()) return false;
	for (auto &s : seq.streams) {
		if (s.channel != ch) continue;
		std::array<float, 3> pos = channels[ch].rest_pos;
		Quat rot = channels[ch].rest_rot;
		sample(s.pos, t, [&](auto &a, auto &b, float u) {
			for (int k = 0; k < 3; ++k) pos[k] = a.p[k] + (b.p[k] - a.p[k]) * u;
		});
		sample(s.rot, t, [&](auto &a, auto &b, float u) { rot = slerp(a.q, b.q, u); });
		out = transform_from(pos, rot);
		return true;
	}
	return false;
}

std::vector<Transform> AnimBank::pose(const AnimSequence &seq, float t, bool root_motion) const {
	std::vector<std::array<float, 3>> pos(channels.size());
	std::vector<Quat> rot(channels.size());
	for (size_t i = 0; i < channels.size(); ++i) {
		pos[i] = channels[i].rest_pos;
		rot[i] = channels[i].rest_rot;
	}
	for (auto &s : seq.streams) {
		if (s.channel >= channels.size()) {
			continue;
		}
		sample(s.pos, t, [&](auto &a, auto &b, float u) {
			for (int k = 0; k < 3; ++k) {
				pos[s.channel][k] = a.p[k] + (b.p[k] - a.p[k]) * u;
			}
		});
		sample(s.rot, t, [&](auto &a, auto &b, float u) { rot[s.channel] = slerp(a.q, b.q, u); });
	}
	if (!root_motion) {
		int body = channel("Body");
		if (body >= 0) {
			pos[size_t(body)] = {0, 0, 0};
			rot[size_t(body)] = {0, 0, 0, 1};
		}
	}
	std::vector<Transform> out(channels.size());
	std::vector<bool> done(channels.size(), false);
	for (size_t pass = 0; pass < channels.size(); ++pass) {
		bool progress = false;
		for (size_t i = 0; i < channels.size(); ++i) {
			if (done[i]) {
				continue;
			}
			int p = channels[i].parent;
			if (p >= 0 && size_t(p) < channels.size() && !done[size_t(p)]) {
				continue;
			}
			Transform local = transform_from(pos[i], rot[i]);
			out[i] = p >= 0 && size_t(p) < channels.size() ? out[size_t(p)].then(local) : local;
			done[i] = progress = true;
		}
		if (!progress) {
			break;
		}
	}
	return out;
}

AnimBank read_anim_bank(const ObjectsFile &f, const ObjectInfo &o) {
	Contents c(f.object_data(o).contents);
	Chunk ch = begin_chunk(c);
	if (ch.version != 4) {
		throw FormatError("AnimationSequenceGroup: unsupported version " + std::to_string(ch.version));
	}
	AnimBank b;
	b.name = text(c);
	b.sequences.resize(c.i32());
	for (auto &s : b.sequences) {
		s.name = text(c);
		s.streams.resize(c.i32());
		for (auto &st : s.streams) {
			st.channel = c.u16();
			Chunk sc = begin_chunk(c);
			if (sc.version == 1) {
				st.pos.resize(c.i32());
				for (auto &k : st.pos) {
					k.t = c.f32();
					k.p = {c.f32(), c.f32(), c.f32()};
				}
				st.rot.resize(c.i32());
				for (auto &k : st.rot) {
					k.t = c.f32();
					k.q = read_quat(c);
				}
			}
			c.seek(sc.end);
		}
	}
	b.channels.resize(c.i32());
	for (auto &a : b.channels) {
		a.name = text(c);
		c.boolean();
		c.boolean();
		a.parent = int16_t(c.u16());
		a.rest_pos = {c.f32(), c.f32(), c.f32()};
		a.rest_rot = read_quat(c);
	}
	c.seek(ch.end);
	return b;
}

std::vector<SkinBone> read_skin(const ObjectsFile &df, const ObjectInfo &di, const ObjectsFile &mf, uint32_t mesh_index) {
	auto d = df.object_data(di);
	std::vector<SkinBone> bones;
	{
		Contents c(d.contents);
		Chunk ch = begin_chunk(c);
		if (ch.version != 3) {
			throw FormatError("ActorDeformInfo: unsupported version " + std::to_string(ch.version));
		}
		text(c);
		while (c.pos() < ch.end && c.boolean()) {
			Chunk dc = begin_chunk(c);
			SkinBone b;
			b.bind.t = {c.f32(), c.f32(), c.f32()};
			for (auto &v : b.bind.r) {
				v = c.f32();
			}
			c.seek(dc.end);
			bones.push_back(std::move(b));
		}
	}
	{
		// References: DE_CObject chunk, then the deform-info chunk with one deformer chunk
		// {BOOL, reference} per bone.
		Contents c(d.references);
		Chunk obj = begin_chunk(c);
		c.seek(obj.end);
		Chunk ch = begin_chunk(c);
		for (auto &b : bones) {
			if (c.pos() >= ch.end) {
				break;
			}
			Chunk dc = begin_chunk(c);
			if (c.boolean()) {
				Reference r = Reference::read(c);
				if (r.kind == Reference::kLocal && r.index < df.objects().size()) {
					b.name = df.objects()[r.index].name;
				}
			}
			c.seek(dc.end);
		}
	}
	// Weights: "Deform<Bone>VertexData" children of the mesh.
	for (uint32_t ci : ObjectsFile::children(mf.object_data(mf.objects()[mesh_index]))) {
		auto &o = mf.objects()[ci];
		if (o.class_path != "DataChannel\\UnitFloats") {
			continue;
		}
		for (auto &b : bones) {
			if (o.name != "Deform" + b.name + "VertexData") {
				continue;
			}
			Contents c(mf.object_data(o).contents);
			Chunk base = begin_chunk(c);  // DE_CDataChannel
			c.seek(base.end);
			Chunk uf = begin_chunk(c);
			if (uf.version == 1) {
				int32_t count = c.i32(), size = c.i32();
				auto px = c.bytes(size_t(count) * size_t(size));
				b.weights.assign(px.begin(), px.end());
			}
		}
	}
	return bones;
}

std::vector<Transform> blend_poses(const std::vector<Transform> &a, const std::vector<Transform> &b, float w) {
	std::vector<Transform> out(b);
	for (size_t i = 0; i < out.size() && i < a.size(); ++i) {
		for (int k = 0; k < 3; ++k) {
			out[i].t[size_t(k)] = a[i].t[size_t(k)] + (b[i].t[size_t(k)] - a[i].t[size_t(k)]) * w;
		}
		std::array<float, 9> r;
		for (int k = 0; k < 9; ++k) {
			r[size_t(k)] = a[i].r[size_t(k)] + (b[i].r[size_t(k)] - a[i].r[size_t(k)]) * w;
		}
		auto norm = [](float *v) {
			float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			if (l > 1e-6f) {
				v[0] /= l, v[1] /= l, v[2] /= l;
			}
		};
		float *x = &r[0], *y = &r[3], *z = &r[6];
		norm(x);
		float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
		for (int k = 0; k < 3; ++k) {
			y[k] -= d * x[k];
		}
		norm(y);
		z[0] = x[1] * y[2] - x[2] * y[1], z[1] = x[2] * y[0] - x[0] * y[2], z[2] = x[0] * y[1] - x[1] * y[0];
		out[i].r = r;
	}
	return out;
}


AnimPath read_anim_path(const ObjectsFile &f, const ObjectInfo &o) {
	Contents c(f.object_data(o).contents);
	Chunk ch = begin_chunk(c);
	AnimPath p;
	if (ch.version == 1) {
		c.block();  // name
		Chunk sc = begin_chunk(c);
		if (sc.version == 1) {
			p.pos.resize(size_t(std::max(0, c.i32())));
			for (auto &k : p.pos) {
				k.t = c.f32();
				k.p = {c.f32(), c.f32(), c.f32()};
			}
			p.rot.resize(size_t(std::max(0, c.i32())));
			for (auto &k : p.rot) {
				k.t = c.f32();
				k.q = read_quat(c);
			}
		}
	}
	return p;
}

bool AnimPath::position(float t, std::array<float, 3> &out, size_t *next) const {
	size_t a, b;
	float u;
	if (!around(pos, t, a, b, u)) return false;
	for (int i = 0; i < 3; ++i) out[size_t(i)] = (pos[b].p[size_t(i)] - pos[a].p[size_t(i)]) * u + pos[a].p[size_t(i)];
	if (next) *next = b;
	return true;
}

bool AnimPath::rotation(float t, Quat &q) const {
	size_t a, b;
	float u;
	if (!around(rot, t, a, b, u)) return false;
	q = a == b ? rot[a].q : slerp(rot[a].q, rot[b].q, u);
	return true;
}

// GetTimeForDistance (0x701c2310): for a negative distance, the time that far back along the
// path from t (segment by segment; 0 past the start); 1 for any other distance.
float AnimPath::time_for_distance(float d, float t) const {
	size_t a, b;
	float u;
	if (!around(pos, t, a, b, u) || a == b) return 0;
	auto len = [&](size_t i, size_t j, float f) {
		float x = (pos[j].p[0] - pos[i].p[0]) * f, y = (pos[j].p[1] - pos[i].p[1]) * f, z = (pos[j].p[2] - pos[i].p[2]) * f;
		return std::sqrt(x * x + y * y + z * z);
	};
	float seg = len(a, b, u);
	while (d < 0) {
		float l = -seg;
		if (l < d) return (pos[b].t - pos[a].t) * ((l - d) / l) * u + pos[a].t;
		if (a == 0) return 0;
		d -= l;
		--a, --b;
		u = 1;
		seg = len(a, b, 1);
	}
	return 1;
}

}  // namespace de
