// openbg-arena: fly through an arena built from the original data (SDL2 + OpenGL 4.1).
//
//   openbg-arena <Data dir> <arena file stem, e.g. Desert> [--eye x,y,z,yaw,pitch] [--time ms] [--no-bump] [--screenshot out.png]
//
// WASD/QE to move (Shift = fast), drag to look, Esc to quit.
// Drawn with the fight's renderer: the materials' stage passes, the arena's lights, reflections and
// the glow layer. The arena's SceneTime runs from --time (0) as BSetSceneTime would set it.
#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "de/lighting.h"
#include "de/scene.h"
#include "gl_util.h"
#include "glow.h"
#include "render.h"

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <Data dir> <arena stem> [--screenshot out.png]\n", argv[0]);
		return 2;
	}
	const char *shot = nullptr;
	float eye_arg[5];
	bool have_eye = false;
	float scene_ms = 0;
	for (int i = 3; i < argc; ++i) {
		if (!strcmp(argv[i], "--no-bump")) {
			static const bool off = false;
			render::g_bump_enabled = &off;
		} else if (!strcmp(argv[i], "--texture-detail") && i + 1 < argc) {
			static int detail = 0;
			detail = atoi(argv[++i]);
			render::g_texture_detail = &detail;
		} else if (!strcmp(argv[i], "--time") && i + 1 < argc) {
			scene_ms = float(atof(argv[++i]));
		} else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
			shot = argv[++i];
		} else if (!strcmp(argv[i], "--eye") && i + 1 < argc) {
			// --eye x,y,z,yaw,pitch (radians)
			have_eye = sscanf(argv[++i], "%f,%f,%f,%f,%f", &eye_arg[0], &eye_arg[1], &eye_arg[2], &eye_arg[3], &eye_arg[4]) == 5;
		}
	}
	try {
		// Indexing every file's GUIDs reads all ~728 MB of data (about 20 s from an external drive).
		fprintf(stderr, "Loading game data from %s (this takes a while)...\n", argv[1]);
		de::World world(argv[1]);
		fprintf(stderr, "Building %s...\n", argv[2]);
		auto *file = world.file(argv[2]);
		if (!file) {
			throw std::runtime_error(std::string("no arena file ") + argv[2]);
		}

		SDL_Init(SDL_INIT_VIDEO);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
		const int W = 1280, H = 720;
		SDL_Window *win = SDL_CreateWindow((std::string("One Must Fall: Battlegrounds — ") + argv[2]).c_str(),
		                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
		                                   SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | (shot ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
		SDL_GLContext ctx = SDL_GL_CreateContext(win);
		SDL_GL_SetSwapInterval(1);
		render::g_live_values = [&](const de::ObjectRef &r) -> std::optional<float> {
			if (r.file == file && r.info().name == "SceneTime") return scene_ms;
			return std::nullopt;
		};
		render::ArenaRenderer arena;
		arena.load(world, argv[2]);
		render::GlowRenderer glow;
		// The arena's lights where the file places them (scripts may move or dim them in a fight).
		for (auto &pl : de::read_scene_lights(world, *file)) {
			render::SceneLight l;
			l.frame = pl.world;
			l.pos = pl.world.t;
			l.shape = pl.cone ? render::SceneLight::kCone : render::SceneLight::kSphere;
			l.inner = pl.inner, l.outer = pl.outer, l.umbra = pl.umbra, l.penumbra = pl.penumbra;
			l.near_begin = pl.near_begin, l.near_end = pl.near_end;
			l.position_static = pl.position_static;
			auto c = pl.info.colour.eval(0);
			l.colour = {c[0], c[1], c[2]};
			l.intensity = pl.info.intensity;
			l.mask = pl.info.mask();
			render::g_lighting.lights.push_back(l);
		}
		const float *lo = arena.lo, *hi = arena.hi;
		printf("%s: %zu triangles, %zu lights, bounds %.0f..%.0f %.0f..%.0f %.0f..%.0f\n", argv[2], arena.triangles, render::g_lighting.lights.size(), lo[0], hi[0],
		       lo[1], hi[1], lo[2], hi[2]);

		// Start above the fighting area looking across it.
		float eye[3] = {0, hi[1] * 0.35f + 120.0f, (hi[2] - lo[2]) * 0.30f};
		float yaw = 0, pitch = 0.22f;
		if (have_eye) {
			memcpy(eye, eye_arg, sizeof eye);
			yaw = eye_arg[3];
			pitch = eye_arg[4];
		}
		if (shot) {
			glu::offscreen(W, H);
		}
		bool running = true, dragging = false;
		Uint64 last = SDL_GetPerformanceCounter();
		while (running) {
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) running = false;
				if (e.type == SDL_MOUSEBUTTONDOWN) dragging = true;
				if (e.type == SDL_MOUSEBUTTONUP) dragging = false;
				if (e.type == SDL_MOUSEMOTION && dragging) {
					yaw += e.motion.xrel * 0.005f;
					pitch = std::fmax(-1.5f, std::fmin(1.5f, pitch + e.motion.yrel * 0.005f));
				}
			}
			Uint64 now = SDL_GetPerformanceCounter();
			float dt = float(now - last) / float(SDL_GetPerformanceFrequency());
			last = now;
			if (!shot) scene_ms += dt * 1000;
			const Uint8 *k = SDL_GetKeyboardState(nullptr);
			float speed = (k[SDL_SCANCODE_LSHIFT] ? 1500.0f : 300.0f) * dt;
			float fx = std::sin(yaw), fz = -std::cos(yaw);
			if (k[SDL_SCANCODE_W]) { eye[0] += fx * speed; eye[2] += fz * speed; }
			if (k[SDL_SCANCODE_S]) { eye[0] -= fx * speed; eye[2] -= fz * speed; }
			if (k[SDL_SCANCODE_A]) { eye[0] += fz * speed; eye[2] -= fx * speed; }
			if (k[SDL_SCANCODE_D]) { eye[0] -= fz * speed; eye[2] += fx * speed; }
			if (k[SDL_SCANCODE_E]) eye[1] += speed;
			if (k[SDL_SCANCODE_Q]) eye[1] -= speed;

			int dw = W, dh = H;
			if (!shot) SDL_GL_GetDrawableSize(win, &dw, &dh);
			glViewport(0, 0, dw, dh);
			glEnable(GL_DEPTH_TEST);
			glClearColor(0, 0, 0, 1);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			float proj[16], view[16], vp[16];
			glu::perspective(proj, 60.0f, float(dw) / float(dh), 2.0f, 20000.0f);
			glu::fps_view(view, eye, yaw, pitch);
			glu::mul(vp, proj, view);
			arena.draw(vp, eye);
			glow.begin();
			arena.draw_glow(vp, eye);
			glow.end(dw, dh);
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
