// openbg: the game's front end (menus), run by the game's own GUI data.
//
//   openbg <Data dir> [--screenshot out.png --frames N]
//
// The menus are the 3D GUI scene in OMFBGGUI.DEObjects (FINDINGS 125, 126): the persistent V.*
// actors (frame, doors, backdrop, environment) and one F.<Screen> folder per page, its VE.*
// actors the buttons, driven by the UI managers in game/ui.h.
//
// --launch <match> starts that match's session at once; --choose Elizabeth,Jaguar makes the UIs
// it launches pick those (testing); --headless N runs N frames without a window and lists the
// natives still missing.
//
// MOUSE="x,y@frame;down@frame;up@frame;key@frame;esc@frame;..." scripts the mouse (and Enter, Esc) in screenshot runs (x, y fractions
// of the 4:3 GUI area, y down); SHOTS="40,80" also writes <out>-<frame>.png.
#include <SDL.h>

#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "de/lighting.h"
#include "game/runtime.h"
#include "game/session.h"
#include "game/descriptions.h"
#include "game/ui.h"
#include "audio.h"
#include "game_view.h"
#include "gl_util.h"
#include "overlay.h"
#include "particles_gl.h"
#include "render.h"
#include "scene_lights.h"

namespace {

// The game's registry (HKCU\Software\Diversions Ent\One Must Fall Battlegrounds in the original),
// kept as "name=value" lines in the user's Application Support folder.
std::string registry_path() {
	const char *home = getenv("HOME");
	return std::string(home ? home : ".") + "/Library/Application Support/openbg/registry.txt";
}
bool load_registry(std::map<std::string, std::string> &reg) {
	FILE *f = fopen(registry_path().c_str(), "r");
	if (!f) return false;
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		std::string l = line;
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
		size_t eq = l.find('=');
		if (eq != std::string::npos) reg[l.substr(0, eq)] = l.substr(eq + 1);
	}
	fclose(f);
	return true;
}
void save_registry(const std::map<std::string, std::string> &reg) {
	std::string dir = registry_path().substr(0, registry_path().rfind('/'));
	std::string cmd = "mkdir -p '" + dir + "'";
	if (system(cmd.c_str()) != 0) return;
	FILE *f = fopen(registry_path().c_str(), "w");
	if (!f) return;
	for (auto &[k, v] : reg) fprintf(f, "%s=%s\n", k.c_str(), v.c_str());
	fclose(f);
}

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
	std::string launch;
	bool simulate_fullscreen = false;  // the launcher's "simulatefullscreen" switch
	std::vector<std::string> choose;  // --choose: names launched UIs pick at once (testing)
	bool headless = false;
	int frames = 60;
	for (int i = 2; i < argc; ++i) {
		if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--launch") && i + 1 < argc) launch = argv[++i];
		else if (!strcmp(argv[i], "simulatefullscreen") || !strcmp(argv[i], "-simulatefullscreen")) simulate_fullscreen = true;
		else if (!strcmp(argv[i], "--choose") && i + 1 < argc) {
			std::string l = argv[++i];
			for (size_t p = 0; p <= l.size();) {
				size_t q = l.find(',', p);
				choose.push_back(l.substr(p, q == std::string::npos ? std::string::npos : q - p));
				if (q == std::string::npos) break;
				p = q + 1;
			}
		}
		else if (!strcmp(argv[i], "--headless")) {
			headless = true;
			if (i + 1 < argc && argv[i + 1][0] != '-') frames = atoi(argv[++i]);
		}
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
			else if (sscanf(item.c_str(), "key@%d", &f) == 1) mouse_script.push_back({f, 3, 0, 0});
			else if (sscanf(item.c_str(), "esc@%d", &f) == 1) mouse_script.push_back({f, 4, 0, 0});
			if (q == std::string::npos) break;
			p = q + 1;
		}
	}
	try {
		fprintf(stderr, "Loading game data from %s (this takes a while)...\n", data.c_str());
		game::World world(data);
		game::ui::AppManager app(world);
		// DE_GLOBAL::FirstRun: Core's DE_CRegistry found no registry yet.
		bool first_run = false;
		if (!shot && !headless) first_run = !load_registry(world.registry);
		app.apply_settings();
		auto saved_registry = world.registry;
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
		app.session_overlay = persistent("V.StandardSceneOverlay");
		app.set_active(app.session_overlay, false);
		app.set_active(app.backdrop, false);
		app.set_active(app.main_frame, false);
		app::GameView view(world);
		view.wire_session();
		// DE_COMF2Manager::ProcessEvent on the session chooser's Complete (FINDINGS 125): a session
		// from the chosen master description's resource (its master modifier), a local
		// user-controlled client named "User", then Begin.
		app.launch_session = [&](const de::ObjectRef &desc, bool single_player) {
			auto res = game::passive_references(world.data(), desc);
			if (res.empty()) {
				app.notify(desc.info().name, "This match has no session to start.");
				return;
			}
			printf("launching %s (%s)%s\n", desc.info().name.c_str(), res[0].info().name.c_str(), single_player ? ", single player" : "");
			auto &ss = game::start_session(world, {res[0]});
			ss.master_description = world.global_object(desc);
			ss.single_player = single_player;
			ss.max_player_clients = 4;  // the session info's MaxPlayers (where the original sets it is not found)
			game::add_client(world, true, "User");
			game::begin_session(world);
			app.set_registry_bool("SinglePlayerPlayed", true);
		};
		// A UI manager the session scripts launch (FINDINGS 127): a description selector over the
		// script's pool; Next completes it for the script.
		world.launch_ui = [&](game::GameObject *ui) {
			auto &u = world.session->ui[ui];
			auto sel = std::make_shared<game::ui::DescriptionSelector>(u.description);
			sel->title = u.description;
			sel->autoselect_key = u.autoselect;
			sel->choose_fighter = u.choose_fighter;
			for (auto &g : u.pool) {
				if (de::ObjectRef d = world.data().by_guid(g)) sel->pool.push_back(d);
			}
			sel->on_complete = [&world, ui](game::ui::AppManager &, game::ui::DescriptionSelector &s) {
				if (!s.chosen) return;
				if (s.chosen_model) game::complete_ui(world, ui, s.chosen.info().guid, &s.chosen_model.info().guid);
				else game::complete_ui(world, ui, s.chosen.info().guid);
			};
			printf("UI \"%s\": %zu descriptions\n", u.description.c_str(), sel->pool.size());
			for (auto &d : sel->pool) {
				if (std::find(choose.begin(), choose.end(), d.info().name) != choose.end()) {
					printf("UI \"%s\": chose %s\n", u.description.c_str(), d.info().name.c_str());
					game::complete_ui(world, ui, d.info().guid);
					return;
				}
			}
			app.set_next(sel, true);
		};
		// DE_CRulesDisplay::Begin: SetNextUIManager(this), the same manager each time (a no-op
		// while it is still current or next).
		std::map<game::GameObject *, game::ui::UIPtr> rules_pages;
		world.show_rules = [&](game::GameObject *rules) {
			auto &page = rules_pages[rules];
			if (!page) page = app.rules_display(rules);
			if (getenv("OPENBG_DEBUG_UI")) fprintf(stderr, "[ui %u] rules display %x shown\n", world.frame(), rules->handle);
			app.set_next(page, true);
		};
		world.close_ui_stack = [&] {
			if (getenv("OPENBG_DEBUG_UI")) fprintf(stderr, "[ui %u] close stack (current %s, next %s, state %d)\n", world.frame(), app.current ? app.current->name.c_str() : "-", app.next ? app.next->name.c_str() : "-", app.transition_state);
			app.close_stack();
		};
		app.has_local_session = [&] { return world.session != nullptr; };
		app.session_fully_sequenced = [] { return true; };
		app.session_scene_rendering = [&] { return world.session != nullptr && view.has_arena(); };
		if (!launch.empty()) {
			de::ObjectRef d = world.data().find("Description\\SessionMaster", launch);
			if (!d) {
				fprintf(stderr, "no match named %s\n", launch.c_str());
				return 1;
			}
			app.launch_session(d, true);
		} else {
			// DE_CGameLauncherWindow::ObjInitContents: the main menu is the first UI manager.
			app.set_next(app.main_menu(), true);
		}
		if (headless) {
			// OPENBG_RECORD_AUDIO=<file>: what is heard, mixed offline (raw f32 stereo, 44100 Hz).
			std::unique_ptr<audio::Mixer> offline;
			if (getenv("OPENBG_RECORD_AUDIO")) {
				offline = std::make_unique<audio::Mixer>();
				offline->open_offline(44100);
			}
			double audio_carry = 0;
			de::Transform ear;
			ear.t = {0.5f, 0.375f, 0.866f};
			for (int fr = 0; fr < frames; ++fr) {
				for (auto &m : mouse_script) {
					if (m.frame == fr && m.kind == 3) app.key(13);
					if (m.frame == fr && m.kind == 4) app.key(27);
				}
				app.update();
				world.tick(app.autopaused() && world.session ? 0 : game::World::kFrameMs);
				if (offline) {
					offline->play_requests(world, view.player ? view.ear() : ear);
					offline->update_music(world);
					audio_carry += 44100.0 * world.frame_ms() / 1000.0;
					int n = int(audio_carry);
					audio_carry -= n;
					offline->pump(n);
				}
				for (auto &[obj, t] : world.texts) {
					static std::map<const game::GameObject *, std::string> shown;
					if (t.text != shown[obj] && !t.text.empty()) printf("frame %5d  [screen] %s\n", fr, t.text.c_str());
					shown[obj] = t.text;
				}
				world.sound_requests.clear();
			}
			for (auto &l : world.log) printf("log: %s\n", l.c_str());
			std::vector<std::pair<uint64_t, std::string>> miss;
			for (auto &[k, n] : world.missing()) miss.emplace_back(n, k);
			std::sort(miss.rbegin(), miss.rend());
			printf("%zu natives still missing (by calls):\n", miss.size());
			for (auto &[n, k] : miss) printf("  %6llu  %s\n", (unsigned long long)n, k.c_str());
			return 0;
		}

		SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
		// Preferences\Renderer\Resolution and Full Screen (the options' Video panel; next start).
		static const int kRes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 960}, {1600, 1200}};
		std::string res = app.registry_string("Preferences\\Renderer\\Resolution");
		int ri = shot || res.empty() ? 2 : std::clamp(atoi(res.c_str()), 0, 4);
		const int W = kRes[ri][0], H = kRes[ri][1];
		bool fullscreen = !shot && app.registry_bool("Preferences\\Renderer\\Full Screen");
		SDL_Window *win = SDL_CreateWindow("One Must Fall: Battlegrounds", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
		                                   SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE |
		                                       (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) | (shot ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
		app.key_name = [](int sc) { return std::string(SDL_GetScancodeName(SDL_Scancode(sc))); };
		SDL_StartTextInput();
		SDL_GLContext ctx = SDL_GL_CreateContext(win);
		SDL_GL_SetSwapInterval(1);
		if (shot) glu::offscreen(W, H);
		view.init_gl();
		view.follow_scene = true;
		render::ActorMeshRenderer actors_r;
		render::ParticleRenderer particles_r;
		particles_r.load();
		render::OverlayRenderer overlay;
		overlay.load(world.data());
		audio::Mixer mixer;
		if (!shot && !mixer.open()) fprintf(stderr, "no audio device: %s\n", SDL_GetError());
		// Render ticks pass real time even while the scene is autopaused (the renderer's clock).
		world.render_ticks_per_frame = false;
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
					float mx = (px - float(vpt.x)) / float(vpt.w), my = (py - float(dh - vpt.y - vpt.h)) / float(vpt.h);
					app.mouse_move(mx, my);
					world.mouse_x = mx;
					world.mouse_y = 1 - my;
				}
				if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) app.mouse_button(true, e.button.clicks);
				if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) app.mouse_button(false);
				if (e.type == SDL_KEYDOWN) app.key(int(e.key.keysym.sym), int(e.key.keysym.scancode));
				if (e.type == SDL_TEXTINPUT) app.text(e.text.text);
				if (e.type == SDL_MOUSEWHEEL) app.wheel(e.wheel.y);
			}
			if (world.registry != saved_registry) {
				save_registry(world.registry);
				saved_registry = world.registry;
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
				if (m.kind == 0) {
					app.mouse_move(m.x, m.y);
					world.mouse_x = m.x;
					world.mouse_y = 1 - m.y;
				}
				else if (m.kind == 3) app.key(13);
				else if (m.kind == 4) app.key(27);
				else app.mouse_button(m.kind == 1);
			}
			// The launcher's first control event 0x3a: DE_CAppManager::BeginUI. On a first run
			// (and without "simulatefullscreen") the launcher also greets the player and launches
			// SinglePlayer\Basic Training, which the retail data does not have.
			if (!ui_begun) {
				ui_begun = true;
				if (first_run && !simulate_fullscreen) {
					app.notify("Welcome", "");
					de::ObjectRef basic = world.data().find("Description\\SessionMaster", "Basic Training");
					if (basic && app.launch_session) app.launch_session(basic, true);
				}
				app.begin_ui();
			}
			// The user's creature hears the keyboard unless a page is up that disables its input
			// (DE_CAppManager::IsUIManagerDisableUserCreatureInput).
			world.input = {};
			if (!shot && !(app.current && app.current->input_off)) {
				// The options' key bindings, in AppManager::actions() order.
				static bool game::Input::*const fields[] = {&game::Input::up, &game::Input::down, &game::Input::left, &game::Input::right,
				                                             &game::Input::left_punch, &game::Input::right_punch, &game::Input::left_kick,
				                                             &game::Input::right_kick, &game::Input::jump, &game::Input::evade, &game::Input::duck};
				const Uint8 *k = SDL_GetKeyboardState(nullptr);
				for (size_t i = 0; i < std::size(fields) && i < game::ui::AppManager::actions().size(); ++i) {
					int sc = app.binding(i);
					if (sc > 0 && sc < SDL_NUM_SCANCODES && k[sc]) world.input.*fields[i] = true;
				}
			}
			app.update();
			// Autopause: the scene passes no time while a page that autopauses is up (the
			// sessions still update).
			world.tick(app.autopaused() && world.session ? 0 : frame_ms);
			++fr;
			mixer.play_requests(world, view.player ? view.ear() : listener);
			mixer.update_music(world);
			{
				Uint64 t = SDL_GetPerformanceCounter();
				float ms = shot ? float(game::World::kFrameMs) : float(double(t - last_render) * 1000.0 / double(SDL_GetPerformanceFrequency()));
				last_render = t;
				game::render_tick(world, std::fmin(ms, 100.0f));
			}

			int dw = W, dh = H;
			if (!shot) SDL_GL_GetDrawableSize(win, &dw, &dh);
			vpt = gui_viewport(dw, dh);
			glViewport(0, 0, dw, dh);
			// A session's scene (DE_CSceneViewerControl3d over the whole window), then the GUI.
			bool game_shown = world.session != nullptr;
			if (game_shown) {
				view.draw(dw, dh, fr, shot != nullptr);
				game_shown = view.has_arena();
			}
			if (!game_shown) {
				glClearColor(0, 0, 0, 1);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			} else {
				glClear(GL_DEPTH_BUFFER_BIT);
			}
			glViewport(vpt.x, vpt.y, vpt.w, vpt.h);
			glEnable(GL_DEPTH_TEST);
			float vfov = 2.0f * std::atan(std::tan(hfov / 2) / aspect) / 0.017453292f;
			float eye[3] = {listener.t[0], listener.t[1], listener.t[2]}, fwd[3] = {0, 0, -1}, up[3] = {0, 1, 0};
			float proj[16], view[16], vp[16];
			glu::perspective(proj, vfov, aspect, 0.01f, 100.0f);
			glu::look_view(view, eye, fwd, up);
			glu::mul(vp, proj, view);
			render::Lighting game_lighting = render::g_lighting;
			if (auto *gf = world.data().file(app.transfer_agent)) render::g_lighting.ambient = de::read_ambient(world.data(), *gf);
			render::FrameLights lights = render::collect_lights(world, fr, gui_only);
			std::vector<render::ActorMeshRenderer::Item> items;
			for (game::GameObject *o : world.scene_objects()) {
				if (!o->is_actor || !o->file || o->meshes_disabled || !game::in_scene(*o) || !game::in_gui(*o)) continue;
				bool live = true;
				for (const game::GameObject *p = o; p && live; p = p->parent) live = p->visible;
				if (const char *sk = getenv("OPENBG_SKIP_ACTOR")) {
					for (const game::GameObject *p = o; p && live; p = p->parent) live = p->name != sk;
				}
				static const int list_at = getenv("OPENBG_LIST_GUI") ? atoi(getenv("OPENBG_LIST_GUI")) : -1;
				if (live && fr == list_at) {
					auto t = o->world().t;
					const float *m = vp;
					float x = m[0] * t[0] + m[4] * t[1] + m[8] * t[2] + m[12], y = m[1] * t[0] + m[5] * t[1] + m[9] * t[2] + m[13], ww = m[3] * t[0] + m[7] * t[1] + m[11] * t[2] + m[15];
					fprintf(stderr, "[gui] %s under %s at %.2f %.2f\n", o->name.c_str(), o->parent ? o->parent->name.c_str() : "-", (x / ww + 1) / 2, 1 - (y / ww + 1) / 2);
				}
				if (live) items.push_back({{o->file, o->index}, o->world(), lights.owner_of(o)});
			}
			render::update_animated_textures(world.image_phases);
			actors_r.texture_version = world.texture_override_version;
			actors_r.draw(world.data(), items, vp, eye);
			particles_r.draw(world, vp, view, eye);
			if (!game_shown) {
				overlay.draw_world(world, vp);
				overlay.draw(world, vpt.w, vpt.h, nullptr);
			}
			render::g_lighting = game_lighting;
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
