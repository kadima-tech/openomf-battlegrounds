// openbg-fight: a fighter driven by its own compiled brain through the native runtime.
//
//   openbg-fight <Data dir> <Fighter, e.g. Jaguar> [--opponent Katana|none] [--arena Desert]
//                [--opponent-pilot Elizabeth] [--difficulty rookie|amateur|veteran|champion|world-champion]
//                [--pilot Lucas] [--wins 1|2|5|10|20|50] [--direct]
//                [--watch] [--headless [frames]] [--screenshot out.png --frames N]
//
// Controls: arrows = joystick (up walks forward, down back, left/right turn), A/S = left/right
// punch, Z/X = left/right kick, Space = jump (with a direction to jump that way), Left Shift or
// C = duck/evade (one button in the original; add left/right to sidestep). Drag to orbit.
//
// The match is the game's own Last Man Standing session (session.h): its scripts load the arena,
// build both robots and pilots from their descriptions (as the menus would have chosen them),
// spawn them, count down, referee knockouts and rounds up to the score limit, and announce the
// winner. The opponent is driven by its pilot's script (BStandardPilot) and its fighter's AI.
// --direct skips the session: both robots are set up and spawned by this program, with a
// stand-in pilot. Headless mode prints what the brains do, the session's on-screen messages
// and which natives are still missing. Drag to orbit the camera, scroll to zoom, Esc to quit.
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include "game/forces.h"
#include "game/runtime.h"
#include "game/session.h"
#include "audio.h"
#include "overlay.h"
#include "particles_gl.h"
#include "render.h"
#include "glow.h"
#include "scene_lights.h"
#include "de/lighting.h"

namespace {

struct Fighter {
	std::string name;
	game::GameObject *actor = nullptr;
	game::Brain *brain = nullptr;
	de::AnimBank bank;
	std::map<std::string, game::InputSequence> inputs;  // the fighter's own move sequences
	render::FighterRenderer renderer;
	bool renderer_loaded = false;
	std::string last_state;
	game::Brain *stats = nullptr;  // PFighterStats
};

Fighter &setup_fighter(game::World &world, const std::string &name, bool user, bool with_stats = true) {
	const de::ObjectsFile *ff = world.data().file(name);
	if (!ff) {
		throw std::runtime_error("no fighter file " + name);
	}
	world.instantiate_file(name);
	static std::deque<Fighter> keep;  // banks must outlive the world's pointers
	Fighter &fi = keep.emplace_back();
	fi.name = name;
	for (auto &o : ff->objects()) {
		if (o.class_path == "JoystickInputSequenceGroup") {
			fi.inputs = game::read_input_bank(*ff, o);
		}
	}
	if (auto *common = world.data().file("Fighter")) {
		for (auto &o : common->objects()) {
			if (o.class_path == "JoystickInputSequenceGroup" && o.name == "StandardInputSequenceBank") {
				world.standard_inputs = game::read_input_bank(*common, o);
				if (getenv("OPENBG_DEBUG_INPUTS")) {  // the standard move sequences (button value/mask, directions)
					for (auto &[n, seq] : world.standard_inputs) {
						fprintf(stderr, "[input] %s:", n.c_str());
						for (auto &r : seq) fprintf(stderr, " {%x/%x dirs %x age %u-%u gap %u-%u}", r.value, r.mask, r.directions, r.min_age, r.max_age, r.min_gap, r.max_gap);
						fprintf(stderr, "\n");
					}
				}
			}
		}
	}
	de::ObjectRef cls_obj;
	for (uint32_t i = 0; i < ff->objects().size(); ++i) {
		auto &o = ff->objects()[i];
		if (!fi.actor && o.class_path == "Actor\\Creature\\OMF2Fighter") {
			// CFighterDescription::NewResource: the fighter is a copy of the file's robot (the
			// file's own objects stay the template, as globals and copies refer to them).
			if (game::GameObject *tpl = world.object_for(ff, i)) fi.actor = world.copy_tree(*tpl);
		}
		if (o.class_path == "AnimationSequenceGroup" && fi.bank.sequences.empty()) {
			fi.bank = de::read_anim_bank(*ff, o);
		}
		if (o.class_path == "Class" && o.name == "P" + name) {
			cls_obj = {ff, i};
		}
	}
	if (!fi.actor || !cls_obj) {
		throw std::runtime_error("fighter actor or class not found");
	}
	fi.actor->bank = &fi.bank;
	fi.actor->inputs = &fi.inputs;
	if (getenv("OPENBG_DEBUG_INPUTS")) {  // the robot's own move sequences
		for (auto &[n, seq] : fi.inputs) {
			fprintf(stderr, "[input %s] %s:", name.c_str(), n.c_str());
			for (auto &r : seq) fprintf(stderr, " {%x/%x dirs %x age %u-%u gap %u-%u}", r.value, r.mask, r.directions, r.min_age, r.max_age, r.min_gap, r.max_gap);
			fprintf(stderr, "\n");
		}
	}
	if (getenv("OPENBG_DEBUG_CHANNELS")) {  // the animation channels (bone actors the bank drives)
		fprintf(stderr, "[channels] %s:", name.c_str());
		for (auto &ch : fi.bank.channels) fprintf(stderr, " %s", ch.name.c_str());
		fprintf(stderr, "\n");
	}
	fi.actor->user_controlled = user && !world.all_ai;
	// The robot's own executors (its properties: StandardJaguarExecutor running PJaguar, EPreCache),
	// attached as the scene's preparation does; they run from the next tick. A fighter without
	// one gets P<Name> at "Entry".
	world.activate_executors(fi.actor, world.properties_of(*fi.actor), false);
	fi.brain = fi.actor->brain;
	if (!fi.brain || fi.brain->cls.name != "P" + name) fi.brain = world.attach_brain(fi.actor, cls_obj, "Entry");
	// Fighter.DEObjects FighterStatsExecutor: PFighterStats (health, energy, stun) runs on its own
	// object under the fighter (BFighterStats::Entry takes the creature from Parent()).
	if (auto *common = with_stats ? world.data().file("Fighter") : nullptr) {
		for (uint32_t i = 0; i < common->objects().size(); ++i) {
			if (common->objects()[i].class_path == "Class" && common->objects()[i].name == "PFighterStats") {
				auto *stats = world.create_child(fi.actor, "Stats", "ClassExecutor");
				stats->component_owner = fi.actor;  // a component: Interface() reaches its brain
				fi.stats = world.attach_brain(stats, {common, i}, "Entry");
			}
		}
	}
	return fi;
}

// BObject::ObjectSpawn with a position and orientation, as the session does from a spawn point.
void spawn(game::World &world, Fighter &f, const de::Transform &at) {
	auto &mem = world.memory();
	uint32_t pos = mem.alloc(12), orient = mem.alloc(36), mom = mem.alloc(12);
	for (int k = 0; k < 3; ++k) {
		mem.set<float>(pos + uint32_t(k) * 4, at.t[size_t(k)]);
	}
	memcpy(mem.ptr(orient, 36), at.r.data(), 36);
	std::vector<uint8_t> args(16);
	memcpy(&args[4], &pos, 4);
	memcpy(&args[8], &orient, 4);
	memcpy(&args[12], &mom, 4);
	f.actor->spawned = true;
	f.brain->call(world, "bool ObjectSpawn(class CVector,class CCoordinateSystem,class CVector)", args);
}

// The arena's player spawn points (actors named PlayerSpawnPoint*, carrying BPlayerSpawnPoint),
// sorted by name as BStandardOMFSession::GatherSpawnPoints does; slot N spawns at point N.
std::vector<de::Transform> spawn_points(const de::World &data, const std::string &arena) {
	std::vector<std::pair<std::string, de::Transform>> found;
	const de::ObjectsFile *f = data.file(arena);
	if (!f) {
		return {};
	}
	const auto &objs = f->objects();
	auto parents = f->parents();
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (objs[i].name.rfind("PlayerSpawnPoint", 0) != 0 || objs[i].class_path.rfind("Actor", 0) != 0) {
			continue;
		}
		de::Transform t;
		bool ok = true;
		for (int32_t k = int32_t(i); k >= 0; k = parents[size_t(k)]) {
			if (objs[size_t(k)].class_path.rfind("Actor", 0) != 0) {
				continue;
			}
			try {
				t = de::read_actor(*f, objs[size_t(k)]).local.then(t);
			} catch (const std::exception &) {
				ok = false;
			}
		}
		if (ok) {
			found.emplace_back(objs[i].name, t);
		}
	}
	std::sort(found.begin(), found.end(), [](auto &a, auto &b) { return a.first < b.first; });
	std::vector<de::Transform> out;
	for (auto &[n, t] : found) {
		out.push_back(t);
	}
	return out;
}

void print_report(game::World &world) {
	for (auto &l : world.log) {
		if (l.find(": ") != l.size() - 2) {
			printf("log: %s\n", l.c_str());
		}
	}
	std::vector<std::pair<uint64_t, std::string>> miss;
	for (auto &[k, n] : world.missing()) {
		miss.emplace_back(n, k);
	}
	std::sort(miss.rbegin(), miss.rend());
	printf("%zu natives still missing (by calls):\n", miss.size());
	for (size_t i = 0; i < miss.size() && i < 40; ++i) {
		printf("  %6llu  %s\n", (unsigned long long)miss[i].first, miss[i].second.c_str());
	}
}

}  // namespace

// The arena's environment description: the one whose resource is the arena's environment
// creature (its root object when it has none).
static std::optional<de::Guid> environment_description(game::World &world, const std::string &arena) {
	auto *af = world.data().file(arena);
	auto *ad = world.data().file("ArenaDescriptions");
	if (!af || af->objects().empty() || !ad) return std::nullopt;
	auto root = af->objects()[0].guid;
	for (auto &o : af->objects()) {
		if (o.class_path == "Actor\\Creature\\Environment") {
			root = o.guid;
			break;
		}
	}
	std::optional<de::Guid> found;
	for (auto &o : ad->objects()) {
		if (o.class_path != "Description\\Environment") continue;
		auto c = ad->object_data(o).contents;
		if (std::search(c.begin(), c.end(), root.bytes, root.bytes + 16) != c.end()) found = o.guid;
	}
	return found;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <Data dir> <Fighter> [--arena Desert] [--pilot Lucas] [--watch] [--headless [frames]] [--screenshot out.png --frames N]\n",
		        argv[0]);
		return 2;
	}
	std::string data = argv[1], name = argv[2], arena = "Desert", pilot = "Lucas", opponent = "Katana",
	            opponent_pilot = "Elizabeth";
	int difficulty = 0, wins = 2;  // DE_COMF2Manager::Activate: registry AIDifficulty, default 0 (Rookie)
	const char *shot = nullptr;
	bool headless = false, session_mode = true, free_camera = false, watch = false;
	int frames = 240;
	for (int i = 3; i < argc; ++i) {
		if (!strcmp(argv[i], "--arena") && i + 1 < argc) {
			arena = argv[++i];
		} else if (!strcmp(argv[i], "--opponent") && i + 1 < argc) {
			opponent = argv[++i];
		} else if (!strcmp(argv[i], "--pilot") && i + 1 < argc) {
			pilot = argv[++i];
		} else if (!strcmp(argv[i], "--opponent-pilot") && i + 1 < argc) {
			opponent_pilot = argv[++i];
		} else if (!strcmp(argv[i], "--difficulty") && i + 1 < argc) {
			// DE_COMF2Manager::SetupAIDifficultyList: Rookie, Amateur, Veteran, Champion, World Champion.
			std::string d = argv[++i];
			const char *names[] = {"rookie", "amateur", "veteran", "champion", "world-champion"};
			difficulty = isdigit(d[0]) ? atoi(d.c_str()) : 2;
			for (int k = 0; k < 5; ++k) {
				if (d == names[k]) difficulty = k;
			}
		} else if (!strcmp(argv[i], "--free-camera")) {
			free_camera = true;
		} else if (!strcmp(argv[i], "--watch")) {
			watch = true;
		} else if (!strcmp(argv[i], "--direct")) {
			session_mode = false;
		} else if (!strcmp(argv[i], "--wins") && i + 1 < argc) {
			wins = atoi(argv[++i]);

		} else if (!strcmp(argv[i], "--headless")) {
			headless = true;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				frames = atoi(argv[++i]);
			}
		} else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
			shot = argv[++i];
		} else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			frames = atoi(argv[++i]);
		}
	}
	try {
		fprintf(stderr, "Loading game data from %s (this takes a while)...\n", data.c_str());
		game::World world(data);
		world.all_ai = watch;
		// Pilots (Pilots.DEObjects): stats for the player and the computer opponent.
		static std::deque<game::Pilot> pilots;
		auto load_pilot = [&](const std::string &who) -> const game::Pilot * {
			if (auto *pf = world.data().file("Pilots")) {
				for (auto &o : pf->objects()) {
					if (o.class_path == "Actor\\Creature\\Pilot" && o.name == who) {
						if (auto p = game::read_pilot(*pf, o, difficulty)) {
							printf("pilot %s: power %.0f agility %.0f endurance %.0f focus %.0f\n", who.c_str(),
							       p->stats.power, p->stats.agility, p->stats.endurance, p->stats.focus);
							return &pilots.emplace_back(*p);
						}
					}
				}
			}
			fprintf(stderr, "pilot %s not found; using neutral stats\n", who.c_str());
			return nullptr;
		};
		std::vector<Fighter *> fighters;
		Fighter *player = nullptr;
		const int spawn_frame = 30;
		std::vector<de::Transform> points;
		if (session_mode) {
			// The game's own session: Last Man Standing, with the robots, pilots and arena chosen
			// up front as BSinglePlayerSubSession does (SubSessionPick*ByGUID) and the menus
			// would. The scripts create the fighters, spawn them and referee the rounds.
			world.ai_difficulty = difficulty;
			world.make_fighter = [&](const de::ObjectRef &actor) -> game::GameObject * {
				Fighter &nf = setup_fighter(world, actor.info().name, false, false);
				fighters.push_back(&nf);
				return nf.actor;
			};
			world.on_creature_owned = [&](game::GameObject *c, int slot) {
				for (auto *fi : fighters) {
					if (fi->actor == c && slot == 0) player = fi;
				}
				if (player) world.focus = player->actor;
				printf("slot %d: %s%s\n", slot, c->name.c_str(), c->user_controlled ? " (player)" : " (computer)");
			};
			auto find = [&](const std::string &file, const std::string &cls, const std::string &obj) -> de::ObjectRef {
				if (auto *ff = world.data().file(file)) {
					for (uint32_t i = 0; i < ff->objects().size(); ++i) {
						if (ff->objects()[i].class_path == cls && ff->objects()[i].name == obj) return {ff, i};
					}
				}
				return {};
			};
			de::ObjectRef master = find("Session", "SessionModifier\\StandardSessionMaster", "LastManStandingMaster");
			if (!master) throw std::runtime_error("LastManStandingMaster not found");
			auto &ss = game::start_session(world, {master});
			// The game options the menus add as session modifiers: the score limit ("N Wins").
			{
				std::string wins_name = wins == 1 ? "One Win" : std::to_string(wins) + " Wins";
				de::ObjectRef d = find("Session", "Description\\SessionModifier", wins_name);
				game::GameObject *tpl = d ? world.global_object(d) : nullptr;
				game::GameObject *m = tpl ? game::new_resource(world, world.copy_tree(*tpl), nullptr) : nullptr;
				if (m) game::add_session_modifier(world, m);
				else fprintf(stderr, "no \"%s\" score limit; rounds go on until you quit\n", wins_name.c_str());
			}
			if (auto g = environment_description(world, arena)) ss.ui_choices["Choose Environment"] = *g;
			if (!ss.ui_choices.count("Choose Environment")) throw std::runtime_error("no environment description for " + arena);
			game::begin_session(world);
			// Pick each slot's robot and pilot (SessionSlotSelectCreature/Pilot, Object1 = GUID).
			auto pick = [&](size_t slot, const char *handle, const de::ObjectRef &desc) {
				if (!desc || slot >= ss.slots.size()) {
					fprintf(stderr, "cannot pick for slot %zu\n", slot);
					return;
				}
				uint32_t g = world.make_instance("CObject::CGUID", 1);
				memcpy(world.memory().ptr(g, 16), desc.info().guid.bytes, 16);
				uint32_t e = game::new_event(world, handle);
				world.memory().set<uint32_t>(e + 8, g);
				world.send_event(ss.slots[slot]->obj, e);
			};
			std::vector<std::pair<std::string, std::string>> picks = {{name, pilot}};
			if (opponent != "none") picks.push_back({opponent, opponent_pilot});
			for (size_t i = 0; i < picks.size(); ++i) {
				// A robot's description may live in another file (Force's is in ForceModel1).
				de::ObjectRef fd = find(picks[i].first, "Description\\Creature\\Fighter", picks[i].first);
				if (!fd) fd = world.data().find("Description\\Creature\\Fighter", picks[i].first);
				pick(i, "SessionSlotSelectCreature", fd);
				pick(i, "SessionSlotSelectPilot", find("Pilots", "Description\\Pilot", picks[i].second));
				game::add_client(world, i == 0, picks[i].second);
			}
			printf("session: %zu modifiers, %zu slots\n", ss.modifiers.size(), ss.slots.size());
		} else {
			// BCreatureAndPilotPicker::AttemptToCreatePilot: the pilot is its description's resource,
			// the description goes under it, and the pilot under the creature (it starts with it).
			// OPENBG_IDLE_AI=<robot> leaves that fighter without a pilot: a still target.
			static const char *idle = getenv("OPENBG_IDLE_AI");
			auto make_pilot = [&](Fighter &fi, const std::string &who) {
				if (idle && fi.actor->name == idle) {
					fi.actor->pilot = load_pilot(who);
					return;
				}
				de::ObjectRef dref;
				if (auto *pf = world.data().file("Pilots")) {
					for (uint32_t i = 0; i < pf->objects().size(); ++i) {
						if (pf->objects()[i].class_path == "Description\\Pilot" && pf->objects()[i].name == who) dref = {pf, i};
					}
				}
				game::GameObject *tpl = dref ? world.global_object(dref) : nullptr;
				game::GameObject *desc = tpl ? world.copy_tree(*tpl) : nullptr;
				game::GameObject *p = desc ? game::new_resource(world, desc, nullptr) : nullptr;
				if (!p) {
					fprintf(stderr, "pilot %s not found; using neutral stats and no pilot\n", who.c_str());
					return;
				}
				desc->parent = p;
				p->children.push_back(desc);
				p->parent = fi.actor;
				fi.actor->children.push_back(p);
				if (p->pilot) printf("pilot %s: power %.0f agility %.0f endurance %.0f focus %.0f\n", who.c_str(), p->pilot->stats.power,
				                     p->pilot->stats.agility, p->pilot->stats.endurance, p->pilot->stats.focus);
			};
			Fighter &f = setup_fighter(world, name, true);
			make_pilot(f, pilot);
			player = &f;
			fighters.push_back(&f);
			if (opponent != "none") {
				fighters.push_back(&setup_fighter(world, opponent, false));
				make_pilot(*fighters.back(), opponent_pilot);
			}
			// The arena itself, as the session's environment picker creates it (the description's
			// resource, owned by the scene): its surfaces, pads, mines and lights.
			if (auto g = environment_description(world, arena)) {
				game::GameObject *desc = world.global_object(world.data().by_guid(*g));
				if (game::GameObject *env = desc ? game::new_resource(world, desc, nullptr) : nullptr) world.enter_scene(env);
				else fprintf(stderr, "cannot create the %s environment\n", arena.c_str());
			}
			// Each fighter joins the scene as a slot's OwnCreature puts it there (its controller made,
			// its update scheduled); the stats executor given by hand runs its entry.
			for (auto *fi : fighters) {
				bool user = fi->actor->user_controlled;
				world.enter_scene(fi->actor);
				world.set_user_controlled(*fi->actor, user ? 0 : -1);
				if (fi->stats) world.start_brain(fi->stats);
			}
			for (auto *fi : fighters) {
				printf("%s: brain %s, %zu methods; %zu moves%s\n", fi->actor->name.c_str(), fi->brain->cls.name.c_str(),
				       fi->brain->cls.methods.size(), fi->bank.sequences.size(),
				       fi->actor->user_controlled ? " (player)" : " (computer)");
			}
			// Spawn points from the arena; without enough of them two facing points 120 apart.
			points = spawn_points(world.data(), arena);
			if (points.size() < fighters.size()) {
				de::Transform a, b;
				a.t = {0, 40, -60};
				b.t = {0, 40, 60};
				b.r = {-1, 0, 0, 0, 1, 0, 0, 0, -1};
				points = {a, b};
			}
			printf("spawn points: %zu\n", points.size());
		}
		auto spawn_all = [&](float) {
			if (session_mode) return;  // the session spawns
			// BStandardOMFSession::GatherAllSpawnPoints: the scene's actors with a BPlayerSpawnPoint
			// behavior, sorted by name; slot n spawns at point n (SpawnCreatureInSlot).
			std::vector<game::GameObject *> gathered;
			for (game::GameObject *o : world.scene_objects()) {
				if (world.has_interface_named(o, "BPlayerSpawnPoint")) gathered.push_back(o);
			}
			std::stable_sort(gathered.begin(), gathered.end(), [](auto *a, auto *b) { return a->name < b->name; });
			if (!gathered.empty()) {
				points.clear();
				for (auto *o : gathered) points.push_back(o->world());
			}
			for (size_t i = 0; i < fighters.size(); ++i) {
				spawn(world, *fighters[i], points[i % points.size()]);
			}
		};
		// Stand-in pilot (BStandardPilot::StateNormal -> FindAttackTarget(1) -> AttackBestEnemy):
		// each computer fighter is kept on an attack command against its first enemy.
		// --direct runs no session; each fighter's pilot drives it as in one (see make_pilot).
		auto pilot_tick = [&]() {};
		// BFighterStats::NormalizedHealth through the stats brain (0..1).
		auto health_of = [&](Fighter &fi) -> float {
			if (!fi.stats) {
				for (auto *ch : fi.actor->children) {
					if (ch->brain && ch->brain->cls.name == "PFighterStats") fi.stats = ch->brain;
				}
			}
			if (!fi.stats) return -1;
			std::vector<uint8_t> args(4);
			if (!fi.stats->call(world, "float NormalizedHealth()", args)) return -1;
			float h;
			memcpy(&h, args.data(), 4);
			return h;
		};
		auto state_of = [&](Fighter &fi) {
			auto *m = fi.brain->exec->current_method();
			return std::string(m ? m->cm->signature : "-") + "  anim " +
			       (fi.actor->sequence >= 0 ? fi.bank.sequences[size_t(fi.actor->sequence)].name : "-");
		};

		// Optional scripted input (headless and screenshots): HOLD="up@150-300,left_punch@320-330" (button@from-to).
		std::vector<std::tuple<std::string, int, int>> holds;
		if (const char *h = getenv("HOLD")) {
			std::string spec = h;
			size_t p = 0;
			while (p < spec.size()) {
				size_t q = spec.find(',', p);
				std::string item = spec.substr(p, q == std::string::npos ? std::string::npos : q - p);
				int a = 0, b = 0;
				char key[32] = {};
				if (sscanf(item.c_str(), "%31[a-z_]@%d-%d", key, &a, &b) == 3) {
					holds.emplace_back(key, a, b);
				}
				if (q == std::string::npos) break;
				p = q + 1;
			}
		}
		std::map<std::string, bool game::Input::*> keys = {
			{"up", &game::Input::up}, {"down", &game::Input::down}, {"left", &game::Input::left},
			{"right", &game::Input::right}, {"jump", &game::Input::jump}, {"duck", &game::Input::duck},
			{"evade", &game::Input::evade}, {"left_punch", &game::Input::left_punch},
			{"right_punch", &game::Input::right_punch}, {"left_kick", &game::Input::left_kick},
			{"right_kick", &game::Input::right_kick}};
		// OPENBG_PLACE="Jaguar:0,90,1035@900": put a fighter somewhere at a frame (testing pads,
		// mines and the death volume).
		std::vector<std::tuple<std::string, std::array<float, 3>, int>> places;
		if (const char *pl = getenv("OPENBG_PLACE")) {  // several, separated by ';'
			std::string spec = pl;
			for (size_t p = 0; p < spec.size();) {
				size_t q = spec.find(';', p);
				std::string item = spec.substr(p, q == std::string::npos ? std::string::npos : q - p);
				char who[32] = {};
				float x = 0, y = 0, z = 0;
				int at = 0;
				if (sscanf(item.c_str(), "%31[^:]:%f,%f,%f@%d", who, &x, &y, &z, &at) == 5) places.emplace_back(who, std::array<float, 3>{x, y, z}, at);
				if (q == std::string::npos) break;
				p = q + 1;
			}
		}
		// OPENBG_FIND="EnergyBoost@2600": where objects of that name are at that frame.
		std::string find_name;
		int find_at = -1;
		if (const char *fd = getenv("OPENBG_FIND")) {
			char nm[64] = {};
			if (sscanf(fd, "%63[^@]@%d", nm, &find_at) == 2) find_name = nm;
		}
		auto apply_holds = [&](int fr) {
			if (fr == find_at) {
				for (game::GameObject *o : world.scene_objects()) {
					if (o->name != find_name) continue;
					auto t = o->world().t;
					game::Brain *br = nullptr;
					for (game::GameObject *x = o; x && !br; x = nullptr) br = x->brain;
					auto *st = br && br->exec ? br->exec->current_state() : nullptr;
					fprintf(stderr, "[find %d] %s (%s) at %.1f %.1f %.1f parent %s active %d visible %d state %s\n", fr, o->name.c_str(), o->class_path.c_str(),
					        t[0], t[1], t[2], o->parent ? o->parent->name.c_str() : "-", o->active, o->visible, st ? st->cm->signature.c_str() : "-");
				}
			}
			world.input = {};
			for (auto &[k, a, b] : holds) {
				if (fr >= a && fr <= b && keys.count(k)) {
					world.input.*keys[k] = true;
				}
			}
			for (auto &[who, p, at] : places) {
				if (fr != at) continue;
				for (auto *fi : fighters) {
					if (fi->actor->name == who) fi->actor->local.t = p, fi->actor->momentum = {0, 0, 0};
				}
			}
		};

		// OPENBG_METHODS="Katana:651,833": name class method indices (op 34/38 operands).
		if (const char *mi = getenv("OPENBG_METHODS")) {
			std::string spec = mi, who = spec.substr(0, spec.find(':')), rest = spec.substr(spec.find(':') + 1);
			for (auto *fi : fighters) {
				if (fi->name != who) continue;
				for (size_t p = 0; p < rest.size();) {
					size_t e = rest.find(',', p);
					uint32_t idx = uint32_t(atoi(rest.c_str() + p));
					auto *m = fi->brain->cls.method(idx);
					printf("method %u: %s\n", idx, m ? m->cm->signature.c_str() : "-");
					p = e == std::string::npos ? rest.size() : e + 1;
				}
			}
		}
		// On-screen messages the session scripts show (BText through CTextPrimitive: "READY",
		// "FIGHT", "<pilot> Wins", the timer...) go to the terminal until the HUD draws them.
		std::map<const game::GameObject *, std::string> shown_text;
		auto print_texts = [&](int fr) {
			for (auto &[obj, t] : world.texts) {
				auto &last = shown_text[obj];
				if (t.text != last) {
					last = t.text;
					if (!t.text.empty()) printf("frame %5d  [screen] %s\n", fr, t.text.c_str());
				}
				if (getenv("OPENBG_DEBUG_TEXT") && t.text.empty() && fr % 300 == 0 && !world.screen_space_meshes.count(t.mesh)) {
					printf("frame %5d  [text] empty world primitive (kind %d) on %s\n", fr, int(t.kind), t.mesh->parent ? t.mesh->parent->name.c_str() : "-");
				}
				if (getenv("OPENBG_DEBUG_TEXT") && !t.text.empty() && fr % 30 == 0) {
					printf("frame %5d  [text] \"%s\" at (%.2f %.2f) h %.3f rgba (%.2f %.2f %.2f %.2f)%s%s\n", fr, t.text.c_str(), t.x,
					       t.y, t.height, t.rgba[0], t.rgba[1], t.rgba[2], t.rgba[3],
					       world.screen_space_meshes.count(t.mesh) ? " screen" : " world", t.centered ? " centered" : "");
				}
			}
		};
		// The frame's sounds are heard from the player's Listener (BFighterCameraMover moves it under
		// the fighter), facing as the fight camera does.
		auto ear_of_player = [&]() {
			de::Transform ear;
			if (!player) return ear;
			std::function<game::GameObject *(game::GameObject *, const char *)> find = [&](game::GameObject *o, const char *n) -> game::GameObject * {
				for (auto *ch : o->children) {
					if (ch->name == n) return ch;
					if (auto *r = find(ch, n)) return r;
				}
				return nullptr;
			};
			auto *cam = find(player->actor, "CameraActor");
			auto *lis = find(player->actor, "Listener");
			ear = cam ? cam->world() : player->actor->world();
			if (lis) ear.t = lis->world().t;
			return ear;
		};
		if (headless) {
			const bool changes = getenv("OPENBG_CHANGES") != nullptr;  // print every state/anim change instead
			const int every = getenv("OPENBG_EVERY") ? atoi(getenv("OPENBG_EVERY")) : 10;
			// OPENBG_RECORD_AUDIO=<file>: the frames' sound and music, mixed offline (raw f32 stereo,
			// 44100 Hz), heard as in a window.
			std::unique_ptr<audio::Mixer> offline;
			if (getenv("OPENBG_RECORD_AUDIO")) {
				offline = std::make_unique<audio::Mixer>();
				offline->open_offline(44100);
			}
			double audio_carry = 0;
			for (int fr = 0; fr < frames; ++fr) {
				if (fr == spawn_frame) {
					spawn_all(0);
				}
				apply_holds(fr);
				pilot_tick();
				world.tick();
				print_texts(fr);
				if (getenv("OPENBG_DEBUG_SOUND")) {
					for (auto &r : world.sound_requests) {
						printf("frame %5d  [sound] %s vol %.2f%s\n", fr, r.noise->name.c_str(), r.volume, r.positional ? "" : " (ambient)");
					}
				}
				if (offline) {
					offline->play_requests(world, ear_of_player());
					offline->update_music(world);
					audio_carry += 44100.0 * world.frame_ms() / 1000.0;
					int n = int(audio_carry);
					audio_carry -= n;
					offline->pump(n);
				}
				world.sound_requests.clear();
				// OPENBG_WATCH="RightPunchMediumHitDetector": print that actor's world position and
				// facing (player fighter).
				if (const char *w = getenv("OPENBG_WATCH")) {
					std::function<game::GameObject *(game::GameObject *)> find = [&](game::GameObject *o) -> game::GameObject * {
						for (auto *ch : o->children) {
							if (ch->name == w) return ch;
							if (auto *r = find(ch)) return r;
						}
						return nullptr;
					};
					if (auto *a = player ? find(player->actor) : nullptr; a && fr % 4 == 0) {
						auto wt = a->world();
						auto p = wt.t;
						auto z = wt.rotate({0, 0, 1});
						auto f = player->actor->world().t;
						printf("watch frame %d %s (%.1f %.1f %.1f) z (%.2f %.2f %.2f), from fighter (%.1f %.1f %.1f)\n", fr, w, p[0], p[1], p[2], z[0], z[1], z[2],
						       p[0] - f[0], p[1] - f[1], p[2] - f[2]);
					}
				}
				// OPENBG_MEMBERS="Katana:216,220f": print brain instance members (byte offsets; f = float)
				// whenever one changes.
				if (const char *mv = getenv("OPENBG_MEMBERS")) {
					std::string spec = mv, who = spec.substr(0, spec.find(':'));
					for (auto *fi : fighters) {
						if (fi->name != who) continue;
						std::string line, rest = spec.substr(spec.find(':') + 1);
						for (size_t p = 0; p < rest.size();) {
							size_t e = rest.find(',', p);
							std::string tok = rest.substr(p, e == std::string::npos ? std::string::npos : e - p);
							p = e == std::string::npos ? rest.size() : e + 1;
							bool isf = !tok.empty() && tok.back() == 'f';
							uint32_t off = uint32_t(atoi(tok.c_str()));
							char buf[64];
							uint32_t a = fi->brain->inst.members + off;
							if (isf) snprintf(buf, sizeof buf, " m%u=%.3f", off, double(world.memory().get<float>(a)));
							else snprintf(buf, sizeof buf, " m%u=%d", off, world.memory().get<int32_t>(a));
							line += buf;
						}
						static std::string last;
						if (line != last) printf("members frame %d %s%s\n", fr, who.c_str(), line.c_str());
						last = line;
					}
				}
				for (auto *fi : fighters) {
					auto *m = fi->brain->exec->current_method();
					std::string seq = fi->actor->sequence >= 0 ? fi->bank.sequences[size_t(fi->actor->sequence)].name : "-";
					std::string now = (m ? m->cm->signature : "-") + "|" + seq;
					if (changes ? now != fi->last_state : fr % every == 0) {
						fi->last_state = now;
						printf("frame %4d  %-8s %-40s anim %-22s t=%.2f  pos (%.1f %.1f %.1f)%s  health %.0f%%\n", fr,
						       fi->name.c_str(), m ? m->cm->signature.c_str() : "-", seq.c_str(), fi->actor->anim_time,
						       fi->actor->local.t[0], fi->actor->local.t[1], fi->actor->local.t[2],
						       fi->actor->on_floor ? " floor" : "", health_of(*fi) * 100);
					}
				}
			}
			print_report(world);
			return 0;
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
		std::string title = "One Must Fall: Battlegrounds — " + name + (fighters.size() > 1 ? " vs " + opponent : "") + " in " + arena;
		SDL_Window *win = SDL_CreateWindow(title.c_str(),
		                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
		                                   SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | (shot ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
		SDL_GLContext ctx = SDL_GL_CreateContext(win);
		SDL_GL_SetSwapInterval(1);
		if (shot) {
			glu::offscreen(W, H);
		}

		render::ArenaRenderer arena_r;
		arena_r.load(world.data(), arena);
		render::OverlayRenderer overlay;
		overlay.load(world.data());
		render::ParticleRenderer particles_r;
		render::GlowRenderer glow_r;
		render::ActorMeshRenderer actors_r;
		actors_r.image_of = [&world](const de::ObjectRef &tm) -> std::optional<de::ObjectRef> {
			auto it = world.texture_overrides.find({tm.file, tm.index});
			if (it == world.texture_overrides.end()) return std::nullopt;
			return it->second;
		};
		// A Mantis bug's images carry the fighter's paint filter: the filter's fighter's pilot colours.
		actors_r.paint_of = [&world](const de::ObjectRef &img) -> std::optional<std::array<std::array<float, 3>, 3>> {
			auto it = world.image_filters.find({img.file, img.index});
			if (it == world.image_filters.end()) return std::nullopt;
			for (game::GameObject *p = it->second; p; p = p->parent) {
				if (p->fighter) {
					if (const game::Pilot *pl = game::pilot_of(p)) return pl->colours;
					break;
				}
			}
			return std::nullopt;
		};
		particles_r.load();
		std::array<float, 16> screen_vp{};
		audio::Mixer mixer;
		if (!shot && !mixer.open()) fprintf(stderr, "no audio device: %s\n", SDL_GetError());
		world.render_ticks_per_frame = shot != nullptr;  // screenshots step one game frame per drawn frame
		auto load_renderers = [&] {
			for (auto *fi : fighters) {
				if (!fi->renderer_loaded) {
					fi->renderer.load(world.data(), *world.data().file(fi->name + "Model1"), world.data().file(fi->name));
					fi->renderer_loaded = true;
				}
			}
		};
		load_renderers();
		const float fighter_radius = 60;  // camera framing
		world.floor = [&](float x, float z, float from) { return arena_r.floor_height(x, z, from); };
		float floor0 = arena_r.floor_height(0, 0);
		if (floor0 < -1e29f) {
			floor0 = 0;
		}
		printf("arena %s: %zu triangles, floor at centre y=%.1f\n", arena.c_str(), arena_r.triangles, floor0);

		float yaw = 0.6f, pitch = 0.18f, distance = fighter_radius * 5.0f;
		bool running = true, dragging = false;
		int fr = 0;
		double accum = 0;
		float cam_base = 0;  // camera heading, trailing the fighter's facing
		bool cam_init = false;
		Uint64 last = SDL_GetPerformanceCounter();
		while (running) {
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) running = false;
				if (e.type == SDL_MOUSEBUTTONDOWN) dragging = true;
				if (e.type == SDL_MOUSEBUTTONUP) dragging = false;
				if (e.type == SDL_MOUSEMOTION && dragging) {
					yaw += e.motion.xrel * 0.01f;
					pitch = std::fmax(-0.3f, std::fmin(1.4f, pitch + e.motion.yrel * 0.01f));
				}
				if (e.type == SDL_MOUSEWHEEL) distance *= e.wheel.y > 0 ? 0.9f : 1.1f;
			}
			// Keyboard -> controller.
			if (!shot) {
				const Uint8 *k = SDL_GetKeyboardState(nullptr);
				world.input.up = k[SDL_SCANCODE_UP];
				world.input.down = k[SDL_SCANCODE_DOWN];
				world.input.left = k[SDL_SCANCODE_LEFT];
				world.input.right = k[SDL_SCANCODE_RIGHT];
				world.input.left_punch = k[SDL_SCANCODE_A];
				world.input.right_punch = k[SDL_SCANCODE_S];
				world.input.left_kick = k[SDL_SCANCODE_Z];
				world.input.right_kick = k[SDL_SCANCODE_X];
				world.input.jump = k[SDL_SCANCODE_SPACE];
				world.input.evade = k[SDL_SCANCODE_LSHIFT];
				world.input.duck = k[SDL_SCANCODE_C];
			}
			// DE_CWindow::Run: a frame once at least 20 ms of real time have passed, passing the scene
			// all of it (World::tick; the fighters update every 16 ms of scene time within). A
			// screenshot run passes exactly 20 ms a frame so it is repeatable.
			Uint64 now = SDL_GetPerformanceCounter();
			accum += double(now - last) * 1000.0 / double(SDL_GetPerformanceFrequency());
			last = now;
			uint32_t frame_ms = 0;
			if (shot) {
				frame_ms = game::World::kFrameMs;
			} else if (accum >= game::World::kFrameMs) {
				frame_ms = uint32_t(std::min(accum, 2000.0));  // a stall: at most 2 s at once
				accum = accum > 2000 ? 0 : accum - frame_ms;  // the leftover fraction carries over
			}
			if (frame_ms == 0) {
				SDL_Delay(1);
				continue;
			}
			{
				if (fr == spawn_frame) {
					spawn_all(floor0);
				}
				if (shot) {
					apply_holds(fr);
				}
				pilot_tick();
				world.tick(frame_ms);
				print_texts(fr);
				++fr;
			}
			mixer.play_requests(world, ear_of_player());
			mixer.update_music(world);
			// Scene render-tick hooks (camera mover, text fades, HUD) with the time since the last drawn frame.
			if (!shot) {
				static Uint64 last_render = SDL_GetPerformanceCounter();
				Uint64 t = SDL_GetPerformanceCounter();
				float ms = float(double(t - last_render) * 1000.0 / double(SDL_GetPerformanceFrequency()));
				last_render = t;
				game::render_tick(world, std::fmin(ms, 100.0f));
			}
			if (!shot && fr > spawn_frame) {
				for (auto *fi : fighters) {
					std::string now = state_of(*fi);
					if (now != fi->last_state) {
						printf("frame %5d  %-8s %s\n", fr, fi->name.c_str(), now.c_str());
						fflush(stdout);
						fi->last_state = now;
					}
				}
			}
			int dw = W, dh = H;
			if (!shot) SDL_GL_GetDrawableSize(win, &dw, &dh);
			glViewport(0, 0, dw, dh);
			glEnable(GL_DEPTH_TEST);
			glClearColor(0.62f, 0.55f, 0.45f, 1);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			de::Transform ft = player ? player->actor->world() : de::Transform{};
			float target[3] = {ft.t[0], ft.t[1] + fighter_radius * 0.8f, ft.t[2]};
			// Orbit relative to the fighter's facing so the camera stays behind it while it turns.
			auto facing = ft.rotate({0, 0, 1});
			float base_yaw = std::atan2(-facing[0], -facing[2]);
			if (!cam_init || shot) {
				cam_base = base_yaw;
				cam_init = true;
			}
			float dy = std::remainder(base_yaw - cam_base, 6.2831853f);
			cam_base += dy * 0.06f;  // trail behind the fighter so turning reads on screen
			float cy = cam_base + yaw;
			float eye[3] = {target[0] + distance * std::sin(cy) * std::cos(pitch), target[1] + distance * std::sin(pitch),
			                target[2] + distance * std::cos(cy) * std::cos(pitch)};
			yaw *= dragging ? 1.0f : 0.97f;  // drift back behind the fighter after orbiting
			float proj[16], view[16], vp[16];
			// The game's fight camera: the CameraActor under the player's FighterCamera
			// (BFighterCameraMover places it), with the Camera's 70.5 degree field of view (4:3
			// horizontal; the vertical angle is kept on wider screens).
			game::GameObject *cam = nullptr;
			if (!free_camera && player) {
				std::function<game::GameObject *(game::GameObject *)> find = [&](game::GameObject *o) -> game::GameObject * {
					for (auto *ch : o->children) {
						if (ch->name == "CameraActor") return ch;
						if (auto *r = find(ch)) return r;
					}
					return nullptr;
				};
				cam = find(player->actor);
			}
			if (cam && player->actor->spawned) {
				auto ct = cam->world();
				auto f = ct.rotate({0, 0, 1});
				float vfov = 2.0f * std::atan(std::tan(0.5f * 70.5f * 0.017453292f) * 0.75f) / 0.017453292f;
				glu::perspective(proj, vfov, float(dw) / float(dh), 10.0f, 100000.0f);
				float ceye[3] = {ct.t[0], ct.t[1], ct.t[2]};
				// The CameraActor's own up keeps BFighterCameraMover's rumble roll.
				auto up = ct.rotate({0, 1, 0});
				glu::look_view(view, ceye, f.data(), up.data());
				memcpy(eye, ceye, sizeof ceye);
			} else {
				glu::perspective(proj, 50.0f, float(dw) / float(dh), 2.0f, 20000.0f);
				glu::fps_view(view, eye, -cy, pitch);
			}
			glu::mul(vp, proj, view);
			// CRenderer::ConvertFromWorldToScreen for the HUD: screen fractions (y up), view depth.
			memcpy(screen_vp.data(), vp, sizeof vp);
			if (!world.world_to_screen) {
				world.world_to_screen = [&screen_vp](const std::array<float, 3> &p) {
					const float *m = screen_vp.data();
					float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
					float y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
					float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
					if (w <= 0) return std::array<float, 3>{0, 0, w};
					return std::array<float, 3>{(x / w + 1) / 2, (y / w + 1) / 2, w};
				};
			}
			// The frame's lights and their lighting owners (scene_lights.h).
			render::FrameLights frame_lights = render::collect_lights(world, fr);
			const auto &light_owners = frame_lights.owners;
			auto light_owner_of = [&](const game::GameObject *o) { return frame_lights.owner_of(o); };
			static const de::ObjectsFile *arena_file = world.data().file(arena);
			// The arena's actors that paths move (trains, cars, ...): drawn live, not baked.
			std::vector<render::ActorMeshRenderer::Item> moving_items;
			{
				std::set<uint32_t> moving;
				std::function<void(game::GameObject *)> add = [&](game::GameObject *o) {
					if (!o->file || o->file != arena_file) return;
					if (o->is_actor) moving_items.push_back({{o->file, o->index}, o->world(), light_owner_of(o)});
					for (game::GameObject *ch : o->children) add(ch);
				};
				for (game::GameObject *o : world.scene_objects()) {
					if (!o->path_moved || o->file != arena_file) continue;
					moving.insert(o->index);
					add(o);
				}
				arena_r.set_moving(moving);
				// The arena batches' lighting owners: their actors are the arena's scene objects.
				static bool owners_set = false;
				if (!owners_set && !light_owners.empty()) {
					owners_set = true;
					std::map<uint32_t, const game::GameObject *> by_index;
					for (game::GameObject *o : world.scene_objects()) {
						if (o->file == arena_file) by_index.emplace(o->index, o);
					}
					arena_r.set_light_owners([&](const std::vector<uint32_t> &chain) -> const void * {
						for (uint32_t i : chain) {
							auto it = by_index.find(i);
							if (it != by_index.end() && light_owners.count(it->second)) return it->second;
						}
						return nullptr;
					});
				}
				static const bool dbg_paths = getenv("OPENBG_DEBUG_PATHS") != nullptr;
				if (dbg_paths && fr % 120 == 0) {
					for (auto &it : moving_items) fprintf(stderr, "[path %d] %s at %.0f %.0f %.0f\n", fr, it.actor.info().name.c_str(), it.world.t[0], it.world.t[1], it.world.t[2]);
				}
			}
			render::update_animated_textures(world.image_phases);
			arena_r.draw(vp, eye);
			// Actors made at run time (pickups, jets, spawned props), where they are now: shown,
			// active, and not a fighter's own body.
			{
				std::vector<render::ActorMeshRenderer::Item> items = moving_items;
				for (game::GameObject *o : world.scene_objects()) {
					if (!o->copied || !o->is_actor || o->fighter || !o->file || o->meshes_disabled) continue;
					// The arena's own actors are the static scene ArenaRenderer draws.
					if (o->file == arena_file) continue;
					bool live = game::in_scene(*o);
					for (const game::GameObject *p = o; p && live; p = p->parent) live = p->visible;
					if (const char *sk = getenv("OPENBG_SKIP_ACTOR")) {
						for (const game::GameObject *p = o; p && live; p = p->parent) live = p->name != sk;
					}
					if (live) items.push_back({{o->file, o->index}, o->world(), light_owner_of(o)});
					static const bool dbg = getenv("OPENBG_DEBUG_ACTOR_MESHES") != nullptr;
					if (dbg && live && fr == 900) fprintf(stderr, "[actor mesh] %s (%s) under %s\n", o->name.c_str(), o->class_path.c_str(), o->parent ? o->parent->name.c_str() : "-");
				}
				actors_r.texture_version = world.texture_override_version;
				if (!getenv("OPENBG_NO_ACTORS")) actors_r.draw(world.data(), items, vp, eye);
			}
			load_renderers();
			if (fr >= spawn_frame) {
				for (auto *fi : fighters) {
					auto *a = fi->actor;
					if (!a->spawned) continue;
					render::FighterRenderer::AnimState st{a->sequence, a->anim_time, a->prelude, a->prelude_time, a->activations,
					                                      a->captured_blend, a->capture_initial.sequence, a->capture_final.sequence,
					                                      a->capture_initial.time, a->capture_final.time};
					// The skeleton as the game posed it: the validated pose plus whatever moved the
					// bone actors since (the body rotation on the Pelvis, a throw's follow).
					de::Transform body = a->world();
					std::vector<de::Transform> pose = world.current_pose(*a);
					if (!pose.empty()) st.pose = &pose;
					st.light_owner = light_owner_of(a);
					// Attachments (Gargoyle's wings): the AnimationSequenceManager under each one's root actor.
					for (const std::string &root : fi->renderer.attachment_roots()) {
						render::FighterRenderer::AnimState::Attachment as;
						std::function<game::GameObject *(game::GameObject *)> find = [&](game::GameObject *o) -> game::GameObject * {
							if (o->name == root) return o;
							for (auto *ch : o->children) {
								if (auto *r = find(ch)) return r;
							}
							return nullptr;
						};
						if (game::GameObject *r = find(a)) {
							for (auto *ch : r->children) {
								if (ch->class_path == "AnimationSequenceManager") {
									as = {ch->sequence, ch->anim_time, ch->prelude, ch->prelude_time, ch->activations};
									break;
								}
							}
						}
						st.attachments.push_back(as);
					}
					if (const game::Pilot *pl = game::pilot_of(a)) fi->renderer.set_paint(pl->colours);
					fi->renderer.draw(vp, eye, body, st);
				}
			}
			if (!getenv("OPENBG_NO_PARTICLES")) particles_r.draw(world, vp, view, eye);
			overlay.draw_world(world, vp);
			// The glow layer, added over the scene (OPENBG_NO_GLOW=1 leaves it out).
			static const bool no_glow = getenv("OPENBG_NO_GLOW") != nullptr;
			if (!no_glow) {
				glow_r.begin();
				arena_r.draw_glow(vp, eye);
				if (fr >= spawn_frame) {
					for (auto *fi : fighters) {
						if (fi->actor->spawned && fi->renderer_loaded) fi->renderer.draw_glow(vp, eye);
					}
				}
				if (!getenv("OPENBG_NO_PARTICLES")) particles_r.draw(world, vp, view, eye, true);
				glow_r.end(dw, dh);
			}
			overlay.draw(world, dw, dh, player ? player->actor : nullptr);
			if (shot) {
				// The final frame goes to `shot`; SHOTS="150,155,..." also writes <shot>-<frame>.png.
				bool last_frame = fr >= frames;
				std::string out;
				if (last_frame) {
					out = shot;
				} else if (const char *list = getenv("SHOTS")) {
					std::string l = std::string(",") + list + ",";
					if (l.find("," + std::to_string(fr) + ",") != std::string::npos) {
						out = std::string(shot) + "-" + std::to_string(fr) + ".png";
					}
				}
				if (!out.empty()) {
					std::vector<uint8_t> px(size_t(W) * H * 4);
					glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
					glu::write_png(out.c_str(), W, H, px);
					printf("frame %d: wrote %s\n", fr, out.c_str());
					for (auto *fi : fighters) {
						printf("    %-8s %s t=%.2f%s\n", fi->name.c_str(), state_of(*fi).c_str(), fi->actor->anim_time,
						       fi->actor->prelude ? " (prelude)" : "");
					}
				}
				if (last_frame) {
					break;
				}
				continue;
			}
			SDL_GL_SwapWindow(win);
		}
		print_report(world);
		SDL_GL_DeleteContext(ctx);
		SDL_DestroyWindow(win);
		SDL_Quit();
	} catch (const std::exception &e) {
		fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
	return 0;
}
