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
// MOUSE="x,y@frame;down@frame;up@frame;key@frame;esc@frame;char c@frame;wheel n@frame;..." scripts the mouse (and Enter, Esc, a character key, the wheel) in screenshot runs (x, y fractions
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

#include "de/contents.h"
#include "de/lighting.h"
#include "game/replay.h"
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

}  // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <Data dir> [--screenshot out.png --frames N]\n", argv[0]);
		return 2;
	}
	std::string data = argv[1];
	const char *shot = nullptr;
	std::string launch;
	std::string replay_path;  // --replay: play this recording at once (testing)
	std::vector<std::string> choose;  // --choose: names launched UIs pick at once (testing)
	bool headless = false;
	int frames = 60;
	for (int i = 2; i < argc; ++i) {
		if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--launch") && i + 1 < argc) launch = argv[++i];
		else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay_path = argv[++i];
		else if (!strcmp(argv[i], "simulatefullscreen") || !strcmp(argv[i], "-simulatefullscreen")) continue;  // the launcher's switch
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
			else if (char ch; sscanf(item.c_str(), "char %c@%d", &ch, &f) == 2) mouse_script.push_back({f, 5, float(ch), 0});
			else if (int dy; sscanf(item.c_str(), "wheel %d@%d", &dy, &f) == 2) mouse_script.push_back({f, 6, float(dy), 0});
			if (q == std::string::npos) break;
			p = q + 1;
		}
	}
	try {
		fprintf(stderr, "Loading game data from %s (this takes a while)...\n", data.c_str());
		game::World world(data);
		// DE_GLOBAL::SystemInit: srand(time(NULL)). Screenshot and headless runs keep the CRT's
		// start value (1) so they repeat; OPENBG_SEED picks one.
		if (const char *seed = getenv("OPENBG_SEED")) game::crt_srand(uint32_t(strtoul(seed, nullptr, 0)));
		else if (!shot && !headless) game::crt_srand(uint32_t(time(nullptr)));
		game::ui::AppManager app(world);
		if (!shot && !headless) load_registry(world.registry);
		game::migrate_lock_records(world.registry);
		// OPENBG_SET="name=value;..." sets registry values for this run (testing).
		if (const char *set = getenv("OPENBG_SET")) {
			std::string l = set;
			for (size_t p = 0; p < l.size();) {
				size_t q = l.find(';', p), e = l.find('=', p);
				if (q == std::string::npos) q = l.size();
				if (e != std::string::npos && e < q) world.registry[l.substr(p, e - p)] = l.substr(e + 1, q - e - 1);
				p = q + 1;
			}
		}
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
		struct RenderTarget {
			GLuint fbo = 0, tex = 0, depth = 0;
		};
		std::map<uint32_t, RenderTarget> render_targets;  // by image handle
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
			// DE_CSession::SetAsServer (Core 0x701f8110): a joinable session takes Network\Max Clients, others 20.
			int max_clients = atoi(app.registry_string("Network\\Max Clients").c_str());
			ss.max_player_clients = !single_player && max_clients > 0 ? max_clients : 20;
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
				// ChosenModelCaller (OMF 0x35056500): the model description's model (its +0xa8 GUID).
				auto res = s.chosen_model ? game::passive_references(world.data(), s.chosen_model) : std::vector<de::ObjectRef>{};
				if (!res.empty()) game::complete_ui(world, ui, s.chosen.info().guid, &res[0].info().guid);
				else game::complete_ui(world, ui, s.chosen.info().guid);
			};
			printf("UI \"%s\": %zu descriptions\n", u.description.c_str(), sel->pool.size());
			for (auto &d : sel->pool) {
				if (std::find(choose.begin(), choose.end(), d.info().name) != choose.end()) {
					printf("UI \"%s\": chose %s\n", u.description.c_str(), d.info().name.c_str());
					if (d.info().class_path == "Description\\Pilot") app.last_pilot = d;
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
			// A rules display whose page was closed before it came up is not shown any more either.
			for (auto &[rules, page] : rules_pages) {
				if (page && page != app.current && page != app.next) world.rules_displays[rules].shown = false;
			}
		};
		world.show_vcr = [&](game::GameObject *) { app.set_next(app.vcr_controls(), true); };
		app.launch_replay = [&](const std::string &path) { return game::start_replay(world, game::load_recording(path)); };
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
		} else if (!replay_path.empty()) {
			if (!game::start_replay(world, game::load_recording(replay_path))) {
				fprintf(stderr, "cannot replay %s\n", replay_path.c_str());
				return 1;
			}
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
					if (m.frame == fr && m.kind == 5) app.key(int(m.x)), app.text(std::string(1, char(m.x)));
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
		// The renderer's resolutions (GetAvailableResolutions): the display's modes.
		for (int i = 0, n = SDL_GetNumDisplayModes(0); i < n; ++i) {
			SDL_DisplayMode m;
			if (SDL_GetDisplayMode(0, i, &m) != 0) continue;
			std::pair<int, int> r{m.w, m.h};
			if (std::find(app.resolutions.begin(), app.resolutions.end(), r) == app.resolutions.end()) app.resolutions.push_back(r);
		}
		std::sort(app.resolutions.begin(), app.resolutions.end());
		// Preferences\Renderer\Resolution X / Y (the Video panel's; the next start) and Full Screen.
		const int W = shot ? 1024 : std::max(320, app.render_started.res_x), H = shot ? 768 : std::max(240, app.render_started.res_y);
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
		auto gui_only = [](const game::GameObject *o) { return game::in_gui(*o) && game::gui_view(*o) == 0; };
		auto picker_only = [](const game::GameObject *o) { return game::in_gui(*o) && game::gui_view(*o) == 1; };
		render::FighterRenderer picker_model;
		de::ObjectRef picker_model_shown, picker_pilot_shown;
		Viewport vpt = gui_viewport(W, H);
		app.text_width = [&](const std::string &t, float size) { return overlay.text_width(t, size, vpt.w, vpt.h); };
		app.char_offsets = [&](const std::string &t, float size) { return overlay.char_offsets(t, size, vpt.w, vpt.h); };
		app.view_w = vpt.w, app.view_h = vpt.h;

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
					if (world.relative_mouse) world.mouse_motion[0] += float(e.motion.xrel), world.mouse_motion[1] += float(e.motion.yrel);
				}
				if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP || e.type == SDL_MOUSEBUTTONDOWN) {
					SDL_Keymod mods = SDL_GetModState();
					app.shift_held = (mods & KMOD_SHIFT) != 0;
					app.ctrl_held = (mods & KMOD_CTRL) != 0;
				}
				if (e.type == SDL_MOUSEBUTTONDOWN) {
					int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
					if (b >= 0) app.mouse_press(b);
				}
				if (e.type == SDL_KEYUP) app.key_up(int(e.key.keysym.sym), int(e.key.keysym.scancode));
				if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) app.mouse_button(true, e.button.clicks);
				if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) app.mouse_button(false);
				if (e.type == SDL_KEYDOWN) app.key(int(e.key.keysym.sym), int(e.key.keysym.scancode));
				if (e.type == SDL_TEXTINPUT) app.text(e.text.text);
				if (e.type == SDL_MOUSEWHEEL) app.wheel(e.wheel.y);
			}
			// BVCRControls deleted its UI manager: the panel closes.
			if (world.vcr_handle && !world.object(world.vcr_handle)) {
				world.vcr_handle = 0, world.vcr_ui = nullptr;
				if (app.current == app.vcr_controls() || app.next == app.vcr_controls()) {
					if (app.stack.empty()) app.set_next(nullptr, false);
					else app.set_last();
				}
			}
			// Screenshot and headless runs neither load nor save the user's registry.
			if (!shot && !headless && world.registry != saved_registry) {
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
				else if (m.kind == 5) app.key(int(m.x)), app.text(std::string(1, char(m.x)));
				else if (m.kind == 6) app.wheel(int(m.x));
				else app.mouse_button(m.kind == 1);
			}
			// The launcher's first control event 0x3a: DE_CAppManager::BeginUI. The retail launcher
			// has no first-run greeting: its imports lack FirstRun, FindMasterObject and LaunchSession,
			// which the 2004 build's greeting (and its SinglePlayer\Basic Training launch) use.
			if (!ui_begun) {
				ui_begun = true;
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
			world.scene_paused = app.autopaused() && world.session;
			world.tick(app.autopaused() && world.session ? 0 : world.scene_step(float(frame_ms)));
			++fr;
			mixer.play_requests(world, view.player ? view.ear() : listener);
			mixer.update_music(world);
			{
				Uint64 t = SDL_GetPerformanceCounter();
				float ms = shot ? float(game::World::kFrameMs) : float(double(t - last_render) * 1000.0 / double(SDL_GetPerformanceFrequency()));
				last_render = t;
				game::render_tick(world, std::fmin(ms, 100.0f));
				if (dynamic_cast<game::ui::ModelPicker *>(app.current.get())) world.view_scene_ms += std::fmin(ms, 100.0f);
			}

			int dw = W, dh = H;
			if (!shot) SDL_GL_GetDrawableSize(win, &dw, &dh);
			vpt = gui_viewport(dw, dh);
			{
				// Controls are sized in the original's screen pixels: window points, not a HiDPI
				// display's drawable pixels.
				int ww = dw, wh = dh;
				if (!shot) SDL_GetWindowSize(win, &ww, &wh);
				float points = dw > 0 ? float(ww) / float(dw) : 1.0f;
				app.view_w = int(float(vpt.w) * points), app.view_h = int(float(vpt.h) * points);
			}
			glViewport(0, 0, dw, dh);
			// A session's scene (DE_CSceneViewerControl3d over the whole window), then the GUI.
			bool game_shown = world.session != nullptr;
			if (game_shown) {
				view.draw(dw, dh, fr, shot != nullptr);
				game_shown = view.has_arena();
			}
			// OPENBG_AUTOPLAY=<frame>: the player's fighter is handed to the AI then (testing whole matches).
			if (const char *ap = getenv("OPENBG_AUTOPLAY"); ap && fr >= atoi(ap)) {
				for (game::GameObject *o : world.scene_objects()) {
					if (o->fighter && o->user_controlled) world.set_user_controlled(*o, -1);
				}
			}
			// OPENBG_KO=<frame>[,<frame>...]: every computer fighter loses all its health then (testing
			// the end of a match).
			if (const char *ko = getenv("OPENBG_KO")) {
				for (const char *p = ko; *p;) {
					if (atoi(p) == fr) {
						game::GameObject *user = nullptr;
						for (game::GameObject *o : world.scene_objects()) {
							if (o->fighter && o->user_controlled) user = o;
						}
						for (game::GameObject *o : world.scene_objects()) {
							if (o->fighter) fprintf(stderr, "[ko %d] %s %s at %.0f %.0f %.0f\n", fr, o->name.c_str(), o->user_controlled ? "player" : "computer", o->world().t[0], o->world().t[1], o->world().t[2]);
						}
						for (game::GameObject *o : world.scene_objects()) {
							if (!o->fighter || o->user_controlled) continue;
							for (game::GameObject *ch : o->children) {
								if (!ch->brain || ch->brain->cls.name != "PFighterStats") continue;
								std::vector<uint8_t> args(16, 0);
								uint32_t h = user ? user->handle : 0;
								float dmg = -100000, stun = 0;
								memcpy(&args[0], &h, 4), memcpy(&args[4], &dmg, 4), memcpy(&args[8], &stun, 4);
								ch->brain->call(world, "AdjustHealthAndStun(class CObject,float,float,class CAttackInfo)", args);
							}
						}
					}
					const char *c = strchr(p, ',');
					if (!c) break;
					p = c + 1;
				}
			}
			// OPENBG_TEST_SNAPSHOT=<frame> runs the launcher's SubSessionCaptureSnapshot(null, null) then.
			static const int test_snapshot = getenv("OPENBG_TEST_SNAPSHOT") ? atoi(getenv("OPENBG_TEST_SNAPSHOT")) : -1;
			if (fr == test_snapshot) {
				for (game::Brain *b : world.brains()) {
					std::vector<uint8_t> args(8);
					if (b->call(world, "SubSessionCaptureSnapshot(class CCreature,class CCreature)", args)) {
						fprintf(stderr, "test snapshot on %s\n", b->owner->name.c_str());
						break;
					}
				}
			}
			// The frame's captures (CRenderTargetImage::Capture): the scene from each camera into
			// its image's 256 x 256 target.
			for (auto &[img, cam_handle] : world.capture_requests) {
				game::GameObject *camera = world.object(cam_handle);
				if (!game_shown || !world.object(img) || !camera) continue;
				RenderTarget &t = render_targets[img];
				if (!t.fbo) {
					glGenTextures(1, &t.tex);
					glBindTexture(GL_TEXTURE_2D, t.tex);
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					glGenRenderbuffers(1, &t.depth);
					glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
					glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 256, 256);
					glGenFramebuffers(1, &t.fbo);
					glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
					glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
				}
				glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
				view.capture_camera = camera;
				view.draw(256, 256, fr, true);
				view.capture_camera = nullptr;
				// OPENBG_DEBUG_CAPTURE=<dir> writes each capture there.
				if (const char *dir = getenv("OPENBG_DEBUG_CAPTURE")) {
					std::vector<uint8_t> px(256 * 256 * 4);
					glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
					std::string out = std::string(dir) + "/capture-" + std::to_string(fr) + ".png";
					glu::write_png(out.c_str(), 256, 256, px);
					fprintf(stderr, "capture %s from %s\n", out.c_str(), camera->name.c_str());
				}
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glViewport(0, 0, dw, dh);
			}
			world.capture_requests.clear();
			for (auto it = render_targets.begin(); it != render_targets.end();) {
				if (world.object(it->first)) { ++it; continue; }
				glDeleteFramebuffers(1, &it->second.fbo), glDeleteRenderbuffers(1, &it->second.depth), glDeleteTextures(1, &it->second.tex);
				it = render_targets.erase(it);
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
			std::vector<render::ActorMeshRenderer::Item> items, doors;
			for (game::GameObject *o : world.scene_objects()) {
				if (!o->is_actor || !o->file || o->meshes_disabled || !game::in_scene(*o) || !gui_only(o)) continue;
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
				bool door = false;
				for (const game::GameObject *p = o; p && !door; p = p->parent) door = p == app.transition;
				if (live) (door ? doors : items).push_back({{o->file, o->index}, o->world(), lights.owner_of(o)});
			}
			auto draw_picker = [&] {
				render::Lighting gui_lighting = render::g_lighting;
			// The model picker's scene viewer (DE_CSceneViewerControl3d over the UI area): its own
			// scene through EnvironmentCamera, lit by its own lights, the model on RobotPedistal.
			auto *picker = dynamic_cast<game::ui::ModelPicker *>(app.current.get());
			if (picker && picker->activated && picker->camera) {
				glClear(GL_DEPTH_BUFFER_BIT);
				app::CameraLens lens = app::camera_lens(picker->camera);
				de::Transform ct = picker->camera->world();
				auto f = ct.rotate({0, 0, 1}), up = ct.rotate({0, 1, 0});
				float ceye[3] = {ct.t[0], ct.t[1], ct.t[2]}, pproj[16], pview[16], pvp[16];
				float pvfov = 2.0f * std::atan(std::tan(0.5f * lens.fov * 0.017453292f) * 0.75f) / 0.017453292f;
				glu::perspective(pproj, pvfov, aspect, lens.near, lens.far);
				glu::look_view(pview, ceye, f.data(), up.data());
				glu::mul(pvp, pproj, pview);
				if (auto *lf = world.data().file("LoadScene")) render::g_lighting.ambient = de::read_ambient(world.data(), *lf);
				render::FrameLights plights = render::collect_lights(world, fr, picker_only);
				std::vector<render::ActorMeshRenderer::Item> pitems;
				for (game::GameObject *o : world.scene_objects()) {
					if (!o->is_actor || !o->file || o->meshes_disabled || !game::in_scene(*o) || !picker_only(o)) continue;
					bool live = true;
					for (const game::GameObject *p = o; p && live; p = p->parent) live = p->visible;
					if (live) pitems.push_back({{o->file, o->index}, o->world(), plights.owner_of(o)});
				}
				actors_r.draw(world.data(), pitems, pvp, ceye);
				// NewStrippedFighterModel (OMF 0x35038940): the model description's model; painted by
				// the last selected pilot (FighterTemplate's image filter).
				de::ObjectRef m = picker->model();
				auto res = m ? game::passive_references(world.data(), m) : std::vector<de::ObjectRef>{};
				if (!res.empty() && picker->pedestal) {
					if (m.file != picker_model_shown.file || m.index != picker_model_shown.index) {
						std::string stem = res[0].file->master().name;
						std::string robot = stem.size() > 6 && stem.compare(stem.size() - 6, 6, "Model1") == 0 ? stem.substr(0, stem.size() - 6) : stem;
						picker_model = render::FighterRenderer();
						picker_model.load(world.data(), *res[0].file, world.data().file(robot), res[0].index);
						picker_model_shown = m;
						picker_pilot_shown = {};
					}
					if (picker->pilot.file != picker_pilot_shown.file || picker->pilot.index != picker_pilot_shown.index) {
						auto pres = picker->pilot ? game::passive_references(world.data(), picker->pilot) : std::vector<de::ObjectRef>{};
						if (!pres.empty()) {
							if (auto pl = game::read_pilot(*pres[0].file, pres[0].info(), 0)) picker_model.set_paint(pl->colours);
						}
						picker_pilot_shown = picker->pilot;
					}
					picker_model.draw(pvp, ceye, picker->pedestal->world(), -1, 0);
				}
				particles_r.only = picker_only;
				particles_r.draw(world, pvp, pview, ceye);
				particles_r.only = nullptr;
				render::g_lighting = gui_lighting;
			}
			};
			render::update_animated_textures(world.image_phases);
			actors_r.texture_version = world.texture_override_version;
			actors_r.draw(world.data(), items, vp, eye);
			particles_r.only = gui_only;
			particles_r.draw(world, vp, view, eye);
			particles_r.only = nullptr;
			if (!game_shown) {
				overlay.draw_world(world, vp);
				overlay.draw(world, vpt.w, vpt.h);
			}
			draw_picker();
			// The controls: button labels (DE_CButtonControl3d::SetText: 16 of 600 virtual pixels,
			// centred; white, the colour the screens pass), then a notification over everything.
			const float white[4] = {1, 1, 1, 1};
			if (app.current && app.current->activated) {
				std::vector<game::ui::DrawItem> items;
				game::ui::UIPtr page = app.current;
				if (page->gui) page->gui->paint(items);
				page->draw(app, items);
				if (page->gui) page->gui->paint_over(items);
				for (auto &d : items) {
					// A control's clip: the scissor, in the window's pixels (origin bottom left).
					bool clip = d.clip[2] > d.clip[0];
					if (clip) {
						glEnable(GL_SCISSOR_TEST);
						int sx0 = vpt.x + int(std::floor(d.clip[0] * float(vpt.w))), sx1 = vpt.x + int(std::ceil(d.clip[2] * float(vpt.w)));
						int sy0 = vpt.y + int(std::floor((1 - d.clip[3]) * float(vpt.h))), sy1 = vpt.y + int(std::ceil((1 - d.clip[1]) * float(vpt.h)));
						glScissor(sx0, sy0, std::max(0, sx1 - sx0), std::max(0, sy1 - sy0));
					}
					if (d.kind == game::ui::DrawItem::Quad) overlay.quad4(d.pts, d.cols, vpt.w, vpt.h);
					else if (d.kind == game::ui::DrawItem::Text && d.cell) overlay.label_cell(d.text, d.x0, d.y0, d.size, d.rgba, vpt.w, vpt.h, d.centered);
					else if (d.kind == game::ui::DrawItem::Rect) overlay.rect(d.x0, d.y0, d.x1, d.y1, d.rgba, vpt.w, vpt.h);
					else if (d.kind == game::ui::DrawItem::Image && d.render_target) {
						if (auto it = render_targets.find(d.render_target); it != render_targets.end()) overlay.texture_rect(it->second.tex, d.x0, d.y0, d.x1, d.y1, vpt.w, vpt.h);
					} else if (d.kind == game::ui::DrawItem::Image) overlay.image_rect(d.image, d.x0, d.y0, d.x1, d.y1, vpt.w, vpt.h, d.rgba);
					else overlay.label(d.text, d.x0, d.y0, d.size, d.rgba, vpt.w, vpt.h, d.centered);
					if (clip) glDisable(GL_SCISSOR_TEST);
				}
				for (auto &c : app.current->controls) {
					overlay.label(c.text, (c.left + c.right) / 2, (c.top + c.bottom) / 2, 16.0f / 600.0f, white, vpt.w, vpt.h);
				}
			}
			// The doors close over the page's controls, which are 3D controls behind them in the original.
			actors_r.draw(world.data(), doors, vp, eye);
			render::g_lighting = game_lighting;
			if (!app.notifications.empty()) {
				// DE_CDialog3d::UserInformation, as the original draws it: a light grey bevelled frame,
				// a navy title bar shading darker to the left with the title and a close box, a black
				// body with the user-information icon, the text centred, and OK in a blue frame.
				auto &n = app.notifications.front();
				auto l = app.notification_layout();
				const float frame_hi[4] = {0.86f, 0.86f, 0.85f, 1}, frame_lo[4] = {0.45f, 0.44f, 0.45f, 1}, black[4] = {0, 0, 0, 1};
				overlay.rect(l.x0, l.y0, l.x1, l.y1, frame_lo, vpt.w, vpt.h);
				overlay.rect(l.x0, l.y0, l.x1 - l.frame * 0.4f, l.y1 - l.frame * 0.4f, frame_hi, vpt.w, vpt.h);
				float fx = l.frame * 0.75f;
				overlay.rect(l.x0 + fx, l.y0 + l.frame, l.x1 - fx, l.y1 - l.frame, black, vpt.w, vpt.h);
				const int strips = 32;
				for (int k = 0; k < strips; ++k) {
					float t = (float(k) + 0.5f) / strips;
					const float c[4] = {0, 0, 0.08f + 0.6f * t, 1};
					float a0 = l.x0 + fx + (l.x1 - l.x0 - 2 * fx) * float(k) / strips, a1 = l.x0 + fx + (l.x1 - l.x0 - 2 * fx) * float(k + 1) / strips;
					overlay.rect(a0, l.y0 + l.frame, a1, l.title_bottom, c, vpt.w, vpt.h);
				}
				const float sep[4] = {0.43f, 0.45f, 0.47f, 1};
				overlay.rect(l.x0 + fx, l.title_bottom, l.x1 - fx, l.title_bottom + 0.004f, sep, vpt.w, vpt.h);
				overlay.label(n.title, l.x0 + l.frame + 0.012f, (l.y0 + l.frame + l.title_bottom) / 2, 0.034f, white, vpt.w, vpt.h, false);
				const float close_face[4] = {0.31f, 0.31f, 0.51f, 1};
				overlay.rect(l.close[0], l.close[1], l.close[2], l.close[3], frame_hi, vpt.w, vpt.h);
				overlay.rect(l.close[0] + 0.002f, l.close[1] + 0.003f, l.close[2] - 0.002f, l.close[3] - 0.003f, close_face, vpt.w, vpt.h);
				overlay.label("X", (l.close[0] + l.close[2]) / 2, (l.close[1] + l.close[3]) / 2, 0.04f, black, vpt.w, vpt.h);
				if (const de::ObjectsFile *core = world.data().file("COREImages")) {
					for (uint32_t i = 0; i < core->objects().size(); ++i) {
						if (core->objects()[i].name != "Not_UserInformation") continue;
						overlay.image_rect({core, i}, l.icon[0], l.icon[1], l.icon[2], l.icon[3], vpt.w, vpt.h);
						break;
					}
				}
				for (size_t i = 0; i < l.lines.size(); ++i) {
					overlay.label(l.lines[i], (l.text_left + l.text_right) / 2, l.text_top + l.line * float(i), l.text_size, white, vpt.w, vpt.h);
				}
				const float ok_frame[4] = {0.17f, 0.16f, 0.57f, 1}, ok_face[4] = {0.27f, 0.27f, 0.31f, 1};
				overlay.rect(l.ok[0], l.ok[1], l.ok[2], l.ok[3], ok_frame, vpt.w, vpt.h);
				overlay.rect(l.ok[0] + 0.004f, l.ok[1] + 0.005f, l.ok[2] - 0.004f, l.ok[3] - 0.005f, ok_face, vpt.w, vpt.h);
				overlay.label("OK", (l.ok[0] + l.ok[2]) / 2, (l.ok[1] + l.ok[3]) / 2, 0.03f, white, vpt.w, vpt.h);
			}
			glViewport(0, 0, dw, dh);
			// The VCR's Snapshot (TakeScreenShot): the drawn frame, saved to the Pictures folder.
			if (app.snapshot_requested) {
				app.snapshot_requested = false;
				std::vector<uint8_t> px(size_t(dw) * dh * 4);
				glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
				char name[96];
				time_t now = time(nullptr);
				strftime(name, sizeof name, "OMFBG Snapshot %Y-%m-%d %H-%M-%S.png", localtime(&now));
				const char *home = getenv("HOME");
				std::string out = std::string(home ? home : ".") + "/Pictures/" + name;
				glu::write_png(out.c_str(), dw, dh, px);
				printf("snapshot %s\n", out.c_str());
			}
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
