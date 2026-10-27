// openbg-viewer: render (and animate) a fighter from the original data natively.
//
//   openbg-viewer <Model.DEObjects> [FighterModel] [--colors r,g,b x3] [--screenshot out.png]
//   openbg-viewer --data <Data dir> --fighter <Name> [--anim <Sequence>] [--time t] [--list]
//                 [--colors ...] [--screenshot out.png]
//
// With --fighter the mesh comes from <Name>Model1.DEObjects and the skeleton, skin and moves
// from <Name>.DEObjects. Drag to orbit, scroll to zoom, Left/Right to change move, Space to
// pause, Esc to quit.
#include <SDL.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "de/anim.h"
#include "de/image.h"
#include "de/mesh.h"
#include "de/objects_file.h"
#include "de/texture.h"
#include "gl_util.h"

namespace {

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
	float center[3] = {};
	float radius = 1;
};

std::vector<uint8_t> read_file(const std::filesystem::path &path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		throw std::runtime_error("cannot open " + path.string());
	}
	return {std::istreambuf_iterator<char>(f), {}};
}

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

// FighterModel -> highest-detail Mesh\Standard child -> its MultiStage material -> "RGBMain"
// TextureManager (UVs); the skin is the FighterModel's "RGBMain" Image\Standard.
Model load_fighter(const de::ObjectsFile &f, const char *spec) {
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
	printf("%s: mesh \"%s\" %zu triangles, skin %dx%d\n", objs[model].name.c_str(), out.mesh.name.c_str(),
	       out.mesh.faces.size(), out.skin.width, out.skin.height);
	return out;
}

// Skeleton, skin and moves for one fighter.
struct Rig {
	de::AnimBank bank;
	std::vector<de::SkinBone> bones;
	std::vector<int> bone_channel;  // channel index per bone
	std::vector<de::Transform> inv_bind;
};

std::unique_ptr<Rig> load_rig(const de::ObjectsFile &fighter, const de::ObjectsFile &model_file, uint32_t mesh_index) {
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
	printf("rig: %zu bones, %zu channels, %zu moves\n", rig->bones.size(), rig->bank.channels.size(),
	       rig->bank.sequences.size());
	return rig;
}

// Positions per mesh vertex for the current pose (linear blend skinning, inferred:
// v' = sum_b w_b * Pose_b * Bind_b^-1 * v, weights normalised per vertex).
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

const char *kVertexShader = R"(#version 410 core
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

const char *kFragmentShader = R"(#version 410 core
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

}  // namespace

int main(int argc, char **argv) {
	const char *model_path = nullptr, *spec = nullptr, *shot = nullptr, *data = nullptr, *fighter = nullptr;
	std::string anim = "Standing";
	float fixed_time = -1;
	bool list = false;
	// Pilot colours for the three mask channels (0..1).
	float colors[9] = {0.75f, 0.78f, 0.82f, 0.10f, 0.35f, 0.85f, 1.00f, 0.80f, 0.15f};
	for (int i = 1; i < argc; ++i) {
		auto arg = [&](const char *name) { return !strcmp(argv[i], name) && i + 1 < argc; };
		if (arg("--colors")) {
			sscanf(argv[++i], "%f,%f,%f,%f,%f,%f,%f,%f,%f", &colors[0], &colors[1], &colors[2], &colors[3], &colors[4],
			       &colors[5], &colors[6], &colors[7], &colors[8]);
		} else if (arg("--screenshot")) {
			shot = argv[++i];
		} else if (arg("--data")) {
			data = argv[++i];
		} else if (arg("--fighter")) {
			fighter = argv[++i];
		} else if (arg("--anim")) {
			anim = argv[++i];
		} else if (arg("--time")) {
			fixed_time = float(atof(argv[++i]));
		} else if (!strcmp(argv[i], "--list")) {
			list = true;
		} else if (!model_path) {
			model_path = argv[i];
		} else {
			spec = argv[i];
		}
	}
	if (!model_path && !(data && fighter)) {
		fprintf(stderr,
		        "usage: %s <Model.DEObjects> [FighterModel] [--colors ...] [--screenshot out.png]\n"
		        "       %s --data <Data dir> --fighter <Name> [--anim <Sequence>] [--time t] [--list]\n",
		        argv[0], argv[0]);
		return 2;
	}
	try {
		std::filesystem::path model_file_path = model_path ? std::filesystem::path(model_path)
		                                                   : std::filesystem::path(data) / (std::string(fighter) + "Model1.DEObjects");
		de::ObjectsFile model_file(read_file(model_file_path));
		Model model = load_fighter(model_file, spec);

		std::unique_ptr<de::ObjectsFile> fighter_file;
		std::unique_ptr<Rig> rig;
		if (data && fighter) {
			fighter_file = std::make_unique<de::ObjectsFile>(read_file(std::filesystem::path(data) / (std::string(fighter) + ".DEObjects")));
			rig = load_rig(*fighter_file, model_file, model.mesh_index);
			if (list) {
				for (auto &s : rig->bank.sequences) {
					printf("%s\n", s.name.c_str());
				}
				return 0;
			}
		}
		size_t seq_index = 0;
		if (rig) {
			for (size_t i = 0; i < rig->bank.sequences.size(); ++i) {
				if (rig->bank.sequences[i].name == anim) {
					seq_index = i;
				}
			}
		}

		SDL_Init(SDL_INIT_VIDEO);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
		const int W = 1024, H = 768;
		SDL_Window *win = SDL_CreateWindow("One Must Fall: Battlegrounds — model viewer", SDL_WINDOWPOS_CENTERED,
		                                   SDL_WINDOWPOS_CENTERED, W, H,
		                                   SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | (shot ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
		if (!win) {
			throw std::runtime_error(SDL_GetError());
		}
		SDL_GLContext ctx = SDL_GL_CreateContext(win);
		SDL_GL_SetSwapInterval(1);
		GLuint prog = glu::program(kVertexShader, kFragmentShader);

		GLuint vao, vbo;
		glGenVertexArrays(1, &vao);
		glBindVertexArray(vao);
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, pos));
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, normal));
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, uv));
		for (GLuint i = 0; i < 3; ++i) {
			glEnableVertexAttribArray(i);
		}
		std::vector<uint8_t> grey{200, 200, 200, 255};
		bool has_skin = !model.skin.channels.empty();
		auto rgba = has_skin ? model.skin.rgba() : grey;
		GLuint tex = glu::texture_rgba(has_skin ? model.skin.width : 1, has_skin ? model.skin.height : 1, rgba.data());
		glBindTexture(GL_TEXTURE_2D, tex);
		if (shot) {
			glu::offscreen(W, H);
		}

		float yaw = 0.45f, pitch = 0.12f, distance = model.radius * 2.6f;
		bool running = true, dragging = false, paused = false;
		float t = fixed_time >= 0 ? fixed_time : 0;
		Uint64 last = SDL_GetPerformanceCounter();
		std::vector<std::array<float, 3>> pos;
		auto title = [&] {
			if (rig) {
				std::string s = std::string(fighter) + " — " + rig->bank.sequences[seq_index].name +
				                "  (Left/Right: move, Space: pause)";
				SDL_SetWindowTitle(win, s.c_str());
			}
		};
		title();
		while (running) {
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
					running = false;
				} else if (e.type == SDL_KEYDOWN && rig) {
					size_t n = rig->bank.sequences.size();
					if (e.key.keysym.sym == SDLK_RIGHT) {
						seq_index = (seq_index + 1) % n;
						t = 0;
						title();
					} else if (e.key.keysym.sym == SDLK_LEFT) {
						seq_index = (seq_index + n - 1) % n;
						t = 0;
						title();
					} else if (e.key.keysym.sym == SDLK_SPACE) {
						paused = !paused;
					}
				} else if (e.type == SDL_MOUSEBUTTONDOWN) {
					dragging = true;
				} else if (e.type == SDL_MOUSEBUTTONUP) {
					dragging = false;
				} else if (e.type == SDL_MOUSEMOTION && dragging) {
					yaw += e.motion.xrel * 0.01f;
					pitch = std::fmax(-1.5f, std::fmin(1.5f, pitch + e.motion.yrel * 0.01f));
				} else if (e.type == SDL_MOUSEWHEEL) {
					distance *= e.wheel.y > 0 ? 0.9f : 1.1f;
				}
			}
			Uint64 now = SDL_GetPerformanceCounter();
			float dt = float(now - last) / float(SDL_GetPerformanceFrequency());
			last = now;
			if (!paused && fixed_time < 0) {
				t += dt * 0.8f;  // sequence time is normalised; ~1.25 s per move (playback rate not decoded yet)
				if (t > 1) {
					t -= 1;
				}
			}
			skin_vertices(model, rig.get(), rig ? &rig->bank.sequences[seq_index] : nullptr, t, pos);
			auto verts = build_vertices(model, pos);
			// Follow the posed model: ease the orbit centre toward the current bounding-box centre.
			{
				float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
				for (auto &p : pos) {
					for (int j = 0; j < 3; ++j) {
						lo[j] = std::fmin(lo[j], p[j]);
						hi[j] = std::fmax(hi[j], p[j]);
					}
				}
				float k = shot ? 1.0f : std::fmin(1.0f, dt * 4.0f);
				for (int j = 0; j < 3; ++j) {
					model.center[j] += ((lo[j] + hi[j]) / 2 - model.center[j]) * k;
				}
			}
			glBindBuffer(GL_ARRAY_BUFFER, vbo);
			glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(Vertex)), verts.data(), GL_DYNAMIC_DRAW);

			int dw = W, dh = H;
			if (!shot) {
				SDL_GL_GetDrawableSize(win, &dw, &dh);
			}
			glViewport(0, 0, dw, dh);
			glEnable(GL_DEPTH_TEST);
			glClearColor(0.11f, 0.12f, 0.15f, 1);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			float proj[16], view[16], mvp[16];
			glu::perspective(proj, 45.0f, float(dw) / float(dh), model.radius * 0.05f, model.radius * 20.0f);
			// Orbit around the model centre.
			float eye[3] = {model.center[0] + distance * std::sin(yaw) * std::cos(pitch), model.center[1] + distance * std::sin(pitch),
			                model.center[2] + distance * std::cos(yaw) * std::cos(pitch)};
			glu::fps_view(view, eye, -yaw, pitch);
			glu::mul(mvp, proj, view);
			glUseProgram(prog);
			glUniformMatrix4fv(glGetUniformLocation(prog, "mvp"), 1, GL_FALSE, mvp);
			glUniform3f(glGetUniformLocation(prog, "light"), 0.4f, 0.7f, 0.6f);
			glUniform1i(glGetUniformLocation(prog, "skin"), 0);
			glUniformMatrix3fv(glGetUniformLocation(prog, "pilot"), 1, GL_FALSE, colors);
			glBindVertexArray(vao);
			glDrawArrays(GL_TRIANGLES, 0, GLsizei(verts.size()));
			if (shot) {
				std::vector<uint8_t> px(size_t(W) * H * 4);
				glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
				glu::write_png(shot, W, H, px);
				printf("wrote %s\n", shot);
				break;
			}
			SDL_GL_SwapWindow(win);
		}
		SDL_GL_DeleteContext(ctx);
		SDL_DestroyWindow(win);
		SDL_Quit();
	} catch (const std::exception &ex) {
		fprintf(stderr, "error: %s\n", ex.what());
		return 1;
	}
	return 0;
}
