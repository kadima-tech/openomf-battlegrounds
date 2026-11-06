#include "fighter_model.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "de/texture.h"

namespace fighter {

namespace {

const de::ObjectInfo *find_child(const de::ObjectsFile &f, size_t parent, const std::string &cls,
                                 const std::string &name = "") {
	for (uint32_t c : de::ObjectsFile::children(f.object_data(f.objects()[parent]))) {
		auto &o = f.objects()[c];
		if (o.class_path == cls && (name.empty() || o.name == name)) {
			return &o;
		}
	}
	return nullptr;
}

}  // namespace

std::vector<uint8_t> read_file(const std::filesystem::path &path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		throw std::runtime_error("cannot open " + path.string());
	}
	return {std::istreambuf_iterator<char>(f), {}};
}

Model load_fighter(const de::ObjectsFile &f, const char *spec, bool verbose) {
	const auto &objs = f.objects();
	size_t model = SIZE_MAX;
	for (size_t i = 0; i < objs.size(); ++i) {
		bool hit = spec ? (spec[0] == '#' ? i == strtoul(spec + 1, nullptr, 10) : objs[i].name == spec)
		                : objs[i].class_path == "FighterModel";
		if (hit) {
			model = i;
			break;
		}
	}
	if (model == SIZE_MAX) {
		throw std::runtime_error("no FighterModel found");
	}
	Model out;
	for (uint32_t c : de::ObjectsFile::children(f.object_data(objs[model]))) {
		if (objs[c].class_path != "Mesh\\Standard") {
			continue;
		}
		auto m = de::read_mesh(f, objs[c]);
		if (m.faces.size() > out.mesh.faces.size()) {
			out.mesh = std::move(m);
			out.mesh_index = c;
		}
	}
	if (out.mesh.faces.empty()) {
		throw std::runtime_error("FighterModel has no mesh");
	}
	out.face_uv.assign(out.mesh.faces.size(), {});
	if (auto mat = find_child(f, out.mesh_index, "TextureProcess\\MultiStage")) {
		if (auto tm = find_child(f, size_t(mat - objs.data()), "TextureManager", "RGBMain")) {
			for (auto &fu : de::read_texture_uvs(f, *tm)) {
				if (fu.face < out.face_uv.size()) {
					out.face_uv[fu.face] = fu.uv;
				}
			}
		}
	}
	if (auto img = find_child(f, model, "Image\\Standard", "RGBMain")) {
		out.skin = de::read_image(f, *img);
	}
	float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
	for (auto &v : out.mesh.vertices) {
		for (int j = 0; j < 3; ++j) {
			lo[j] = std::fmin(lo[j], v[j]);
			hi[j] = std::fmax(hi[j], v[j]);
		}
	}
	for (int j = 0; j < 3; ++j) {
		out.center[j] = (lo[j] + hi[j]) / 2;
	}
	out.radius = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
	                       (hi[2] - lo[2]) * (hi[2] - lo[2])) / 2;
	if (verbose) printf("%s: mesh \"%s\" %zu triangles, skin %dx%d\n", objs[model].name.c_str(), out.mesh.name.c_str(),
	       out.mesh.faces.size(), out.skin.width, out.skin.height);
	return out;
}

std::unique_ptr<Rig> load_rig(const de::ObjectsFile &fighter, const de::ObjectsFile &model_file, uint32_t mesh_index, bool verbose) {
	auto rig = std::make_unique<Rig>();
	const de::ObjectInfo *bank = nullptr, *deform = nullptr;
	for (auto &o : fighter.objects()) {
		if (!bank && o.class_path == "AnimationSequenceGroup") {
			bank = &o;
		}
		if (!deform && o.class_path == "ActorDeformInfo") {
			deform = &o;
		}
	}
	if (!bank || !deform) {
		throw std::runtime_error("fighter file has no animation bank or deform info");
	}
	rig->bank = de::read_anim_bank(fighter, *bank);
	rig->bones = de::read_skin(fighter, *deform, model_file, mesh_index);
	for (auto &b : rig->bones) {
		rig->bone_channel.push_back(rig->bank.channel(b.name));
		rig->inv_bind.push_back(b.bind.inverse());
	}
	if (verbose) printf("rig: %zu bones, %zu channels, %zu moves\n", rig->bones.size(), rig->bank.channels.size(),
	       rig->bank.sequences.size());
	return rig;
}

void skin_vertices(const Model &m, const Rig *rig, const de::AnimSequence *seq, float t,
                   std::vector<std::array<float, 3>> &pos) {
	pos = m.mesh.vertices;
	if (!rig || !seq) {
		return;
	}
	auto pose = rig->bank.pose(*seq, t);
	static bool debug = getenv("OPENBG_DEBUG") != nullptr;
	if (debug) {
		for (size_t b = 0; b < rig->bones.size(); ++b) {
			int c = rig->bone_channel[b];
			auto &bt = rig->bones[b].bind.t;
			auto pt = c >= 0 ? pose[size_t(c)].t : std::array<float, 3>{0, 0, 0};
			fprintf(stderr, "bone %-14s ch %3d bind (%6.2f %6.2f %6.2f) pose (%6.2f %6.2f %6.2f) weights %zu\n",
			        rig->bones[b].name.c_str(), c, bt[0], bt[1], bt[2], pt[0], pt[1], pt[2], rig->bones[b].weights.size());
		}
		debug = false;
	}
	std::vector<de::Transform> skin(rig->bones.size());
	for (size_t b = 0; b < rig->bones.size(); ++b) {
		int c = rig->bone_channel[b];
		skin[b] = c >= 0 ? pose[size_t(c)].then(rig->inv_bind[b]) : de::Transform{};
	}
	for (size_t v = 0; v < pos.size(); ++v) {
		float acc[3] = {0, 0, 0}, wsum = 0;
		for (size_t b = 0; b < rig->bones.size(); ++b) {
			auto &w = rig->bones[b].weights;
			if (v >= w.size() || !w[v]) {
				continue;
			}
			auto p = skin[b].apply(m.mesh.vertices[v]);
			float wf = w[v] / 255.0f;
			for (int k = 0; k < 3; ++k) {
				acc[k] += p[k] * wf;
			}
			wsum += wf;
		}
		if (wsum > 0) {
			pos[v] = {acc[0] / wsum, acc[1] / wsum, acc[2] / wsum};
		}
	}
}

std::vector<Vertex> build_vertices(const Model &m, const std::vector<std::array<float, 3>> &pos) {
	std::vector<std::array<float, 3>> vn(pos.size(), {0, 0, 0});
	for (auto &face : m.mesh.faces) {
		auto &a = pos[face.v[0]], &b = pos[face.v[1]], &c = pos[face.v[2]];
		float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
		float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
		for (auto v : face.v) {
			for (int k = 0; k < 3; ++k) {
				vn[v][k] += n[k];
			}
		}
	}
	std::vector<Vertex> out;
	out.reserve(m.mesh.faces.size() * 3);
	for (size_t i = 0; i < m.mesh.faces.size(); ++i) {
		for (int k = 0; k < 3; ++k) {
			uint16_t vi = m.mesh.faces[i].v[k];
			Vertex v{};
			memcpy(v.pos, pos[vi].data(), sizeof v.pos);
			float l = std::sqrt(vn[vi][0] * vn[vi][0] + vn[vi][1] * vn[vi][1] + vn[vi][2] * vn[vi][2]);
			for (int j = 0; j < 3; ++j) {
				v.normal[j] = l > 0 ? vn[vi][j] / l : 0;
			}
			v.uv[0] = m.face_uv[i][k][0];
			v.uv[1] = m.face_uv[i][k][1];
			out.push_back(v);
		}
	}
	return out;
}

const char *const kVertexShader = R"(#version 410 core
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
uniform mat4 mvp;
out vec3 v_normal;
out vec2 v_uv;
void main() {
	gl_Position = mvp * vec4(pos, 1.0);
	v_normal = normal;
	v_uv = uv;
}
)";

const char *const kFragmentShader = R"(#version 410 core
in vec3 v_normal;
in vec2 v_uv;
uniform sampler2D skin;
uniform vec3 light;
uniform mat3 pilot;  // columns: colours for mask channels R, G, B
out vec4 color;
void main() {
	vec3 n = normalize(v_normal);
	float d = abs(dot(n, normalize(light)));  // two-sided: winding not verified yet
	// DE_COMF2FighterImageFilter::Filter (OMF 0x350350e0): out = r*C0 + g*C1 + b*C2, clamped;
	// tables built by DE_CColorConversion::Update (0x35035ff0) as clamp(colour*256*i, 0, 65280).
	vec3 base = min(pilot * texture(skin, v_uv).rgb, vec3(1.0));
	color = vec4(base * (0.35 + 0.75 * d), 1.0);
}
)";

}  // namespace fighter
