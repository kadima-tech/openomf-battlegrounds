// The observer camera's autopilot (see observer.h). Offsets in comments are DE_CObserveCommand's
// (0x154 bytes, ObjInitContents 0x15021e90) unless another class is named.
#include "game/observer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "game/session.h"
#include "game/simple_physics.h"

namespace game {
namespace {

using Vec = std::array<float, 3>;
Vec add(const Vec &a, const Vec &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec sub(const Vec &a, const Vec &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec mul(const Vec &a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float len(const Vec &a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
// DE_TCoord3D::RotateY.
Vec rotate_y(const Vec &v, float a) {
	float c = std::cos(a), s = std::sin(a);
	return {v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c};
}
Vec pos(const GameObject *o) { return o->world().t; }

// DE_CBox (+0x104 min, +0x110 max).
struct Box {
	Vec mn{1e6f, 1e6f, 1e6f}, mx{-1e6f, -1e6f, -1e6f};
	void reset() { *this = Box{}; }
	void grow(const Vec &p) {
		for (size_t k = 0; k < 3; ++k) mn[k] = std::min(mn[k], p[k]), mx[k] = std::max(mx[k], p[k]);
	}
	Vec center() const { return mul(add(mn, mx), 0.5f); }
	Vec size() const { return sub(mx, mn); }
};

// Core MATH::Rand (0x70025e70 float, 0x70025ea0 int) on the shared rand() stream.
struct Rand {
	int next() { return crt_rand(); }
	float f(float x) { return float(next()) * x / 32768.0f; }
	int i(int n) { return (next() * n) >> 15; }
};
Rand g_rand;

enum class Exec { None, Propulsion, MouseAxis, Teleport };

// Moves the actor's Z axis towards a world point (DE_CActor::PointZ with a position).
void point_z_at(GameObject &a, const Vec &target) {
	Vec z = sub(target, pos(&a));
	float l = len(z);
	if (l <= 0) return;
	z = mul(z, 1 / l);
	Vec up = std::fabs(z[1]) > 0.99f ? Vec{1, 0, 0} : Vec{0, 1, 0};
	Vec x{up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
	x = mul(x, 1 / len(x));
	Vec y{z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
	if (a.parent) {
		de::Transform pw = a.parent->world();
		x = pw.inverse_rotate(x), y = pw.inverse_rotate(y), z = pw.inverse_rotate(z);
	}
	a.local.r = {x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]};
}

}  // namespace

// DE_CCommand: MoveTo (type 2), LookAt (4) and Observe (8).
struct Command {
	uint32_t type = 0;
	bool is_sub = false;  // +0x60: a sub-command, never updated by the manager
	Vec point{0, 0, 0};  // MoveTo Destination / LookAt Target (+0x70)
	float required = 0;  // MoveTo RequiredDistance (+0x80)
	virtual ~Command() = default;
};

struct ObserveCommand;

struct ObserverSystem::Manager {
	struct Info {  // CControllerInfo
		GameObject *controller = nullptr;
		std::vector<Exec> executors;
		Exec running = Exec::None;   // +0x28
		Command *command = nullptr;  // +0x38
	};
	GameObject *creature = nullptr;
	std::vector<Info> infos;
	std::vector<std::unique_ptr<Command>> list;  // the received commands, in arrival order
	bool sort = true, reassign = false;          // +0xa0, +0xa4
	uint32_t last_ms = UINT32_MAX;

	bool listed(const Command *c) const {
		for (auto &p : list) {
			if (p.get() == c) return true;
		}
		return false;
	}
	void receive(std::unique_ptr<Command> c) {
		list.push_back(std::move(c));
		sort = true;
	}
	void receive_again() { sort = true; }  // ReceiveCommand of a listed command: only re-sorted
	void forget_all() {
		list.clear();
		for (auto &i : infos) i.command = nullptr, i.running = Exec::None;
		reassign = true;
	}
};

namespace {

// IsHandler (DOP 0x150294e0, MouseAxisNavigation 0x15029e80, Teleport 0x1502c4a0).
bool handles(Exec e, const GameObject &ctrl) {
	switch (e) {
	case Exec::Propulsion: return ctrl.is("CreatureController\\JoystickInputManager");  // Num2DOrientations 1
	case Exec::MouseAxis: return ctrl.is("CreatureController\\MouseAxis");
	case Exec::Teleport: return true;
	default: return false;
	}
}

}  // namespace

struct ObserveCommand : Command {
	ObserveCommand() { type = 8; }
	World *w = nullptr;
	GameObject *creature = nullptr;  // +0x40
	std::vector<GameObject *> destinations, watched, ignored;  // +0x6c, +0x80, +0x94
	Command *move = nullptr, *look = nullptr;                  // +0xa0, +0xa8
	int drama = 0;                     // +0xb0 (only 0 is reachable: g_DramaticTimeChanges is 0)
	int placement = 1;                 // +0xb4: 0 orbit, 1 vantage point, 2 follow watched[0]
	float threshold = 0.5f;            // +0xcc
	float interest = 0;                // +0xd0
	float orbit_angle = 0, orbit_radius = 70, orbit_height = 17.5f, orbit_speed = 0;  // +0xe0..+0xec
	float watch_diagonal = 0;          // +0xf0
	float last_major = 0;              // +0xf4
	float extent = 0;                  // +0xf8
	bool gather = true;                // +0xfc
	GameObject *vantage = nullptr;     // +0x100
	Box box;                           // +0x104
	float evaluate_timer = 0;          // +0x11c
	float major_timer = 0;             // +0x120
	// +0x124 is never initialised in the original (operator new, not zeroed); 0 is what a block
	// from fresh zeroed pages holds, and keeps step 9 to occluded views and the quotes path.
	float best_quality = 0;
	int sample = 0;                    // +0x128
	float quality = 0;                 // +0x12c
	float fov = 60, fov_rate = 0, framing = 1;  // +0x130, +0x134, +0x138
	bool interesting = true;           // +0x140
	Vec follow{0, 0, 0};               // +0x144
	bool quotes_seen = false;          // +0x150
	GameObject *lens = nullptr;        // FindCamera(creature.Camera(0))

	GameObject *find_lens() {
		if (lens && w->object(lens->handle) == lens) return lens;
		lens = nullptr;
		if (GameObject *cam = creature_camera(*w, creature, 0)) {
			for (GameObject *ch : cam->children) {
				if (ch->class_path == "Camera") lens = ch;
			}
		}
		return lens;
	}

	bool live(GameObject *o) const { return o && w->object(o->handle) == o && in_scene(*o); }
	// GlobalInterest (creature vtable +0x158, OMF 0x3502ae40): a read refreshes a fighter's cached
	// value once 300 ms have passed, so watching re-phases what the fighters' own AI reads.
	float interest_of(GameObject *o) const { return w->global_interest_of(o); }
	bool blocked(const Vec &a, const Vec &b) const { return w->segment_blocked(a, b); }

	// PositionWatchQuality (0x150223f0): lower is better, 1 at best.
	float watch_quality(const Vec &p) const {
		float d = len(sub(p, box.center()));
		float ideal = len(box.size()) * 0.9f + 40;
		float q = d > ideal ? (d - ideal) / ideal + 1 : 2 * (ideal - d) / ideal + 1;
		for (GameObject *c : watched) {
			Vec cp = add(pos(c), {0, 15, 0});
			if (blocked(p, cp)) q *= 2;
			if (blocked(cp, p)) q *= 5;
		}
		return q;
	}

	// EvaluateCreaturesBeingWatched (0x15022f80).
	void evaluate() {
		if (!interesting) return;
		ignored.clear();
		std::vector<GameObject *> list;
		for (GameObject *o : w->scene_objects()) {
			if (o->is("Actor\\Creature") && in_scene(*o) && !in_gui(*o)) list.push_back(o);
		}
		float max = 0;
		GameObject *top = nullptr;
		for (size_t i = list.size(); i-- > 0;) {
			float gi = interest_of(list[i]);
			if (gi < threshold) {
				ignored.push_back(list[i]);
				list.erase(list.begin() + long(i));
			} else if (gi > max) {
				max = gi, top = list[i];
			}
		}
		if (!top) return;
		float total = 0;
		for (GameObject *c : list) total += len(sub(pos(c), pos(top)));
		float radius = std::min(total / float(list.size()) * 2.1f, extent * 0.4f);
		std::vector<GameObject *> near;
		for (GameObject *c : list) {
			if (len(sub(pos(c), pos(top))) > radius) ignored.push_back(c);
			else near.push_back(c);
		}
		watched = near;
		static const bool debug = getenv("OPENBG_DEBUG_OBSERVER") != nullptr;
		if (debug) {
			fprintf(stderr, "[observer %u] evaluate:", w->frame());
			for (GameObject *c : watched) fprintf(stderr, " %s#%x %.0f", c->name.c_str(), c->handle, interest_of(c));
			fprintf(stderr, "\n");
		}
		evaluate_timer = 10000;
		major_timer = -3000;
	}

	// MajorMove (0x15022780).
	void major_move(ObserverSystem::Manager &m) {
		evaluate();
		if (gather) {
			uint32_t e = new_event(*w, "GenericEventGatherTeleportDestinationActors");
			w->send_event(w->create("Scene", "Scene"), e);
			gather = false;
			Box b;
			b.mn = {999999.9f, 999999.9f, 999999.9f}, b.mx = {-999999.9f, -999999.9f, -999999.9f};
			for (uint32_t h : w->event_actors[e]) {
				if (GameObject *a = w->object(h)) {
					destinations.push_back(a);
					b.grow(pos(a));
				}
			}
			w->event_actors.erase(e);
			extent = len(b.size());
			if (destinations.empty()) gather = true;
		}
		vantage = nullptr;
		float best = 9999999.0f;
		for (GameObject *d : destinations) {
			float q = watch_quality(pos(d));
			if (q < best) best = q, vantage = d;
		}
		if (vantage) {
			Vec v = sub(pos(vantage), box.center());
			orbit_angle = std::atan2(v[0], v[2]);
			orbit_height = v[1];
			orbit_radius = std::sqrt(v[0] * v[0] + v[2] * v[2]);
			if (orbit_radius < 25) orbit_radius = 25;
		} else {
			orbit_angle = float(g_rand.i(360));  // degrees taken as radians, as the original does
			orbit_height = 15;
			orbit_radius = float(g_rand.next()) * 25 / 32768 + 25;
		}
		orbit_speed = float(g_rand.next()) * 0.001f / 32768 - 0.0005f;
		if (orbit_radius > 150) orbit_speed /= orbit_radius / 150;
		m.reassign = true;
		for (GameObject *p : w->simple_physics->physics_of(*creature)) {
			if (p->class_path == "Physics\\Simple") w->simple_physics->state(*p).momentum = {0, 0, 0};
		}
		last_major = float(uint32_t(w->scene_ms()));
		major_timer = 15000;
		placement = g_rand.i(2);
		if (!interesting) {
			g_rand.i(3);
			placement = 2;
		}
		fov_rate = (g_rand.f(1.5f) + 1) * 0.0006f;
		framing = g_rand.f(0.15f) + 1;
		fov = std::atan(len(box.size()) / orbit_radius) * 57.29578f * framing * 1.2f;
		framing *= 1.1f;
		if (placement == 2) {
			framing = 0.7f, fov_rate = 0, fov = 60;
			follow = {-20, float(g_rand.i(15) + 8), -40};
			follow = mul(follow, 1.1f);
			if (g_rand.i(2) == 0) follow[0] = -follow[0];
			if (watched.size() == 1) {
				follow = rotate_y(follow, 3.141594f);
			} else if (watched.size() == 2 && len(sub(pos(watched[1]), pos(watched[0]))) < 100) {
				follow = rotate_y(follow, g_rand.i(2) != 0 ? -1.570797f : 1.570797f);
			}
		} else if (g_rand.i(2) != 0) {
			fov += g_rand.f(20.0f);
			if (fov > 80) fov = 80;
		}
	}

	// Update (0x15023400).
	void update(ObserverSystem::Manager &m, float delta) {
		if (!live(creature)) return;
		auto prune = [&](std::vector<GameObject *> &v) { v.erase(std::remove_if(v.begin(), v.end(), [&](GameObject *o) { return !live(o); }), v.end()); };
		prune(watched), prune(ignored), prune(destinations);
		if (vantage && !live(vantage)) vantage = nullptr;
		float h = 15;
		GameObject *scene = w->create("Scene", "Scene");
		World::ObjectValue *quotes = w->object_value(scene, "QuotesInProgress");
		if ((quotes && quotes->bits) || quotes_seen) {
			h = 25;
			if (!quotes_seen) quotes_seen = true, major_timer = -50000, last_major = -10000;
		}
		if (!move) {
			auto c = std::make_unique<Command>();
			c->type = 2, c->required = 20, c->is_sub = true;
			move = c.get();
			m.receive(std::move(c));
		}
		if (!look) {
			auto c = std::make_unique<Command>();
			c->type = 4, c->is_sub = true;
			look = c.get();
			m.receive(std::move(c));
		}
		if (!creature->user_controlled) autopilot(m, delta, h);
		if (GameObject *l = find_lens()) {
			float want = std::atan(len(box.size()) / orbit_radius) * 57.29578f * framing * 1.2f;
			fov += (want - fov) * fov_rate * delta;
			l->camera_fov = creature->user_controlled ? 60 : fov;
		}
	}

	void autopilot(ObserverSystem::Manager &m, float delta, float h) {
		bool snap = false, reissue = false;
		float now = float(uint32_t(w->scene_ms()));
		evaluate_timer -= delta;
		if (evaluate_timer < 0) evaluate();
		// The pre- and post-increment sample only odd destinations, as the original does.
		++sample;
		if (size_t(sample) < destinations.size()) {
			float q = watch_quality(pos(destinations[size_t(sample)]));
			if (q < best_quality) best_quality = q;
			++sample;
		} else {
			sample = 0;
		}
		Vec me = pos(creature);
		quality = watch_quality(me);
		major_timer -= delta;
		box.reset();
		for (GameObject *c : watched) {
			Vec p = add(pos(c), {0, h, 0});
			box.grow(p);
			if (blocked(me, p)) major_timer -= 5 * delta;
			if (blocked(p, me)) major_timer -= 5 * delta;
		}
		if (watched.empty()) {
			placement = 1;
			box.grow({0, 0, 0});
		}
		Vec t{0, 0, 0};
		if (placement == 0 || placement == 1) {
			t = box.center();
			look->point = t;
		} else if (placement == 2) {
			GameObject *w0 = watched[0];
			float k = len(sub(box.center(), pos(w0)));
			t = add(pos(w0), w0->world().rotate({0, h, 0.8f * k}));
			look->point = t;
			move->required = 10;
		}
		float diag = len(box.size());
		if (diag < 50) {
			Vec c = box.center();
			box.grow(sub(c, {15, 15, 15}));
			box.grow(add(c, {15, 15, 15}));
			diag = len(box.size());
		}
		watch_diagonal = diag;
		float r = std::clamp((len(sub(t, me)) + diag) * 0.05f, 5.0f, 200.0f);
		t[1] += r;
		// Lower quality is better, yet it moves when the current one beats the best sampled.
		if (major_timer < 0 && std::fabs(now - last_major) > 2000 && (1 + 0.5f * (major_timer / 15000)) * quality < best_quality) {
			major_move(m);
			snap = reissue = true;
		}
		float s = 0;
		for (GameObject *c : watched) s += interest_of(c);
		for (GameObject *c : ignored) s += interest_of(c);
		if (drama == 0 && interest != s && std::fabs(interest - s) > 0.4f * s) {
			if (now - last_major > 2000 && interest < s) {
				major_move(m);
				reissue = true;
			}
			interest = s;
		}
		interest -= delta * 1e-5f;
		Vec p = t;
		if (placement == 0) {
			orbit_angle += delta * orbit_speed / (orbit_radius * 0.001f + 1);
			p = add(t, rotate_y({0, orbit_height, orbit_radius}, orbit_angle));
		} else if (placement == 1) {
			p = vantage ? pos(vantage) : t;
		} else if (placement == 2 && !watched.empty()) {
			p = add(pos(watched[0]), watched[0]->world().rotate(follow));
		}
		if (snap) {
			creature->local.t = p;
			point_z_at(*creature, box.center());
			if (GameObject *l = find_lens()) l->camera_fov = fov;
		}
		move->point = p;
		if (reissue) m.receive_again();
		static const bool debug = getenv("OPENBG_DEBUG_OBSERVER") != nullptr;
		if (debug && (snap || reissue || w->frame() % 50 == 0)) {
			Vec c = box.center();
			fprintf(stderr, "[observer %u] t %.0f mode %d watched %zu ignored %zu dests %zu%s%s at (%.0f %.0f %.0f) to (%.0f %.0f %.0f) box (%.0f %.0f %.0f) diag %.0f fov %.1f timer %.0f q %.2f S %.1f\n",
			        w->frame(), now, placement, watched.size(), ignored.size(), destinations.size(), snap ? " SNAP" : "", reissue ? " MOVE" : "",
			        me[0], me[1], me[2], p[0], p[1], p[2], c[0], c[1], c[2], diag, fov, major_timer, quality, s);
		}
	}
};

namespace {

// Confidence: DOP 0x15029490, MouseAxisNavigation 0x15029e10, Teleport 0x1502c390.
float confidence(Exec e, const GameObject &ctrl, const GameObject &creature, const Command &cmd) {
	switch (e) {
	case Exec::Propulsion: return ctrl.is("CreatureController\\JoystickInputManager") && cmd.type == 2 ? 1.0f : 0.0f;
	case Exec::MouseAxis:
		if (!ctrl.is("CreatureController\\MouseAxis")) return 0;
		return cmd.type == 2 ? 0.5f : cmd.type == 4 ? 1.0f : 0.0f;
	case Exec::Teleport:
		if (ctrl.is("CreatureController\\MouseAxis") || cmd.type != 2) return 0;
		return len(sub(pos(&creature), cmd.point)) * 0.01f;
	default: return 0;
	}
}

std::array<float, 3> creature_momentum(World &w, GameObject &creature) {
	for (GameObject *p : w.simple_physics->physics_of(creature)) {
		if (p->class_path == "Physics\\Simple") return w.simple_physics->state(*p).momentum;
	}
	return {0, 0, 0};
}

void set_float(World &w, GameObject &o, const char *name, float v) {
	uint32_t bits;
	memcpy(&bits, &v, 4);
	w.values[&o][name].bits = bits;
}

// DE_CDigitalOrientationPropulsionExecutor::Tick (0x15029500): the local offset to the
// destination, less what the momentum will still carry (0.5 / 0.0004 x the local momentum) and
// 0.9 x the required distance, as the PropelOffsetX/Y/Z values; the 8-way pad orientation.
void propulsion(World &w, GameObject &creature, GameObject &ctrl, const Command &cmd) {
	if (cmd.type != 2) return;
	float eps = 0.9f * cmd.required;
	de::Transform cw = creature.world();
	Vec p = cw.inverse_rotate(sub(cmd.point, cw.t));
	Vec a = mul(cw.inverse_rotate(creature_momentum(w, creature)), 0.5f / 0.0004f);
	for (size_t k = 0; k < 3; ++k) {
		if (std::fabs(p[k] - a[k]) < eps) p[k] = 0;
	}
	float l = len(p);
	p = l <= eps ? Vec{0, 0, 0} : mul(p, (l - eps) / l);
	set_float(w, creature, "PropelOffsetX", p[0]);
	set_float(w, creature, "PropelOffsetY", p[1]);
	set_float(w, creature, "PropelOffsetZ", p[2]);
	auto axis = [eps](float v, float m) {
		int c = v - 0.5f * m > eps ? 0 : v - 0.5f * m < -eps ? 2 : 1;
		if (v - m < -eps) c = 2;
		else if (v - m > eps) c = 0;
		return c;
	};
	int zc = axis(p[2], a[2]), xc = axis(p[0], a[0]);
	static const uint8_t table[3][3] = {{7, 6, 5}, {0, 8, 4}, {1, 2, 3}};  // [xc][zc]
	ctrl.pad_orientation = table[xc][zc];
}

// DE_CMouseAxisNavigationExecutor::DeltaTick / Point (0x1502a190 / 0x15029ea0): the yaw and
// pitch to the target in degrees, set (not added) as the mouse controller's analog axes.
void mouse_axis(GameObject &creature, GameObject &ctrl, const Command &cmd) {
	if (cmd.type != 4 || !ctrl.is("CreatureController\\MouseAxis")) return;
	de::Transform cw = creature.world();
	Vec d = cw.inverse_rotate(sub(cmd.point, cw.t));
	float l = len(d);
	if (l <= 0) return;
	d = mul(d, 1 / l);
	if (d[1] > 0.8f || d[1] < -0.8f) {
		d[1] = d[1] > 0 ? 0.8f : -0.8f;
		d = mul(d, 1 / len(d));
	}
	ctrl.analog_axis[0] = -std::atan2(d[0], d[2]) / 0.0174533f;
	ctrl.analog_axis[1] = -std::atan2(d[1], d[2]) / 0.0174533f;
}

}  // namespace

ObserverSystem::ObserverSystem(World &w) : w_(w) {}
ObserverSystem::~ObserverSystem() = default;

ObserverSystem::Manager &ObserverSystem::manager(GameObject &creature) {
	auto &mp = managers_[&creature];
	if (mp) return *mp;
	mp = std::make_unique<Manager>();
	Manager &m = *mp;
	m.creature = &creature;
	// DE_CCommandManager::SetCreature (0x1501d8c0): one info per direct CreatureController child,
	// each listing the manager's executors that handle it.
	std::vector<Exec> execs;
	for (GameObject *ch : creature.children) {
		if (!ch->is("UniqueActorProperty\\CommandManager")) continue;
		for (GameObject *e : ch->children) {
			if (e->is("CommandExecutor\\DigitalOrientationPropulsion")) execs.push_back(Exec::Propulsion);
			else if (e->is("CommandExecutor\\MouseAxisNavigation")) execs.push_back(Exec::MouseAxis);
			else if (e->is("CommandExecutor\\Teleport")) execs.push_back(Exec::Teleport);
		}
	}
	for (GameObject *ch : creature.children) {
		if (ch->class_path.rfind("CreatureController\\", 0) != 0) continue;
		Manager::Info info;
		info.controller = ch;
		for (Exec e : execs) {
			if (handles(e, *ch)) info.executors.push_back(e);
		}
		m.infos.push_back(info);
	}
	return m;
}

void ObserverSystem::observe_interesting(GameObject &creature, float threshold) {
	static const bool debug = getenv("OPENBG_DEBUG_OBSERVER") != nullptr;
	if (debug) fprintf(stderr, "[observe %u] interesting creatures over %.2f\n", w_.frame(), threshold);
	Manager &m = manager(creature);
	m.forget_all();
	auto c = std::make_unique<ObserveCommand>();
	c->w = &w_, c->creature = &creature, c->threshold = threshold;
	c->orbit_speed = float(g_rand.next()) * 0.001f / 32768 - 0.0005f;
	m.receive(std::move(c));
	last_ = &creature;
}

void ObserverSystem::observe(GameObject &creature, const std::vector<GameObject *> &targets) {
	if (targets.empty() || !targets[0]) return;
	Manager &m = manager(creature);
	m.forget_all();
	auto c = std::make_unique<ObserveCommand>();
	c->w = &w_, c->creature = &creature, c->interesting = false;
	c->orbit_speed = float(g_rand.next()) * 0.001f / 32768 - 0.0005f;
	for (GameObject *t : targets) {
		if (t) c->watched.push_back(t);
	}
	m.receive(std::move(c));
	last_ = &creature;
}

GameObject *ObserverSystem::current() {
	if (last_ && (w_.object(last_->handle) != last_ || !in_scene(*last_))) last_ = nullptr;
	return last_;
}

void ObserverSystem::delta_tick(GameObject &creature, float delta) {
	auto it = managers_.find(&creature);
	if (it == managers_.end()) return;
	Manager &m = *it->second;
	uint32_t now = uint32_t(w_.scene_ms());
	if (now == m.last_ms) return;
	m.last_ms = now;
	// DE_CCommandManager::DeltaTick (0x1501e1b0).
	if (m.reassign) {
		// ReassignExecutors (0x1501dd30): each info takes the command its most confident executor
		// handles best (Execute 0x1501de30; the first wins a tie), then activates it.
		m.reassign = false;
		for (auto &info : m.infos) {
			Exec best = Exec::None;
			float best_conf = 0;
			Command *best_cmd = nullptr;
			for (auto &cp : m.list) {
				Exec e = Exec::None;
				float conf = 0;
				for (Exec x : info.executors) {
					float k = confidence(x, *info.controller, creature, *cp);
					if (k > conf) conf = k, e = x;
				}
				if (e != Exec::None && conf > best_conf) best = e, best_conf = conf, best_cmd = cp.get();
			}
			info.running = best, info.command = best_cmd;
		}
	}
	if (m.sort) m.sort = false, m.reassign = true;  // Sort (0x1501e380) asks for a reassignment
	std::vector<Command *> top;
	for (auto &cp : m.list) {
		if (!cp->is_sub) top.push_back(cp.get());
	}
	for (Command *c : top) {
		if (m.listed(c) && c->type == 8) static_cast<ObserveCommand *>(c)->update(m, delta);
	}
	if (creature.user_controlled) return;
	for (size_t i = 0; i < m.infos.size(); ++i) {
		auto &info = m.infos[i];
		if (!info.command || !m.listed(info.command)) {
			info.command = nullptr, info.running = Exec::None;
			m.reassign = true;
			continue;
		}
		const Command &cmd = *info.command;
		switch (info.running) {
		case Exec::Propulsion: propulsion(w_, creature, *info.controller, cmd); break;
		case Exec::MouseAxis: mouse_axis(creature, *info.controller, cmd); break;
		case Exec::Teleport:
			// DE_CTeleportExecutor::Tick (0x1502c4b0): jump to the destination, and every info
			// runs the teleport this frame (the look-at one turns to its target); then reassign.
			if (cmd.type == 2) {
				creature.local.t = cmd.point;
				for (auto &o : m.infos) o.running = Exec::Teleport;
			} else if (cmd.type == 4) {
				point_z_at(creature, cmd.point);
			}
			m.reassign = true;
			break;
		default: break;
		}
	}
}

GameObject *creature_camera(World &w, GameObject *creature, int index) {
	if (!creature || index < 0) return nullptr;
	if (creature->file) {
		auto lists = reference_lists(w.data(), {creature->file, creature->index});
		if (lists.size() > 2 && !lists[2].empty() && size_t(index) < lists[2][0].size() && lists[2][0][size_t(index)]) {
			GameObject *c = nearest_copy(creature, lists[2][0][size_t(index)]);
			if (c && c->is_actor && !c->is("Actor\\PassiveReference") && w.object(c->handle) == c) return c;
		}
	}
	int n = 0;
	std::function<GameObject *(GameObject *)> find = [&](GameObject *o) -> GameObject * {
		for (GameObject *ch : o->children) {
			if (ch->class_path == "Camera" && o->is_actor && n++ == index) return o;
		}
		for (GameObject *ch : o->children) {
			if (ch->is_actor && !ch->is("Actor\\Creature")) {
				if (GameObject *r = find(ch)) return r;
			}
		}
		return nullptr;
	};
	return find(creature);
}

int controller_orientation(World &, const GameObject *ctrl) {
	if (!ctrl) return 8;
	if (ctrl->input_device < 0) return ctrl->pad_orientation;
	for (int b : {0, 1, 7, 4, 3, 5, 2, 6}) {  // Get2DOrientation's test order
		if (ctrl->pad_record & (1u << b)) return b;
	}
	return 8;
}

void set_renderer_camera(World &w, GameObject *actor) {
	if (!actor) return;
	w.renderer_camera = actor;
	static const bool debug = getenv("OPENBG_DEBUG_OBSERVER") != nullptr;
	if (debug) fprintf(stderr, "[camera %u] %s%s%s\n", w.frame(), actor->name.c_str(), actor->parent ? " under " : "", actor->parent ? actor->parent->name.c_str() : "");
	uint32_t e = new_event(w, "RendererSetCamera");
	w.memory().set<uint32_t>(e + 8, w.create("Renderer", "Renderer")->handle);  // Object1
	w.send_event(w.create("Scene", "Scene"), e);
}

void register_observer_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };
	reg("CCreature::CommandObserveInterestingCreatures(float)", [](const NativeContext &c) {
		if (GameObject *o = c.self()) c.world.observers->observe_interesting(*o, c.arg<float>(0));
	});
	reg("CCreature::CommandObserve(class CCreature)", [](const NativeContext &c) {
		if (GameObject *o = c.self()) c.world.observers->observe(*o, {c.arg_object(0)});
	});
	reg("CCreature::CommandObserve(class CCreature,class CCreature)", [](const NativeContext &c) {
		static const bool debug = getenv("OPENBG_DEBUG_OBSERVER") != nullptr;
		if (debug) {
			Brain *b = c.world.brain_of(c.ex);
			auto *m = b && b->exec ? b->exec->current_method() : nullptr;
			GameObject *a = c.arg_object(0), *z = c.arg_object(1);
			fprintf(stderr, "[observe %u] %s#%x, %s#%x (%s)\n", c.world.frame(), a ? a->name.c_str() : "-", a ? a->handle : 0, z ? z->name.c_str() : "-", z ? z->handle : 0, m ? m->cm->signature.c_str() : "-");
		}
		if (GameObject *o = c.self()) c.world.observers->observe(*o, {c.arg_object(0), c.arg_object(1)});
	});
	// DE_CCreature::DeltaTick (0x15016700): only BObserverCamera::Tick and the sub-session
	// snapshot call it; it runs the command manager's executor path.
	reg("CCreature::DeltaTick(float)", [](const NativeContext &c) {
		if (GameObject *o = c.self()) c.world.observers->delta_tick(*o, c.arg<float>(0));
	});
	reg("CCreature::class CActor Camera(int)", [](const NativeContext &c) {
		c.ret_object(creature_camera(c.world, c.self(), c.arg<int32_t>(0)));
	});
	// DE_CGenericEvent::Add: the event's actor list (+0x4c/+0x50), which MajorMove reads back.
	for (const char *k : {"CGenericEvent::Add(class CActor)", "CGenericEvent::Add(class CReferenceTarget)", "CGenericEvent::Add(class CObject)"}) {
		reg(k, [](const NativeContext &c) {
			if (uint32_t e = c.self_handle(); e && c.arg<uint32_t>(0)) c.world.event_actors[e].push_back(c.arg<uint32_t>(0));
		});
	}
	reg("CRenderer::SetCamera(class CActor)", [](const NativeContext &c) { set_renderer_camera(c.world, c.arg_object(0)); });
}

}  // namespace game
