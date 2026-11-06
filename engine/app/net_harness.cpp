// openbg-net: a network match between a server and a client in one process, through the loopback
// link (net/link.h) with a simulated delay, jitter and loss. No window; both machines run headless
// in lockstep, 20 ms a frame.
//
//   openbg-net <Data dir> [--server Katana[:Elizabeth]] [--client Jaguar[:Lucas]] [--arena Desert]
//              [--ping ms] [--jitter ms] [--loss 0..1] [--frames N] [--wins N] [--budget bytes/s]
//              [--difficulty rookie..master] [--server-user] [--client-ai] [--every N]
//
// Both run the game's own Last Man Standing session with the same choices: slot 0 is the server's
// (its pilot's AI drives it unless --server-user, then HOLD scripts it), slot 1 the client's (CLIENT_HOLD
// scripts it, or its pilot's AI with --client-ai). It prints both machines' view of each fighter,
// the position error of the client's copy against the server's, the corrections, rollbacks and bytes
// per second, and the messages each machine shows.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "de/platform.h"
#include "game/netplay.h"
#include "game/replay.h"
#include "game/runtime.h"
#include "game/session.h"
#include "game_view.h"
#include "net/link.h"

namespace {

std::vector<std::tuple<std::string, int, int>> parse_holds(const char *h) {
	std::vector<std::tuple<std::string, int, int>> out;
	if (!h) return out;
	std::string spec = h;
	for (size_t p = 0; p < spec.size();) {
		size_t q = spec.find(',', p);
		std::string item = spec.substr(p, q == std::string::npos ? std::string::npos : q - p);
		int a = 0, b = 0;
		char key[32] = {};
		if (sscanf(item.c_str(), "%31[a-z_]@%d-%d", key, &a, &b) == 3) out.emplace_back(key, a, b);
		if (q == std::string::npos) break;
		p = q + 1;
	}
	return out;
}

const std::map<std::string, bool game::Input::*> kKeys = {
    {"up", &game::Input::up}, {"down", &game::Input::down}, {"left", &game::Input::left}, {"right", &game::Input::right},
    {"jump", &game::Input::jump}, {"duck", &game::Input::duck}, {"evade", &game::Input::evade},
    {"left_punch", &game::Input::left_punch}, {"right_punch", &game::Input::right_punch},
    {"left_kick", &game::Input::left_kick}, {"right_kick", &game::Input::right_kick}};

std::optional<de::Guid> environment_description(game::World &world, const std::string &arena) {
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
	for (auto *file : {ad, af}) {
		for (auto &o : file->objects()) {
			if (o.class_path != "Description\\Environment") continue;
			auto c = file->object_data(o).contents;
			if (std::search(c.begin(), c.end(), root.bytes, root.bytes + 16) != c.end()) return o.guid;
		}
	}
	return std::nullopt;
}

struct Side {
	std::string name;
	std::unique_ptr<game::World> world;
	std::unique_ptr<app::GameView> view;
	std::unique_ptr<game::NetSession> net;
	game::SessionState *session = nullptr;
	uint32_t rand = 1;
	std::vector<std::tuple<std::string, int, int>> holds;
	std::map<const game::GameObject *, std::string> shown;
	game::NetCounters last{};
	uint64_t last_bytes = 0;
};

de::ObjectRef find(game::World &w, const std::string &file, const std::string &cls, const std::string &obj) {
	if (auto *ff = w.data().file(file)) {
		for (uint32_t i = 0; i < ff->objects().size(); ++i) {
			if (ff->objects()[i].class_path == cls && ff->objects()[i].name == obj) return {ff, i};
		}
	}
	return {};
}

// Both machines build the same match: Last Man Standing, the score limit, the arena, each slot's
// robot and pilot. The server's slot 1 belongs to a remote client; the client's slot 0 to the
// server's player, and its own slot 1 is locally controlled (what the server tells it in phase 2).
void setup(Side &s, game::NetRole role, const std::string &arena, int wins, int difficulty, bool server_user, bool client_ai,
           const std::vector<std::pair<std::string, std::string>> &picks, game::NetConfig cfg) {
	game::World &w = *s.world;
	w.show_rules = [&w](game::GameObject *rules) {
		auto &r = w.rules_displays[rules];
		r.shown = r.displayed = r.authorized = true;
	};
	w.ai_difficulty = difficulty;
	s.view = std::make_unique<app::GameView>(w);
	s.view->wire_session();
	de::ObjectRef master = find(w, "Session", "SessionModifier\\StandardSessionMaster", "LastManStandingMaster");
	if (!master) throw std::runtime_error("LastManStandingMaster not found");
	auto &ss = game::start_session(w, {master});
	s.session = &ss;
	std::string wins_name = wins == 1 ? "One Win" : std::to_string(wins) + " Wins";
	if (de::ObjectRef d = find(w, "Session", "Description\\SessionModifier", wins_name)) {
		if (game::GameObject *tpl = w.global_object(d)) {
			if (game::GameObject *m = game::new_resource(w, w.copy_tree(*tpl), nullptr)) game::add_session_modifier(w, m);
		}
	}
	auto env = environment_description(w, arena);
	if (!env) throw std::runtime_error("no environment description for " + arena);
	ss.ui_choices["Choose Environment"] = *env;
	s.net = std::make_unique<game::NetSession>(w, role, cfg);
	game::begin_session(w);
	auto pick = [&](size_t slot, const char *handle, const de::ObjectRef &desc) {
		if (!desc || slot >= ss.slots.size()) return;
		uint32_t g = w.make_instance("CObject::CGUID", 1);
		memcpy(w.memory().ptr(g, 16), desc.info().guid.bytes, 16);
		uint32_t e = game::new_event(w, handle);
		w.memory().set<uint32_t>(e + 8, g);
		w.send_event(ss.slots[slot]->obj, e);
	};
	for (size_t i = 0; i < picks.size(); ++i) {
		de::ObjectRef fd = find(w, picks[i].first, "Description\\Creature\\Fighter", picks[i].first);
		if (!fd) fd = w.data().find("Description\\Creature\\Fighter", picks[i].first);
		pick(i, "SessionSlotSelectCreature", fd);
		pick(i, "SessionSlotSelectPilot", find(w, "Pilots", "Description\\Pilot", picks[i].second));
		bool mine = (role == game::NetRole::Server) == (i == 0);
		auto &cl = game::add_client(w, mine, picks[i].second);
		cl.local = mine;
		if (role == game::NetRole::Client && mine && i < ss.slots.size()) ss.slots[i]->local = true;
	}
	w.all_ai = role == game::NetRole::Server ? !server_user : client_ai;
}

game::GameObject *slot_creature(Side &s, size_t i) {
	return s.session && i < s.session->slots.size() ? s.session->slots[i]->creature : nullptr;
}

float health(Side &s, game::GameObject *f) {
	if (!f) return -1;
	for (game::GameObject *ch : f->children) {
		if (!ch->brain || ch->brain->cls.name != "PFighterStats") continue;
		std::vector<uint8_t> args(4);
		if (!ch->brain->call(*s.world, "float NormalizedHealth()", args)) return -1;
		float h;
		memcpy(&h, args.data(), 4);
		return h;
	}
	return -1;
}

std::string state_of(game::GameObject *f) {
	auto *m = f && f->brain && f->brain->exec ? f->brain->exec->current_state() : nullptr;
	if (!m) return "-";
	std::string n = m->cm->signature;
	size_t colon = n.rfind(':');
	return colon == std::string::npos ? n : n.substr(colon + 1);
}

}  // namespace

int main(int argc, char **argv) {
	de::init_platform();
	if (argc < 2) {
		fprintf(stderr, "usage: %s <Data dir> [--server Katana[:Elizabeth]] [--client Jaguar[:Lucas]] [--arena Desert] [--ping ms] [--jitter ms] [--loss p] [--frames N]\n", argv[0]);
		return 2;
	}
	std::string data = argv[1], arena = "Desert";
	std::pair<std::string, std::string> server_pick{"Katana", "Elizabeth"}, client_pick{"Jaguar", "Lucas"};
	float ping = 0, jitter = 0, loss = 0;
	int frames = 3000, wins = 2, difficulty = 0, every = 50, budget = 16384;
	bool server_user = false, client_ai = false;
	auto split = [](const std::string &v, std::pair<std::string, std::string> &out) {
		size_t c = v.find(':');
		out.first = v.substr(0, c);
		if (c != std::string::npos) out.second = v.substr(c + 1);
	};
	for (int i = 2; i < argc; ++i) {
		std::string a = argv[i];
		auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
		if (a == "--server") split(next(), server_pick);
		else if (a == "--client") split(next(), client_pick);
		else if (a == "--arena") arena = next();
		else if (a == "--ping") ping = float(atof(next().c_str()));
		else if (a == "--jitter") jitter = float(atof(next().c_str()));
		else if (a == "--loss") loss = float(atof(next().c_str()));
		else if (a == "--frames") frames = atoi(next().c_str());
		else if (a == "--wins") wins = atoi(next().c_str());
		else if (a == "--budget") budget = atoi(next().c_str());
		else if (a == "--every") every = std::max(1, atoi(next().c_str()));
		else if (a == "--server-user") server_user = true;
		else if (a == "--client-ai") client_ai = true;
		else if (a == "--difficulty") {
			std::string d = next();
			const char *names[] = {"rookie", "amateur", "veteran", "champion", "world-champion", "master"};
			for (int k = 0; k < 6; ++k) {
				if (d == names[k]) difficulty = k;
			}
		}
	}
	try {
		uint32_t seed = getenv("OPENBG_SEED") ? uint32_t(strtoul(getenv("OPENBG_SEED"), nullptr, 0)) : 1;
		Side server, client;
		server.name = "server", client.name = "client";
		server.rand = client.rand = seed;
		fprintf(stderr, "Loading game data twice from %s...\n", data.c_str());
		server.world = std::make_unique<game::World>(data);
		client.world = std::make_unique<game::World>(data);
		server.holds = parse_holds(getenv("HOLD"));
		client.holds = parse_holds(getenv("CLIENT_HOLD"));
		uint32_t clock = 0;
		net::LoopbackLink link({ping / 2, jitter, loss}, [&clock] { return clock; }, seed);
		std::vector<std::pair<std::string, std::string>> picks = {server_pick, client_pick};
		game::NetConfig cfg;
		cfg.budget = budget;
		game::use_rand_state(&server.rand);
		setup(server, game::NetRole::Server, arena, wins, difficulty, server_user, client_ai, picks, cfg);
		game::use_rand_state(&client.rand);
		setup(client, game::NetRole::Client, arena, wins, difficulty, server_user, client_ai, picks, cfg);
		server.net->add_peer(&link.a());
		client.net->add_peer(&link.b());
		printf("ping %.0f ms (one way %.0f), jitter %.0f ms, loss %.0f%%, budget %d B/s\n", ping, ping / 2, jitter, loss * 100, budget);

		double err_sum[2] = {0, 0}, err_max[2] = {0, 0};
		uint64_t err_n[2] = {0, 0};
		for (int fr = 0; fr < frames; ++fr) {
			clock = uint32_t(fr) * game::World::kFrameMs;
			for (Side *s : {&server, &client}) {
				game::use_rand_state(&s->rand);
				game::World &w = *s->world;
				w.input = {};
				for (auto &[k, a, b] : s->holds) {
					if (fr >= a && fr <= b && kKeys.count(k)) w.input.*kKeys.at(k) = true;
				}
				// The client's creature: what the server takes states for, and what both measure distance from.
				if (!s->net->peers()[0].creature) s->net->peers()[0].creature = slot_creature(*s, 1);
				w.tick(s == &client ? s->net->frame_ms(game::World::kFrameMs) : game::World::kFrameMs);
				s->net->update();
				for (auto &[obj, t] : w.texts) {
					auto &last = s->shown[obj];
					if (t.text != last && !t.text.empty()) printf("frame %5d  %s [screen] %s\n", fr, s->name.c_str(), t.text.c_str());
					last = t.text;
				}
			}
			for (size_t i = 0; i < 2; ++i) {
				game::GameObject *a = slot_creature(server, i), *b = slot_creature(client, i);
				if (!a || !b || !a->spawned || !b->spawned) continue;
				auto pa = a->local.t, pb = b->local.t;
				double e = std::sqrt(double((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) + (pa[2] - pb[2]) * (pa[2] - pb[2])));
				err_sum[i] += e, err_n[i]++;
				err_max[i] = std::max(err_max[i], e);
			}
			if (const char *dm = getenv("OPENBG_DEBUG_NET"); dm && fr == atoi(dm)) {
				for (Side *s : {&server, &client}) {
					for (game::GameObject *r : s->world->scene_roots) printf("%s root %s (%s) in scene %d\n", s->name.c_str(), r->name.c_str(), r->class_path.c_str(), game::in_scene(*r));
					for (auto &m : game::net_managers(*s->world)) {
						std::string shown;
						for (char ch : m.id) shown += ch >= 32 && ch < 127 ? ch : '.';
						game::NetState st;
						bool has = game::net_newest_state(*s->world, m.owner, st);
						printf("%s manager %s (%s) id %u up %d down %d controlled %d newest %d/%u a%u s%u f%x: %s\n", s->name.c_str(), m.owner->name.c_str(), m.owner->class_path.c_str(), m.server_id, m.up, m.down,
						       game::locally_controlled(*s->world, m.owner), has, st.time, st.authority, st.significance, st.flags, shown.c_str());
					}
				}
			}
			if (fr % every == every - 1) {
				game::use_rand_state(&server.rand);
				for (size_t i = 0; i < 2; ++i) {
					game::GameObject *a = slot_creature(server, i), *b = slot_creature(client, i);
					if (!a || !b) continue;
					auto pa = a->local.t, pb = b->local.t;
					double e = std::sqrt(double((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) + (pa[2] - pb[2]) * (pa[2] - pb[2])));
					game::use_rand_state(&server.rand);
					float ha = health(server, a);
					game::use_rand_state(&client.rand);
					float hb = health(client, b);
					printf("frame %5d  slot %zu %-7s server (%7.1f %6.1f %7.1f) %3.0f%% %-28s client (%7.1f %6.1f %7.1f) %3.0f%% %-28s err %6.1f\n", fr, i,
					       a->name.c_str(), pa[0], pa[1], pa[2], ha * 100, state_of(a).c_str(), pb[0], pb[1], pb[2], hb * 100, state_of(b).c_str(), e);
				}
				double secs = every * game::World::kFrameMs / 1000.0;
				for (Side *s : {&server, &client}) {
					auto &c = game::net_counters(*s->world);
					auto &l = s->last;
					uint64_t bytes = s->net->bytes_sent();
					printf("frame %5d  %s: in %.0f/s inserted %.0f/s rejected %.0f/s held %.0f/s corrections %.0f/s rollbacks %.0f/s (%.0f ticks/s) registered %.0f/s, sent %.0f B/s, lag %.0f ms, ping %.0f ms, slew %d\n",
					       fr, s->name.c_str(), (c.received - l.received) / secs, (c.inserted - l.inserted) / secs, (c.rejected - l.rejected) / secs,
					       (c.held - l.held) / secs, (c.corrections_in - l.corrections_in) / secs, (c.rollbacks - l.rollbacks) / secs,
					       (c.rollback_ticks - l.rollback_ticks) / secs, (c.registered - l.registered) / secs, (bytes - s->last_bytes) / secs, s->net->lag_ms,
					       s->net->peers()[0].ping, s->net->slew_pending);
					l = c;
					s->last_bytes = bytes;
				}
			}
		}
		double secs = frames * game::World::kFrameMs / 1000.0;
		printf("summary: ping %.0f ms jitter %.0f loss %.2f over %.0f s\n", ping, jitter, loss, secs);
		for (size_t i = 0; i < 2; ++i) {
			game::GameObject *a = slot_creature(server, i);
			printf("summary: slot %zu %s position error mean %.2f max %.2f (%llu samples)\n", i, a ? a->name.c_str() : "-",
			       err_n[i] ? err_sum[i] / double(err_n[i]) : 0.0, err_max[i], (unsigned long long)err_n[i]);
		}
		for (Side *s : {&server, &client}) {
			auto &c = game::net_counters(*s->world);
			printf("summary: %s received %llu inserted %llu same %llu rejected %llu held %llu forced %llu corrections %llu rollbacks %llu (%.1f/s, %llu ticks) registered %llu, sent %llu states %.0f B/s, clock jumps %llu\n",
			       s->name.c_str(), (unsigned long long)c.received, (unsigned long long)c.inserted, (unsigned long long)c.same, (unsigned long long)c.rejected,
			       (unsigned long long)c.held, (unsigned long long)c.held_forced, (unsigned long long)c.corrections_in, (unsigned long long)c.rollbacks,
			       c.rollbacks / secs, (unsigned long long)c.rollback_ticks, (unsigned long long)c.registered, (unsigned long long)s->net->states_sent,
			       s->net->bytes_sent() / secs, (unsigned long long)s->net->clock_jumps);
		}
		for (Side *s : {&server, &client}) {
			for (auto &l : s->world->log) {
				if (l.find("No pickups") == std::string::npos) printf("%s log: %s\n", s->name.c_str(), l.c_str());
			}
		}
	} catch (const std::exception &e) {
		fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
	return 0;
}
