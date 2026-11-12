// openbg: the game's front end (menus), run by the game's own GUI data.
//
//   openbg <Data dir> [--screenshot out.png --frames N]
//
// The menus are the 3D GUI scene in OMFBGGUI.DEObjects (FINDINGS 125, 126): the persistent V.*
// actors (frame, doors, backdrop, environment) and one F.<Screen> folder per page, its VE.*
// actors the buttons, driven by the UI managers in game/ui.h.
//
// --launch <match> starts that match's session at once (--host <match>: as a joinable session,
// whose Choose Cast lists the Available Arenas, served on the network; --join host[:port] joins one,
// --port the port to host on or join at); --choose Elizabeth,Jaguar makes the UIs
// it launches pick those (testing), and Choose Cast take the modifiers and starting match named
// there ("Turbo Speed", "Intensity") and the defaults for the rest; --headless N runs N frames
// without a window and lists the natives still missing. --start-match N starts a tournament at
// its match N whatever is unlocked (the editor's Play from match).
//
// MOUSE="x,y@frame;down@frame;up@frame;key@frame;esc@frame;char c@frame;wheel n@frame;..." scripts the mouse (and Enter, Esc, a character key, the wheel) in screenshot runs (x, y fractions
// of the window, y down); SHOTS="40,80" also writes <out>-<frame>.png.
#include <SDL.h>
#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "de/contents.h"
#include "de/lighting.h"
#include "de/platform.h"
#include "game/replay.h"
#include "game/runtime.h"
#include "game/perf.h"
#include "game/session.h"
#include "game/descriptions.h"
#include "game/netgame.h"
#include "game/ui.h"
#include "audio.h"
#include "game_view.h"
#include "gl_util.h"
#include "gl_window.h"
#include "overlay.h"
#include "particles_gl.h"
#include "render.h"
#include "scene_lights.h"
#include "vm/disasm.h"

namespace {

// BFighter's m_LastDamageInflictor, a reference member VMC2 leaves unnamed: the one SendDeathEvent reads.
int last_damage_inflictor_member(const vm::ScriptClass &cls) {
	const vm::ScriptMethod *m = cls.find_by_name("SendDeathEvent(bool)");
	if (!m) return -1;
	for (const vm::Instruction &ins : vm::decode(m->cm->code)) {
		if (ins.opcode == 64) return int(ins.args[0]);
	}
	return -1;
}

// The game's registry (HKCU\Software\Diversions Ent\One Must Fall Battlegrounds in the original),
// kept as "name=value" lines in the user's settings folder (de::user_data_dir()).
std::filesystem::path registry_path() { return de::user_data_dir() / "registry.txt"; }
bool load_registry(std::map<std::string, std::string> &reg) {
	std::ifstream f(registry_path(), std::ios::binary);
	if (!f) return false;
	std::string l;
	while (std::getline(f, l)) {
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
		size_t eq = l.find('=');
		if (eq != std::string::npos) reg[l.substr(0, eq)] = l.substr(eq + 1);
	}
	return true;
}
void save_registry(const std::map<std::string, std::string> &reg) {
	std::error_code ec;
	std::filesystem::create_directories(registry_path().parent_path(), ec);
	if (ec) return;
	std::ofstream f(registry_path(), std::ios::binary | std::ios::trunc);
	if (!f) return;
	for (auto &[k, v] : reg) f << k << '=' << v << '\n';
}

// The GUI's viewport: the whole window. InitGUI (2004 build 0x408b20) hangs the UI parent actor,
// scaled (1, 4/3, 1), under the PostSceneView control that fills the window, so the 0..1 x 0..0.75
// GUI scene is stretched to the window's shape, which is the 4:3 view drawn into it. Wider windows
// (a deliberate deviation, PLACEHOLDERS.md) get the centred 4:3 area; the 3D scene widens around it.
struct Viewport {
	int x = 0, y = 0, w = 1, h = 1;
	bool wide = false;
};
Viewport gui_viewport(int dw, int dh) {
	Viewport v;
	v.w = dw, v.h = dh;
	if (int64_t(dw) * 3 > int64_t(dh) * 4) {
		v.wide = true;
		v.w = int(std::lround(double(dh) * 4 / 3));
		v.x = (dw - v.w) / 2;
	}
	return v;
}

// OPENBG_DEBUG_STATE=N: every N frames, the rand stream, the scene's size and the fighters'
// places, to compare runs.
void print_state(game::World &world, int fr) {
	uint32_t n = 0;
	uint64_t h = 1469598103934665603ull;
	std::string fighters;
	for (game::GameObject *o : world.scene_objects()) {
		++n;
		de::Transform t = o->world();
		for (float f : t.t) {
			uint32_t b;
			memcpy(&b, &f, 4);
			h = (h ^ b) * 1099511628211ull;
		}
		if (o->fighter) {
			char buf[128];
			snprintf(buf, sizeof buf, " %s %.3f %.3f %.3f", o->name.c_str(), t.t[0], t.t[1], t.t[2]);
			fighters += buf;
		}
	}
	fprintf(stderr, "[state %d] rand %08x scene %u %016llx%s\n", fr, game::crt_rand_state(), n, (unsigned long long)h, fighters.c_str());
}

}  // namespace

int main(int argc, char **argv) {
	de::init_platform();
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (argc < 2) {
		fprintf(stderr, "usage: %s <Data dir> [--addon file.DEObjects] [--screenshot out.png --frames N]\n", argv[0]);
		return 2;
	}
	std::string data = argv[1];
	const char *shot = nullptr;
	std::string launch;
	int start_match = -1;
	bool host = false;  // --host: --launch as a joinable session, as BMatchServer sets one up (testing)
	std::string replay_path;  // --replay: play this recording at once (testing)
	std::vector<std::string> choose;  // --choose: names launched UIs pick at once (testing)
	std::vector<std::string> highlight;  // --highlight: a name the choosers highlight first (testing)
	bool headless = false;
	int frames = 60;
	std::string join;  // --join: the server to join at once
	int port = 0;      // --port
	int shot_w = 1024, shot_h = 768;  // --size WxH: a screenshot run's frame
	for (int i = 2; i < argc; ++i) {
		if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &shot_w, &shot_h);
		else if (!strcmp(argv[i], "--launch") && i + 1 < argc) launch = argv[++i];
		else if (!strcmp(argv[i], "--start-match") && i + 1 < argc) start_match = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--host") && i + 1 < argc) launch = argv[++i], host = true;
		else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay_path = argv[++i];
		else if (!strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
		else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "simulatefullscreen") || !strcmp(argv[i], "-simulatefullscreen")) continue;  // the launcher's switch
		else if (!strcmp(argv[i], "--highlight") && i + 1 < argc) highlight.push_back(argv[++i]);
		else if (!strcmp(argv[i], "--choose") && i + 1 < argc) {
			std::string l = argv[++i];
			for (size_t p = 0; p <= l.size();) {
				size_t q = l.find(',', p);
				choose.push_back(l.substr(p, q == std::string::npos ? std::string::npos : q - p));
				if (q == std::string::npos) break;
				p = q + 1;
			}
		}
		else if (!strcmp(argv[i], "--addon") && i + 1 < argc) {
			// An extra add-on .DEObjects, loaded as if it were in the Data folder (OPENBG_ADDONS).
			std::string list = getenv("OPENBG_ADDONS") ? getenv("OPENBG_ADDONS") : "";
#ifdef _WIN32
			list += (list.empty() ? "" : ";") + std::string(argv[++i]);
			_putenv_s("OPENBG_ADDONS", list.c_str());
#else
			list += (list.empty() ? "" : ":") + std::string(argv[++i]);
			setenv("OPENBG_ADDONS", list.c_str(), 1);
#endif
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
	// HOLD="left_punch@300-420,up@500-510": the user's buttons held over those frames, as in openbg-fight.
	std::vector<std::tuple<bool game::Input::*, int, int>> holds;
	if (const char *h = getenv("HOLD")) {
		const std::map<std::string, bool game::Input::*> keys = {
		    {"up", &game::Input::up}, {"down", &game::Input::down}, {"left", &game::Input::left},
		    {"right", &game::Input::right}, {"jump", &game::Input::jump}, {"duck", &game::Input::duck},
		    {"evade", &game::Input::evade}, {"left_punch", &game::Input::left_punch},
		    {"right_punch", &game::Input::right_punch}, {"left_kick", &game::Input::left_kick},
		    {"right_kick", &game::Input::right_kick}};
		std::string spec = h;
		for (size_t p = 0; p < spec.size();) {
			size_t q = spec.find(',', p);
			std::string item = spec.substr(p, q == std::string::npos ? std::string::npos : q - p);
			int a = 0, b = 0;
			char key[32] = {};
			if (sscanf(item.c_str(), "%31[a-z_]@%d-%d", key, &a, &b) == 3 && keys.count(key)) holds.emplace_back(keys.at(key), a, b);
			if (q == std::string::npos) break;
			p = q + 1;
		}
	}
	auto held = [&holds](game::Input &in, int fr) {
		for (auto &[f, a, b] : holds) {
			if (fr >= a && fr <= b) in.*f = true;
		}
	};
	try {
		fprintf(stderr, "Loading game data from %s (this takes a while)...\n", data.c_str());
		game::World world(data);
		// DE_GLOBAL::SystemInit: srand(time(NULL)), and SystemTime is the wall clock. Screenshot and
		// headless runs keep the CRT's start value (1) and 20 ms a frame so they repeat; OPENBG_SEED
		// picks a seed.
		if (const char *seed = getenv("OPENBG_SEED")) game::crt_srand(uint32_t(strtoul(seed, nullptr, 0)));
		else if (!shot && !headless) game::crt_srand(uint32_t(time(nullptr)));
		if (!shot && !headless) {
			const auto t0 = std::chrono::steady_clock::now();
			world.system_clock = [t0] { return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()); };
		}
		game::ui::AppManager app(world);
		app.cast_presets = choose;
		app.start_match = start_match;
		game::ui::system_time = [&world] { return world.system_ms(); };
		world.ui_manager_active = [&app] { return app.current && app.current->activated; };
		world.ui_active_locally = [&app](const game::GameObject *ui) { return app.ui_active(app.page_of(ui)); };
		world.notify_user = [&app](const std::string &title, const std::string &text) { app.notify(title, text); };
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
		// OPENBG_ALL_AI=1: every fighter, the user's too, is its pilot's AI's (as openbg-net's), for whole network matches.
		world.all_ai = getenv("OPENBG_ALL_AI") != nullptr;
		auto saved_registry = world.registry, traced_registry = world.registry;
		// Network play (docs/NETWORK_PLAN.md): hosting a joinable session, or joining one.
		game::NetGame netgame(world);
		netgame.notify = [&app](const std::string &title, const std::string &text) {
			fprintf(stderr, "net: %s: %s\n", title.c_str(), text.c_str());
			app.notify(title, text);
		};
		// OPENBG_JOIN_RETRY=1 (test runs): --join tries again until the server first accepts it.
		bool joined_once = false;
		const bool join_retry = getenv("OPENBG_JOIN_RETRY") != nullptr;
		netgame.on_joined = [&app, &joined_once] {
			joined_once = true;
			app.close_stack();
		};
		netgame.on_closed = [&world, &app] {
			game::destroy_all_sessions(world);
			app.close_stack();
			app.set_next(app.main_menu(), true);
		};
		// No session outlives the screens: back at the main menu every one ends, and the network game with it.
		app.leave_sessions = [&] {
			netgame.close();
			if (!world.sessions.empty()) game::destroy_all_sessions(world);
		};
		auto net_options = [&] {
			game::NetOptions o = game::net_options(world.registry);
			if (port > 0 && port < 65536) o.port = uint16_t(port);
			return o;
		};
		app.join_server = [&](const std::string &address, const std::string &password, std::string *error) {
			game::NetOptions o = net_options();
			o.password = password;
			netgame.close();
			return netgame.join(address, o, error);
		};
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
		app.launch_session = [&](const de::ObjectRef &desc, bool single_player) -> game::SessionState * {
			auto res = game::passive_references(world.data(), desc);
			if (res.empty()) {
				app.notify(desc.info().name, "This match has no session to start.");
				return nullptr;
			}
			printf("launching %s (%s)%s\n", desc.info().name.c_str(), res[0].info().name.c_str(), single_player ? ", single player" : "");
			auto &ss = game::start_session(world, {res[0]});
			ss.master_description = world.global_object(desc);
			ss.single_player = single_player;
			// DE_CSession::SetAsServer (Core 0x701f8110): a joinable session takes Network\Max Clients, others 20;
			// DE_CNetworkServiceFactory::PostInit (0x701ab300) assures it as 16 when unset (ours also when not positive).
			int max_clients = atoi(app.registry_string("Network\\Max Clients").c_str());
			ss.max_player_clients = single_player ? 20 : max_clients > 0 ? max_clients : 16;
			game::SessionState *sp = &ss;
			auto begin = [&world, &app, sp, single_player] {
				for (auto &s : world.sessions) {
					if (s.get() == sp) world.session = s;
				}
				if (world.session.get() != sp) return;
				// DE_CClient::SetAsUserControlled (Core 0x701f4c70): online, a set Network\Player Name renames it.
				const std::string player = single_player ? std::string() : app.registry_string("Network\\Player Name");
				game::add_client(world, true, player.empty() ? "User" : player);
				game::begin_session(world);
				app.set_registry_bool("SinglePlayerPlayed", true);
			};
			if (!single_player) {
				std::string error;
				if (netgame.state() != game::NetGame::State::Idle) netgame.close();
				if (!netgame.host(ss, net_options(), &error)) {
					app.notify("WinSock", error);
					game::end_session(world, ss);
					return nullptr;
				}
			}
			if (app.satisfy_master_requirements(ss, begin)) begin();
			return sp;
		};
		// A UI manager the session scripts launch (FINDINGS 127): a description selector over the
		// script's pool; Next completes it for the script.
		world.satisfy_master_requirements = [&](game::SessionState &s) { app.satisfy_master_requirements(s, nullptr, true); };
		world.launch_ui = [&](game::GameObject *ui) {
			game::SessionState *owner = game::session_of_ui(world, ui);
			auto &u = (owner ? owner : world.session.get())->ui[ui];
			auto sel = std::make_shared<game::ui::DescriptionSelector>(u.description);
			sel->title = u.description;
			sel->autoselect_key = u.autoselect;
			sel->choose_fighter = u.choose_fighter;
			sel->highlight = highlight;
			sel->script = ui;
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
			if (!u.activate_hooks.empty()) sel->on_begin = [&world, ui](game::ui::AppManager &) { game::ui_manager_began(world, ui); };
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
			if (!page || page->deleted) page = app.rules_display(rules);
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
		// IsDisplayedCaller: a rules display's page has activated and no page transition is busy. Until
		// then BRulesDisplay keeps calling AssureRulesDisplay, whose CloseUIManagerStack drops the page
		// its Begin pushed (Choose Creature, which autopauses) once the rules display is current.
		auto rules_displayed = [&] {
			bool busy = false, asked = false;
			for (auto &[rules, page] : rules_pages) {
				bool up = page && page == app.current && page->activated;
				if (up && !asked) busy = app.transition_busy(), asked = true;
				world.rules_displays[rules].displayed = up && !busy;
			}
		};
		world.show_vcr = [&](game::GameObject *) { app.set_next(app.vcr_controls(), true); };
		app.launch_replay = [&](const std::string &path) { return game::start_replay(world, game::load_recording(path)); };
		// AddIfAbsentLocalSession comes with the session's Begin: one Choose Cast made is not local.
		app.has_local_session = [&] { return world.session != nullptr && world.session->begun; };
		// IsFullySequenced (Core 0x70010e10) of the newest local session: Sequenced (status 1000).
		app.session_fully_sequenced = [&] {
			for (auto it = world.sessions.rbegin(); it != world.sessions.rend(); ++it) {
				if (!(*it)->owner && (*it)->begun) return (*it)->net_status == 1000;
			}
			return true;
		};
		app.session_scene_rendering = [&] { return world.session != nullptr && view.has_arena(); };
		if (!launch.empty()) {
			de::ObjectRef d = world.data().find("Description\\SessionMaster", launch);
			if (!d) {
				fprintf(stderr, "no match named %s\n", launch.c_str());
				return 1;
			}
			app.launch_session(d, !host);
		} else if (!join.empty()) {
			std::string error;
			if (!netgame.join(join, net_options(), &error)) {
				fprintf(stderr, "cannot join %s: %s\n", join.c_str(), error.c_str());
				return 1;
			}
		} else if (!replay_path.empty()) {
			if (!game::start_replay(world, game::load_recording(replay_path))) {
				fprintf(stderr, "cannot replay %s\n", replay_path.c_str());
				return 1;
			}
		} else {
			// DE_CGameLauncherWindow::ObjInitContents: the main menu is the first UI manager.
			app.set_next(app.main_menu(), true);
		}
		// The testing switches that act on the world at a frame (screenshot and headless runs).
		auto test_switches = [&](int fr) {
			// OPENBG_NET_TEST="tab@F;vote@F:<player>:kick|stay|none;admin@F:<password>": at frame F the
			// Client Browser key, this machine's kick vote on a player, the Admin button's password.
			if (const char *nt = getenv("OPENBG_NET_TEST")) {
				std::string spec = nt;
				for (size_t p = 0; p < spec.size();) {
					size_t q = spec.find(';', p);
					std::string item = spec.substr(p, q == std::string::npos ? std::string::npos : q - p);
					size_t at = item.find('@'), colon = item.find(':');
					std::string what = item.substr(0, at), arg = colon == std::string::npos ? "" : item.substr(colon + 1);
					if (at != std::string::npos && atoi(item.c_str() + at + 1) == fr) {
						if (what == "tab") app.client_browser_impulse();
						if (what == "admin") app.admin_password_entered(true, arg);
						if (what == "vote") {
							size_t c2 = arg.find(':');
							std::string who = arg.substr(0, c2), how = c2 == std::string::npos ? "" : arg.substr(c2 + 1);
							if (game::NetSession *n = netgame.session(); n && n->session) {
								for (auto &cl : n->session->clients) {
									if (cl->player_name == who) cl->my_vote = how == "kick" ? -1 : how == "stay" ? 1 : 0;
								}
							}
						}
					}
					if (q == std::string::npos) break;
					p = q + 1;
				}
			}
			// OPENBG_AUTOPLAY=<frame>: the player's fighter is handed to the AI then (testing whole matches).
			if (const char *ap = getenv("OPENBG_AUTOPLAY"); ap && fr >= atoi(ap)) {
				for (game::GameObject *o : world.scene_objects()) {
					if (o->fighter && o->user_controlled) world.set_user_controlled(*o, -1), o->autoplayed = true;
				}
			}
			// OPENBG_KO=<frame>[,<frame>...]: every computer fighter loses all its health then, the player
			// named as the attacker (testing the end of a match). OPENBG_KO=auto[:N] does it N frames
			// (default 150) after each start of the action (the session master's
			// StandardSessionIsActionStarted).
			if (const char *ko = getenv("OPENBG_KO")) {
				auto knock_out = [&] {
					game::GameObject *user = nullptr;
					for (game::GameObject *o : world.scene_objects()) {
						if (o->fighter && (o->user_controlled || o->autoplayed)) user = o;
					}
					for (game::GameObject *o : world.scene_objects()) {
						if (o->fighter) fprintf(stderr, "[ko %d] %s %s at %.0f %.0f %.0f\n", fr, o->name.c_str(), o == user ? "player" : "computer", o->world().t[0], o->world().t[1], o->world().t[2]);
					}
					for (game::GameObject *o : world.scene_objects()) {
						if (!o->fighter || o == user) continue;
						for (game::GameObject *ch : o->children) {
							if (!ch->brain || ch->brain->cls.name != "PFighterStats") continue;
							std::vector<uint8_t> args(16, 0);
							uint32_t h = user ? user->handle : 0;
							float dmg = -100000, stun = 0;
							memcpy(&args[0], &h, 4), memcpy(&args[4], &dmg, 4), memcpy(&args[8], &stun, 4);
							ch->brain->call(world, "AdjustHealthAndStun(class CObject,float,float,class CAttackInfo)", args);
						}
						// BFighter::ReceiveDamage credits a hit owned by a fighter after the stats.
						if (int k = user && o->brain ? last_damage_inflictor_member(o->brain->cls) : -1; k >= 0) {
							auto &refs = o->brain->inst.ref_members;
							if (refs.size() <= size_t(k)) refs.resize(size_t(k) + 1, 0);
							refs[size_t(k)] = user->handle;
						}
					}
				};
				if (!strncmp(ko, "auto", 4)) {
					static int started = -1;
					const int delay = ko[4] == ':' ? atoi(ko + 5) : 150;
					bool action = false;
					for (auto &s : world.sessions) {
						if (s->ended || s->modifiers.empty() || !s->modifiers[0]->brain) continue;
						std::vector<uint8_t> ret(4, 0);
						if (s->modifiers[0]->brain->call(world, "bool StandardSessionIsActionStarted()", ret) && ret[0]) action = true;
					}
					if (!action) started = -1;
					else if (started < 0) started = fr;
					else if (fr - started == delay) knock_out();
				}
				for (const char *p = ko; *p && isdigit((unsigned char)*p);) {
					if (atoi(p) == fr) knock_out();
					const char *c = strchr(p, ',');
					if (!c) break;
					p = c + 1;
				}
			}
			// OPENBG_HELP_CYCLE="<frame>,<every>": from that frame the open description selector shows its entries in turn.
			if (const char *hc = getenv("OPENBG_HELP_CYCLE")) {
				int start = 0, every = 1;
				if (sscanf(hc, "%d,%d", &start, &every) == 2 && every > 0 && fr >= start && (fr - start) % every == 0) {
					if (auto *sel = dynamic_cast<game::ui::DescriptionSelector *>(app.current.get())) {
						std::string n = sel->show_entry(app, size_t((fr - start) / every));
						if (!n.empty()) fprintf(stderr, "[help %d] entry %d: %s\n", fr, (fr - start) / every, n.c_str());
					}
				}
			}
			// OPENBG_QUIT_SESSION=<frame>: the in-game menu's Main Menu then (testing recordings).
			static const int quit_session = getenv("OPENBG_QUIT_SESSION") ? atoi(getenv("OPENBG_QUIT_SESSION")) : -1;
			if (fr == quit_session) game::destroy_all_sessions(world);
			// OPENBG_CHANGE_FIGHTER=<frame>: the in-game menu's Change Fighter then (network tests).
			static const int change_fighter = getenv("OPENBG_CHANGE_FIGHTER") ? atoi(getenv("OPENBG_CHANGE_FIGHTER")) : -1;
			if (fr == change_fighter) game::net_change_fighter(world);
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
			// OPENBG_PLACE="Jaguar:0,90,1035@900;...": puts a fighter somewhere at a frame, as openbg-fight does.
			if (const char *pl = getenv("OPENBG_PLACE")) {
				std::string spec = pl;
				for (size_t p = 0; p < spec.size();) {
					size_t q = spec.find(';', p);
					std::string item = spec.substr(p, q == std::string::npos ? std::string::npos : q - p);
					char who[32] = {};
					float x = 0, y = 0, z = 0;
					int at = 0;
					if (sscanf(item.c_str(), "%31[^:]:%f,%f,%f@%d", who, &x, &y, &z, &at) == 5 && at == fr) {
						for (game::GameObject *o : world.scene_objects()) {
							if (o->fighter && o->name == who) o->local.t = {x, y, z}, o->momentum = {0, 0, 0};
						}
					}
					if (q == std::string::npos) break;
					p = q + 1;
				}
			}
			static const int memory_every = getenv("OPENBG_DEBUG_MEMORY") ? std::max(1, atoi(getenv("OPENBG_DEBUG_MEMORY"))) : 0;
			if (memory_every && fr % memory_every == 0) fprintf(stderr, "[memory %d] script arena %u MB, %s\n", fr, world.memory().used() >> 20, world.debug_counts().c_str());
			// OPENBG_DEBUG_TREE=<name>@<frame> prints the scene branches under objects of that name then.
			if (const char *dt = getenv("OPENBG_DEBUG_TREE"); dt && strchr(dt, '@') && atoi(strchr(dt, '@') + 1) == fr) {
				std::string name(dt, size_t(strchr(dt, '@') - dt));
				std::function<void(game::GameObject *, int)> dump = [&](game::GameObject *o, int depth) {
					auto t = o->world().t;
					fprintf(stderr, "[tree %d] %*s%s [%s] active %d visible %d in scene %d at %.1f %.1f %.1f\n", fr, depth * 2, "", o->name.c_str(), o->class_path.c_str(), o->active, o->visible, game::in_scene(*o), t[0], t[1], t[2]);
					if (depth < 6) for (game::GameObject *ch : o->children) dump(ch, depth + 1);
				};
				std::function<void(game::GameObject *)> find = [&](game::GameObject *o) {
					if (o->name == name) dump(o, 0);
					else for (game::GameObject *ch : o->children) find(ch);
				};
				for (game::GameObject *r : world.scene_roots) find(r);
			}
			// OPENBG_DEBUG_REGISTRY=1 prints each registry value as it changes (testing unlocks).
			static const bool debug_registry = getenv("OPENBG_DEBUG_REGISTRY") != nullptr;
			if (debug_registry && world.registry != traced_registry) {
				for (auto &[k, v] : world.registry) {
					auto it = traced_registry.find(k);
					if (it == traced_registry.end() || it->second != v) fprintf(stderr, "[registry %u] %s=%s\n", world.frame(), k.c_str(), v.c_str());
				}
				traced_registry = world.registry;
			}
		};
		auto net_trace = [&](int fr) {
			// OPENBG_NET_TRACE=<file>: each frame the session scene's time, the wall clock and every slot's
			// fighter (place, health, state), for tools/net-lan/compare.py to set two machines side by side.
			static FILE *trace = getenv("OPENBG_NET_TRACE") ? fopen(getenv("OPENBG_NET_TRACE"), "w") : nullptr;
			if (trace && world.session && world.session->begun) {
				game::SessionState *ss = world.session.get();
				const long long wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
				fprintf(trace, "%d %u %d %lld", fr, world.scene_ms_of(ss->obj), ss->net_status, wall);
				for (auto &sl : ss->slots) {
					game::GameObject *f = sl->creature;
					if (!f || !f->spawned) continue;
					float h = -1;
					for (game::GameObject *ch : f->children) {
						std::vector<uint8_t> args(4);
						if (ch->brain && ch->brain->cls.name == "PFighterStats" && ch->brain->call(world, "float NormalizedHealth()", args)) memcpy(&h, args.data(), 4);
					}
					auto *m = f->brain && f->brain->exec ? f->brain->exec->current_state() : nullptr;
					std::string st = m ? m->cm->signature : "-";
					if (size_t c = st.rfind(':'); c != std::string::npos) st = st.substr(c + 1);
					for (char &ch : st) ch = ch == ' ' ? '_' : ch;
					fprintf(trace, " | %d %s %.2f %.2f %.2f %.3f %s %u", sl->index, f->name.c_str(), f->local.t[0], f->local.t[1], f->local.t[2], h, st.c_str(),
					        f->creation_time + uint32_t(f->creature_ticks * game::World::kTickMs));
				}
				// " # ", then the scene's other creatures: name, place, 1 linked / 0 unlinked / - not replicated.
				fputs(" #", trace);
				std::map<const game::GameObject *, uint32_t> ids;
				for (auto &m : game::net_managers(world)) ids[m.owner] = m.server_id;
				for (game::GameObject *o : world.scene_objects()) {
					if (o->fighter || !o->is("Actor\\Creature") || !o->active) continue;
					bool part = false;
					for (game::GameObject *p = o->parent; p && !part && world.object(p->handle) == p; p = p->parent) part = p->fighter;
					if (part) continue;
					auto it = ids.find(o);
					if (it == ids.end() && !o->is("Actor\\Creature\\OMF2Projectile")) continue;
					std::string n = o->name;
					for (char &ch : n) ch = ch == ' ' ? '_' : ch;
					auto t = o->world().t;
					fprintf(trace, " %s:%.0f,%.0f,%.0f:%s", n.c_str(), t[0], t[1], t[2], it == ids.end() ? "-" : it->second ? "1" : "0");
				}
				fputc('\n', trace);
			}
		};
		if (headless) {
			// OPENBG_RECORD_AUDIO=<file>: what is heard, mixed offline (raw f32 stereo, 44100 Hz).
			auto offline = std::make_unique<audio::Mixer>();
			const bool recording = getenv("OPENBG_RECORD_AUDIO") != nullptr;
			if (recording) offline->open_offline(44100);
			double audio_carry = 0;
			de::Transform ear;
			ear.t = {0.5f, 0.375f, 0.866f};
			// A network game runs at the wall clock's pace, as the other machine does.
			const bool paced = netgame.state() != game::NetGame::State::Idle;
			const auto t0 = std::chrono::steady_clock::now();
			if (paced) world.system_clock = [t0] { return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()); };
			uint32_t last_ms = 0;
			for (int fr = 0; fr < frames; ++fr) {
				uint32_t step = game::World::kFrameMs;
				if (paced) {
					uint32_t now = world.system_ms();
					while (now - last_ms < game::World::kFrameMs) {
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
						now = world.system_ms();
					}
					step = std::min<uint32_t>(now - last_ms, 2000);
					last_ms = now;
				}
				for (auto &m : mouse_script) {
					if (m.frame == fr && m.kind == 3) app.key(13);
					if (m.frame == fr && m.kind == 4) app.key(27);
					if (m.frame == fr && m.kind == 5) app.key(int(m.x)), app.text(std::string(1, char(m.x)));
				}
				world.input = {};
				if (!(app.current && app.current->input_off)) held(world.input, fr);
				app.update();
				rules_displayed();
				world.tick_gui(0, game::World::kFrameMs);
				if (dynamic_cast<game::ui::ModelPicker *>(app.current.get())) world.tick_gui(1, game::World::kFrameMs);
				world.autopaused = app.autopaused();
				{ PERF_ZONE("frame.world_tick"); world.tick(netgame.frame_ms(step)); }
				netgame.update();
				if (perf::enabled() && fr > 0 && fr % 600 == 0) {
					static auto perf_t0 = std::chrono::steady_clock::now();
					double perf_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - perf_t0).count();
					perf_t0 = std::chrono::steady_clock::now();
					fprintf(stderr, "[counts %d] script arena %u MB, %s\n", fr, world.memory().used() >> 20, world.debug_counts().c_str());
					fprintf(stderr, "[hist %d] %s\n", fr, world.debug_histogram().c_str());
					perf::report(fr, 600, perf_ms);
				}
				if (join_retry && !join.empty() && !joined_once && netgame.state() == game::NetGame::State::Closed && fr % 50 == 0) {
					std::string error;
					netgame.close();
					netgame.join(join, net_options(), &error);
				}
				test_switches(fr);
				net_trace(fr);
				offline->play_requests(world, view.player ? view.ear() : ear);
				if (recording) {
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
			if (game::NetSession *n = netgame.session()) {
				auto &c = game::net_counters(world);
				const double secs = std::max(1.0, frames * game::World::kFrameMs / 1000.0);
				printf("net summary: received %llu inserted %llu same %llu adjusted %llu rejected %llu held %llu forced %llu corrections %llu rollbacks %llu (%llu ticks) registered %llu, sent %llu states in %llu packets, %.0f B/s, clock jumps %llu, ID requests %llu, deletes %llu, rosters %llu\n",
				       (unsigned long long)c.received, (unsigned long long)c.inserted, (unsigned long long)c.same, (unsigned long long)c.adjusted, (unsigned long long)c.rejected,
				       (unsigned long long)c.held, (unsigned long long)c.held_forced, (unsigned long long)c.corrections_in, (unsigned long long)c.rollbacks,
				       (unsigned long long)c.rollback_ticks, (unsigned long long)c.registered, (unsigned long long)n->states_sent, (unsigned long long)n->packets_sent,
				       n->bytes_sent() / secs, (unsigned long long)n->clock_jumps, (unsigned long long)n->id_requests, (unsigned long long)n->deletes, (unsigned long long)n->rosters);
				for (auto &p : n->peers()) printf("net summary: peer ping %.0f ms, lag %.0f ms, status %d\n", p.ping, n->lag_ms, p.status);
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
		const int W = shot ? std::max(320, shot_w) : std::max(320, app.render_started.res_x), H = shot ? std::max(240, shot_h) : std::max(240, app.render_started.res_y);
		bool fullscreen = !shot && app.registry_bool("Preferences\\Renderer\\Full Screen");
		SDL_GLContext ctx = nullptr;
		SDL_Window *win = glu::create_gl_window("One Must Fall: Battlegrounds", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
		                                        SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE |
		                                            (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) | (shot ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN),
		                                        &ctx);
		if (!glu::load_gl()) throw std::runtime_error("OpenGL 4.1 is required; update your graphics driver");
		app.key_name = [](int sc) { return std::string(SDL_GetScancodeName(SDL_Scancode(sc))); };
		SDL_StartTextInput();
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
		uint32_t picker_robot_shown = 0;
		Viewport vpt = gui_viewport(W, H);
		app.text_width = [&](const std::string &t, float size) { return overlay.text_width(t, size, vpt.w, vpt.h); };
		app.char_offsets = [&](const std::string &t, float size) { return overlay.char_offsets(t, size, vpt.w, vpt.h); };
		app.view_w = vpt.w, app.view_h = vpt.h;

		// The UI parent frame: local x 0..1 across the width and y 0..0.75 up, one unit from the
		// 60-degree frame-control camera, which looks down -Z (DE_CTopControl3d::AdjustRootActor,
		// AssureRenderObjects; FINDINGS 126), drawn 4:3 and stretched to the window (gui_viewport).
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
			// A network screenshot run keeps the wall clock's pace, as the other machine does.
			if (shot && netgame.state() == game::NetGame::State::Idle && !(app.current && app.current->wall_clock)) {
				frame_ms = game::World::kFrameMs;
				accum = 0;
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
			if (shot && !(app.current && app.current->input_off)) held(world.input, fr);
			app.update();
			rules_displayed();
			PERF_ZONE("frame.all_cpu");
			world.tick_gui(0, frame_ms);
			if (auto *picker = dynamic_cast<game::ui::ModelPicker *>(app.current.get())) {
				world.tick_gui(1, picker->scene_paused ? 0 : frame_ms);
				picker->scene_paused = false;
			}
			world.autopaused = app.autopaused();
			{ PERF_ZONE("frame.world_tick"); world.tick(netgame.frame_ms(frame_ms)); }
			netgame.update();
			net_trace(fr);
			if (join_retry && !join.empty() && !joined_once && netgame.state() == game::NetGame::State::Closed && fr % 50 == 0) {
				std::string error;
				netgame.close();
				netgame.join(join, net_options(), &error);
			}
			++fr;
			static const int state_every = getenv("OPENBG_DEBUG_STATE") ? std::max(1, atoi(getenv("OPENBG_DEBUG_STATE"))) : 0;
			if (state_every && fr % state_every == 0) print_state(world, fr);
			mixer.play_requests(world, view.player ? view.ear() : listener);
			mixer.update_music(world);
			{
				Uint64 t = SDL_GetPerformanceCounter();
				float ms = shot ? float(game::World::kFrameMs) : float(double(t - last_render) * 1000.0 / double(SDL_GetPerformanceFrequency()));
				last_render = t;
				PERF_ZONE("frame.render_tick");
				game::render_tick(world, std::fmin(ms, 100.0f));
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
				{ PERF_ZONE("frame.view_draw"); view.draw(dw, dh, fr, shot != nullptr); }
				game_shown = view.has_arena();
			}
			test_switches(fr);
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
			if (vpt.wide) glViewport(0, 0, dw, dh);
			else glViewport(vpt.x, vpt.y, vpt.w, vpt.h);
			glEnable(GL_DEPTH_TEST);
			float vfov = 2.0f * std::atan(std::tan(hfov / 2) / aspect) / 0.017453292f;
			float eye[3] = {listener.t[0], listener.t[1], listener.t[2]}, fwd[3] = {0, 0, -1}, up[3] = {0, 1, 0};
			float proj[16], view[16], vp[16];
			glu::perspective(proj, vfov, vpt.wide ? float(dw) / float(dh) : aspect, 0.01f, 100.0f);
			glu::look_view(view, eye, fwd, up);
			glu::mul(vp, proj, view);
			render::Lighting game_lighting = render::g_lighting;
			if (auto *gf = world.data().file(app.transfer_agent)) render::g_lighting.ambient = de::read_ambient(world.data(), *gf);
			render::FrameLights lights = render::collect_lights(world, fr, gui_only, 1);
			render::g_reflections_enabled = true;  // the GUI's scene never autopauses: ApplyOptions forces it
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
				if (live) (door ? doors : items).push_back({{o->file, o->index}, o->world(), lights.owner_of(o), app::actor_values(world, o), {}, o});
			}
			auto draw_picker = [&] {
				render::Lighting gui_lighting = render::g_lighting;
			// The model picker's scene viewer (DE_CSceneViewerControl3d over the UI area): its own
			// scene through EnvironmentCamera, lit by its own lights, the model on RobotPedistal.
			auto *picker = dynamic_cast<game::ui::ModelPicker *>(app.current.get());
			if (picker && picker->activated && picker->camera) {
				glClear(GL_DEPTH_BUFFER_BIT);
				if (vpt.wide) glViewport(vpt.x, vpt.y, vpt.w, vpt.h);
				app::CameraLens lens = app::camera_lens(picker->camera);
				de::Transform ct = picker->camera->world();
				auto f = ct.rotate({0, 0, 1}), up = ct.rotate({0, 1, 0});
				float ceye[3] = {ct.t[0], ct.t[1], ct.t[2]}, pproj[16], pview[16], pvp[16];
				float pvfov = 2.0f * std::atan(std::tan(0.5f * lens.fov * 0.017453292f) * 0.75f) / 0.017453292f;
				glu::perspective(pproj, pvfov, aspect, lens.near, lens.far);
				glu::look_view(pview, ceye, f.data(), up.data());
				glu::mul(pvp, pproj, pview);
				if (auto *lf = world.data().file("LoadScene")) render::g_lighting.ambient = de::read_ambient(world.data(), *lf);
				render::FrameLights plights = render::collect_lights(world, fr, picker_only, 2);
				// Its scene autopauses (+0x1c0 cleared), so unlike the GUI's it is not lit whatever the option.
				if (!world.lighting_enabled) render::g_lighting.lights.clear();
				render::g_reflections_enabled = world.reflections_enabled;
				std::vector<render::ActorMeshRenderer::Item> pitems;
				for (game::GameObject *o : world.scene_objects()) {
					if (!o->is_actor || !o->file || o->meshes_disabled || !game::in_scene(*o) || !picker_only(o)) continue;
					bool live = true;
					for (const game::GameObject *p = o; p && live; p = p->parent) live = p->visible;
					if (live) pitems.push_back({{o->file, o->index}, o->world(), plights.owner_of(o), app::actor_values(world, o), {}, o});
				}
				actors_r.draw(world.data(), pitems, pvp, ceye);
				// The robot creature: its stripped model, undeformed, painted by its pilot through the
				// template's image filter. Each UpdateModel makes a new robot whose images decode anew.
				const game::GameObject *fm = game::ui::ModelPicker::robot_model(picker->robot);
				if (fm && fm->file) {
					if (picker->robot->handle != picker_robot_shown) {
						std::string stem = fm->file->master().name;
						std::string robot = stem.size() > 6 && stem.compare(stem.size() - 6, 6, "Model1") == 0 ? stem.substr(0, stem.size() - 6) : stem;
						picker_model = render::FighterRenderer();
						picker_model.load(world.data(), *fm->file, world.data().file(robot), fm->index);
						if (const game::GameObject *p = game::ui::ModelPicker::robot_pilot(picker->robot)) picker_model.set_paint(p->pilot->colours);
						picker_robot_shown = picker->robot->handle;
					}
					picker_model.draw(pvp, ceye, picker->robot->world(), -1, 0);
				}
				particles_r.only = picker_only;
				particles_r.draw(world, pvp, pview, ceye);
				particles_r.only = nullptr;
				render::g_lighting = gui_lighting;
				if (vpt.wide) glViewport(0, 0, dw, dh);
			}
			};
			render::update_animated_textures(world.image_phases);
			actors_r.texture_version = world.texture_override_version;
			// The scene viewer is a control under the page's own actors (VE.LoadFrame's console).
			draw_picker();
			glClear(GL_DEPTH_BUFFER_BIT);
			actors_r.draw(world.data(), items, vp, eye);
			particles_r.only = gui_only;
			particles_r.draw(world, vp, view, eye);
			particles_r.only = nullptr;
			if (!game_shown) {
				overlay.draw_world(world, vp);
				overlay.draw(world, vpt.wide ? dw : vpt.w, vpt.h);
			}
			if (vpt.wide) glViewport(vpt.x, vpt.y, vpt.w, vpt.h);
			// The controls: button labels (DE_CButtonControl3d::SetText: 16 of 600 virtual pixels,
			// centred; white, the colour the screens pass), then a notification over everything.
			const float white[4] = {1, 1, 1, 1};
			auto draw_items = [&](const std::vector<game::ui::DrawItem> &items) {
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
			};
			if (app.current && app.current->activated) {
				std::vector<game::ui::DrawItem> items;
				game::ui::UIPtr page = app.current;
				if (page->gui) page->gui->paint(items);
				page->draw(app, items);
				if (page->gui) page->gui->paint_over(items);
				draw_items(items);
				for (auto &c : app.current->controls) {
					overlay.label(c.text, (c.left + c.right) / 2, (c.top + c.bottom) / 2, 16.0f / 600.0f, white, vpt.w, vpt.h);
					const float cover[4] = {0, 0, 0, 0.5f};
					if (c.read_only) overlay.rect(c.left, c.top, c.right, c.bottom, cover, vpt.w, vpt.h);
				}
			}
			// The doors close over the page's controls, which are 3D controls behind them in the original.
			if (vpt.wide) glViewport(0, 0, dw, dh);
			actors_r.draw(world.data(), doors, vp, eye);
			if (vpt.wide) glViewport(vpt.x, vpt.y, vpt.w, vpt.h);
			render::g_lighting = game_lighting;
			if (game::ui::Gui *g = app.notification_gui()) {
				std::vector<game::ui::DrawItem> items;
				g->paint(items);
				g->paint_over(items);
				draw_items(items);
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
				// Minimal Linux desktops may have no Pictures folder yet.
				std::error_code ec;
				std::filesystem::create_directories(de::pictures_dir(), ec);
				std::string out = (de::pictures_dir() / name).string();
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
			if (perf::enabled()) {
				{ PERF_ZONE("frame.gpu_finish"); glFinish(); }
				static Uint64 perf_t0 = SDL_GetPerformanceCounter();
				static int perf_f0 = fr;
				double perf_ms = double(SDL_GetPerformanceCounter() - perf_t0) * 1000.0 / double(SDL_GetPerformanceFrequency());
				if (perf_ms >= 5000) {
					fprintf(stderr, "[counts %d] script arena %u MB, %s\n", fr, world.memory().used() >> 20, world.debug_counts().c_str());
					perf::report(fr, fr - perf_f0, perf_ms);
					perf_t0 = SDL_GetPerformanceCounter();
					perf_f0 = fr;
				}
			}
			SDL_GL_SwapWindow(win);
		}
		SDL_GL_DeleteContext(ctx);
		SDL_DestroyWindow(win);
		SDL_Quit();
		if (shot) {
			for (auto &l : world.log) printf("log: %s\n", l.c_str());
			for (auto &[k, n] : world.missing()) printf("missing native: %s (%llu calls)\n", k.c_str(), (unsigned long long)n);
		}
	} catch (const std::exception &e) {
		fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
	return 0;
}
