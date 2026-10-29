// openbg: the game's front end (menus), run by the game's own GUI data.
//
//   openbg <Data dir> [--screenshot out.png --frames N]
//
// The menus are the 3D GUI scene in OMFBGGUI.DEObjects (FINDINGS 125, 126): the persistent V.*
// actors (frame, doors, backdrop, environment) and one F.<Screen> folder per page, its VE.*
// actors the buttons, driven by the UI managers in game/ui.h.
//
// MOUSE="x,y@frame;down@frame;up@frame;..." scripts the mouse in screenshot runs (x, y fractions
// of the 4:3 GUI area, y down); SHOTS="40,80" also writes <out>-<frame>.png.
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "de/lighting.h"
#include "game/runtime.h"
#include "game/session.h"
#include "game/ui.h"
#include "audio.h"
#include "gl_util.h"
#include "overlay.h"
#include "particles_gl.h"
#include "render.h"
#include "scene_lights.h"

namespace {

// The GUI's viewport: 4:3, as the layout's 4/3 (DE_CUIManager::SetupControlArea) assumes,
// pillar- or letterboxed in the window.
struct Viewport {
	int x = 0, y = 0, w = 1, h = 1;
};
Viewport gui_viewport(int dw, int dh) {
	Viewport v;
	if (dw * 3 > dh * 4) {
		v.h = dh, v.w = dh * 4 / 3, v.x = (dw - v.w) / 2;
	} else {
		v.w = dw, v.h = dw * 3 / 4, v.y = (dh - v.h) / 2;
	}
	return v;
}

// A notification's text in lines of about `width` characters.
std::vector<std::string> wrap(const std::string &text, size_t width) {
	std::vector<std::string> lines;
	size_t p = 0;
	while (p <= text.size()) {
		size_t nl = text.find('\n', p);
		std::string para = text.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
		std::string line;
		size_t q = 0;
		while (q < para.size()) {
			size_t sp = para.find(' ', q);
			std::string word = para.substr(q, sp == std::string::npos ? std::string::npos : sp - q);
			if (!line.empty() && line.size() + 1 + word.size() > width) {
				lines.push_back(line);
				line.clear();
			}
			line += (line.empty() ? "" : " ") + word;
			if (sp == std::string::npos) break;
			q = sp + 1;
		}
		lines.push_back(line);
		if (nl == std::string::npos) break;
		p = nl + 1;
	}
	return lines;
}

}  // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <Data dir> [--screenshot out.png --frames N]\n", argv[0]);
		return 2;
	}
	std::string data = argv[1];
	const char *shot = nullptr;
	int frames = 60;
	for (int i = 2; i < argc; ++i) {
		if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
	}
	struct MouseStep {
		int frame;
		int kind;  // 0 move, 1 down, 2 up
		float x, y;
	};
	std::vector<MouseStep> mouse_script;
	if (const char *m = getenv("MOUSE")) {
		std::string spec = m;
		for (size_t p = 0; p < spec.size();) {
			size_t q = spec.find(';', p);
			std::string item = spec.substr(p, q == std::string::npos ? std::string::npos : q - p);
			float x, y;
			int f;
			if (sscanf(item.c_str(), "%f,%f@%d", &x, &y, &f) == 3) mouse_script.push_back({f, 0, x, y});
			else if (sscanf(item.c_str(), "down@%d", &f) == 1) mouse_script.push_back({f, 1, 0, 0});
			else if (sscanf(item.c_str(), "up@%d", &f) == 1) mouse_script.push_back({f, 2, 0, 0});
			if (q == std::string::npos) break;
			p = q + 1;
		}
	}
	try {
		fprintf(stderr, "Loading game data from %s (this takes a while)...\n", data.c_str());
		game::World world(data);
		game::ui::AppManager app(world);
		// The launcher's InitGUI (not readable): the GUI scene's persistent actors, matched to the
		// app manager's roles by their names (FINDINGS 125 inference).
		auto persistent = [&](const char *name) {
			game::GameObject *o = app.construct(name);
			if (o) {
				o->gui = true;
				world.enter_scene(o);
			}
			return o;
		};
		persistent("V.Environment");
		app.backdrop = persistent("V.Backdrop");
		app.main_frame = persistent("V.MainFrame");
		app.transition = persistent("V.MainDoors");
		app.set_active(app.backdrop, false);
		app.set_active(app.main_frame, false);
		// DE_CGameLauncherWindow::ObjInitContents: the main menu is the first UI manager.
		app.set_next(app.main_menu(), true);

		SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
		const int W = 1024, H = 768;
		SDL_Window *win = SDL_CreateWindow("One Must Fall: Battlegrounds", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
		                                   SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE |
		                                       (shot ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
		SDL_GLContext ctx = SDL_GL_CreateContext(win);
		SDL_GL_SetSwapInterval(1);
		if (shot) glu::offscreen(W, H);
		render::ActorMeshRenderer actors_r;
		render::ParticleRenderer particles_r;
		particles_r.load();
		render::OverlayRenderer overlay;
		overlay.load(world.data());
		audio::Mixer mixer;
		if (!shot && !mixer.open()) fprintf(stderr, "no audio device: %s\n", SDL_GetError());
		if (auto *gf = world.data().file(app.transfer_agent)) render::g_lighting.ambient = de::read_ambient(world.data(), *gf);
		world.render_ticks_per_frame = shot != nullptr;
		auto gui_only = [](const game::GameObject *o) { return game::in_gui(*o); };
		Viewport vpt = gui_viewport(W, H);
		app.text_width = [&](const std::string &t, float size) { return overlay.text_width(t, size, vpt.w, vpt.h); };

		// The UI parent frame: local x 0..1 across the width and y 0..0.75 up, one unit from the
		// 60-degree frame-control camera, which looks down -Z (DE_CTopControl3d::AdjustRootActor,
		// AssureRenderObjects; FINDINGS 126).
		const float hfov = 60.0f * 0.017453292f, aspect = 4.0f / 3.0f;
		const float cam_d = 0.5f / std::tan(hfov / 2);
		de::Transform listener;
		listener.t = {0.5f, 0.5f / aspect, cam_d};

		bool running = true, ui_begun = false;
		double accum = 0;
		Uint64 last = SDL_GetPerformanceCounter(), last_render = last;
		for (int fr = 0; running;) {
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_QUIT) running = false;
				if (e.type == SDL_MOUSEMOTION) {
					int ww, wh, dw, dh;
					SDL_GetWindowSize(win, &ww, &wh);
					SDL_GL_GetDrawableSize(win, &dw, &dh);
					float px = float(e.motion.x) * float(dw) / float(ww), py = float(e.motion.y) * float(dh) / float(wh);
					app.mouse_move((px - float(vpt.x)) / float(vpt.w), (py - float(dh - vpt.y - vpt.h)) / float(vpt.h));
				}
				if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) app.mouse_button(true, e.button.clicks);
				if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) app.mouse_button(false);
				if (e.type == SDL_KEYDOWN) app.key(int(e.key.keysym.sym));
			}
			if (app.quit_requested) break;
			Uint64 now = SDL_GetPerformanceCounter();
			accum += double(now - last) * 1000.0 / double(SDL_GetPerformanceFrequency());
			last = now;
			uint32_t frame_ms = 0;
			if (shot) {
				frame_ms = game::World::kFrameMs;
			} else if (accum >= game::World::kFrameMs) {
				frame_ms = uint32_t(std::min(accum, 2000.0));
				accum = accum > 2000 ? 0 : accum - frame_ms;
			}
			if (frame_ms == 0) {
				SDL_Delay(1);
				continue;
			}
			for (auto &m : mouse_script) {
				if (m.frame != fr) continue;
				if (m.kind == 0) app.mouse_move(m.x, m.y);
				else app.mouse_button(m.kind == 1);
			}
			// The launcher's first control event 0x3a: DE_CAppManager::BeginUI.
			if (!ui_begun) {
				ui_begun = true;
				app.begin_ui();
			}
			app.update();
			world.tick(frame_ms);
			++fr;
			mixer.play_requests(world, listener);
			mixer.update_music(world);
			if (!shot) {
				Uint64 t = SDL_GetPerformanceCounter();
				float ms = float(double(t - last_render) * 1000.0 / double(SDL_GetPerformanceFrequency()));
				last_render = t;
				game::render_tick(world, std::fmin(ms, 100.0f));
			}

			int dw = W, dh = H;
			if (!shot) SDL_GL_GetDrawableSize(win, &dw, &dh);
			vpt = gui_viewport(dw, dh);
			glViewport(0, 0, dw, dh);
			glClearColor(0, 0, 0, 1);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glViewport(vpt.x, vpt.y, vpt.w, vpt.h);
			glEnable(GL_DEPTH_TEST);
			float vfov = 2.0f * std::atan(std::tan(hfov / 2) / aspect) / 0.017453292f;
			float eye[3] = {listener.t[0], listener.t[1], listener.t[2]}, fwd[3] = {0, 0, -1}, up[3] = {0, 1, 0};
			float proj[16], view[16], vp[16];
			glu::perspective(proj, vfov, aspect, 0.01f, 100.0f);
			glu::look_view(view, eye, fwd, up);
			glu::mul(vp, proj, view);
			render::FrameLights lights = render::collect_lights(world, fr, gui_only);
			std::vector<render::ActorMeshRenderer::Item> items;
			for (game::GameObject *o : world.scene_objects()) {
				if (!o->is_actor || !o->file || o->meshes_disabled || !game::in_scene(*o) || !game::in_gui(*o)) continue;
				bool live = true;
				for (const game::GameObject *p = o; p && live; p = p->parent) live = p->visible;
				if (live) items.push_back({{o->file, o->index}, o->world(), lights.owner_of(o)});
			}
			render::update_animated_textures(world.image_phases);
			actors_r.texture_version = world.texture_override_version;
			actors_r.draw(world.data(), items, vp, eye);
			particles_r.draw(world, vp, view, eye);
			overlay.draw_world(world, vp);
			overlay.draw(world, vpt.w, vpt.h, nullptr);
			// The controls: button labels (DE_CButtonControl3d::SetText: 16 of 600 virtual pixels,
			// centred; white, the colour the screens pass), then a notification over everything.
			const float white[4] = {1, 1, 1, 1};
			if (app.current && app.current->activated) {
				std::vector<game::ui::DrawItem> items;
				app.current->draw(app, items);
				for (auto &d : items) {
					if (d.kind == game::ui::DrawItem::Rect) overlay.rect(d.x0, d.y0, d.x1, d.y1, d.rgba, vpt.w, vpt.h);
					else if (d.kind == game::ui::DrawItem::Image) overlay.image_rect(d.image, d.x0, d.y0, d.x1, d.y1, vpt.w, vpt.h);
					else overlay.label(d.text, d.x0, d.y0, d.size, d.rgba, vpt.w, vpt.h, d.centered);
				}
				for (auto &c : app.current->controls) {
					overlay.label(c.text, (c.left + c.right) / 2, (c.top + c.bottom) / 2, 16.0f / 600.0f, white, vpt.w, vpt.h);
				}
			}
			if (!app.notifications.empty()) {
				auto &n = app.notifications.front();
				auto lines = wrap(n.text, 52);
				float h = 0.12f + 0.035f * float(lines.size()), y0 = 0.5f - h / 2;
				const float back[4] = {0.05f, 0.08f, 0.12f, 0.9f}, edge[4] = {0.55f, 0.75f, 0.9f, 0.9f};
				overlay.rect(0.18f, y0 - 0.004f, 0.82f, y0 + h + 0.004f, edge, vpt.w, vpt.h);
				overlay.rect(0.185f, y0, 0.815f, y0 + h, back, vpt.w, vpt.h);
				overlay.label(n.title, 0.5f, y0 + 0.035f, 0.035f, white, vpt.w, vpt.h);
				for (size_t i = 0; i < lines.size(); ++i) overlay.label(lines[i], 0.5f, y0 + 0.08f + 0.035f * float(i), 0.026f, white, vpt.w, vpt.h);
				overlay.label("OK", 0.5f, y0 + h - 0.025f, 0.026f, edge, vpt.w, vpt.h);
			}
			glViewport(0, 0, dw, dh);
			if (shot) {
				auto save = [&](const std::string &out) {
					std::vector<uint8_t> px(size_t(W) * H * 4);
					glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
					glu::write_png(out.c_str(), W, H, px);
					printf("wrote %s\n", out.c_str());
				};
				if (fr >= frames) {
					save(shot);
					break;
				}
				if (const char *list = getenv("SHOTS")) {
					std::string l = std::string(",") + list + ",";
					if (l.find("," + std::to_string(fr) + ",") != std::string::npos) save(std::string(shot) + "-" + std::to_string(fr) + ".png");
				}
				continue;
			}
			SDL_GL_SwapWindow(win);
		}
		SDL_GL_DeleteContext(ctx);
		SDL_DestroyWindow(win);
		SDL_Quit();
	} catch (const std::exception &e) {
		fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
	return 0;
}
