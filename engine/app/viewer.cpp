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
#include <cstdlib>

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
#include "de/platform.h"
#include "de/image.h"
#include "de/mesh.h"
#include "de/objects_file.h"
#include "de/texture.h"
#include "gl_util.h"
#include "gl_window.h"
#include "fighter_model.h"

using namespace fighter;

int main(int argc, char **argv) {
	de::init_platform();
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
		SDL_GLContext ctx = nullptr;
		SDL_Window *win = glu::create_gl_window("One Must Fall: Battlegrounds — model viewer", SDL_WINDOWPOS_CENTERED,
		                                        SDL_WINDOWPOS_CENTERED, W, H,
		                                        SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | (shot ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN), &ctx);
		if (!glu::load_gl()) throw std::runtime_error("OpenGL 4.1 is required; update your graphics driver");
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
