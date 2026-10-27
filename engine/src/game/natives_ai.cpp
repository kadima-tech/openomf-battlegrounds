// Native AI support for computer-controlled fighters: the creature command manager
// (DE_CCommandManager / DE_CStandardHookupExecutor, Creature module) reduced to its
// script-visible contract, plus the perception queries the fighter AI scripts make.
//
// A pilot (or the fighter itself) issues commands: navigation (CommandMoveTo), look-at
// (CommandLookAt) and attack (the pilot's AttackBestEnemy). Every frame World::run_commands
// hands each active command to the hook the fighter registered for it
// (CStandardHookupExecutor::Register*Hook), and the fighter's own script turns that into
// movement and attacks.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "de/contents.h"
#include "game/navigation.h"
#include "game/runtime.h"

namespace game {

namespace {

// DE_CCommandFactory::Init (Creature 0x1501c6d0): CCommand::type* constants.
constexpr uint32_t kTypeNavigation = 0x2, kTypeLookAt = 0x4, kTypeAttack = 0x10, kTypeAquire = 0x20;

float distance(const std::array<float, 3> &a, const std::array<float, 3> &b) {
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

// A small deterministic generator so headless runs repeat exactly.
float random01(World &w) {
	static uint32_t state = 0x2097;
	(void)w;
	state = state * 1664525u + 1013904223u;
	return float(state >> 8) / float(1u << 24);
}

namespace {

// The fighter's controlling object for a component native (controller -> its fighter).
GameObject *fighter_of(GameObject *o) {
	while (o && !o->fighter) {
		o = o->parent;
	}
	return o;
}


float time_ms(World &w) { return w.scene_ms(); }

// The attack target the pilot is working on (pilot +0x338; here the attack command's target).
GameObject *pilot_target(World &w, GameObject &o) {
	for (auto *ch : o.children) {
		if (ch->class_path == "Actor\\Creature\\Pilot") return w.object(ch->pilot_target);
	}
	return nullptr;
}

// AIAttackRangeCaller (OMF 0x35020d60): base (+0x5f4, 1) x multiplier (+0x5f8) x the pilot's
// attack-range stat, at least 40.
float attack_range(World &w, const GameObject &o) {
	float stat = o.pilot ? o.pilot->stat(0x5e8, time_ms(w)) : 120.0f;
	return std::max(40.0f, 1.0f * o.ai_range_multiplier * stat);
}

// DE_COMF2Fighter::GetDesirability (0x3502b120). -0.75 x danger at the spot, plus
// a range-keeping term (positions about the attack range from the target score highest), plus a
// flanking term: within 130 units of the target, on the side the fighter is already on, spots
// behind the target gain BackAttack x Left/Right affinity x 0.07 (x Melee when Melee < 1).
float desirability(World &w, GameObject &o, const std::array<float, 3> &p) {
	GameObject *t = pilot_target(w, o);
	if (!t) {
		return 0;
	}
	float time = time_ms(w);
	auto stat = [&](uint32_t off) { return o.pilot ? o.pilot->stat(off, time) : 1.0f; };
	auto me = o.world().t;
	de::Transform tt = t->world();
	auto tp = tt.t;
	float range = attack_range(w, o);
	float d = distance(p, tp);
	std::array<float, 3> a{p[0] - me[0], p[1] - me[1], p[2] - me[2]}, b{me[0] - tp[0], me[1] - tp[1], me[2] - tp[2]};
	auto norm = [](std::array<float, 3> &v) {
		float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (l > 1e-6f) {
			v = {v[0] / l, v[1] / l, v[2] / l};
		}
	};
	norm(a);
	norm(b);
	float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	float v = -0.75f * w.danger_at(p, &o, nullptr, nullptr) + (range - d) / (range + 50.0f) * dot * 0.6f;
	if (!t->fighter) {
		return v;
	}
	float weight = 0.07f;
	if (stat(0x4e8) < 1) {
		weight = stat(0x4e8) * 0.07f;
	}
	auto rel = tt.inverse_rotate({p[0] - tp[0], 0, p[2] - tp[2]});  // spot, in the target's frame
	rel = {rel[0] / 130, 0, rel[2] / 130};
	auto self = tt.inverse_rotate({me[0] - tp[0], me[1] - tp[1], me[2] - tp[2]});
	float len = std::sqrt(rel[0] * rel[0] + rel[2] * rel[2]);
	if (len >= 1) {
		return v;
	}
	if (len > 1e-6f) {  // MakeLength(1 - len): nearer spots weigh more
		rel = {rel[0] / len * (1 - len), 0, rel[2] / len * (1 - len)};
	}
	float side;
	if (rel[0] <= 0) {
		if (self[0] >= 0) {
			return v;
		}
		side = stat(0x3e8);
	} else {
		if (self[0] <= 0) {
			return v;
		}
		side = stat(0x408);
	}
	return v - (rel[2] - 1) * stat(0x528) * side * weight;
}

// DE_COMF2Fighter::FindDesirableMoveToPosition (0x3502b690): try 8 directions `radius` away,
// starting towards the target, weighted forward 1.3 / side-forward 0.8 / side-back 0.7 /
// back 1.1, keeping only points with floor below; returns best minus current desirability.
float find_desirable_move(World &w, GameObject &o, float radius, std::array<float, 3> &out) {
	de::Transform t = o.world();
	float base = 0;
	if (GameObject *tg = pilot_target(w, o)) {
		auto tp = tg->world().t;
		auto l = t.inverse_rotate({tp[0] - t.t[0], tp[1] - t.t[1], tp[2] - t.t[2]});
		base = std::atan2(l[0], l[2]);
	}
	float here = desirability(w, o, t.t);
	float best = -10000;
	out = t.t;
	const float weight[8] = {1.3f, 0.8f, 1, 0.7f, 1.1f, 0.7f, 1, 0.8f};
	for (int i = 0; i < 8; ++i) {
		float a = float(i) * 0.785398f + base;
		auto p = t.apply({radius * std::sin(a), 0, radius * std::cos(a)});
		float v = desirability(w, o, p) * weight[i];
		if (v > best && w.floor && w.floor(p[0], p[2], p[1] + 25) >= p[1] - 100) {
			best = v;
			out = p;
		} else if (v > best && !w.floor) {
			best = v;
			out = p;
		}
	}
	return best - here;
}

// CPilot::aiAffinity* bit -> the stat it weighs (DE_CPilot::AttackAffinity 0x3504c9c0); the
// usage slot has the bit's index. Only these 11 bits are tested; the others (6-8, 11, 12, 16+)
// add nothing to the affinity but still count as used in TestAttackAffinity.
uint32_t affinity_stat(int bit) {
	static const uint32_t table[16] = {0x428, 0x448, 0x468, 0x488, 0x4a8, 0x4e8, 0, 0,
	                                   0, 0x568, 0x588, 0, 0, 0x4c8, 0x3e8, 0x408};
	return bit < 16 ? table[bit] : 0;
}

// The mean over the flagged attack categories of stat / usage (0.1 when none count). Whenever
// (time + the flagged stats' phases) / 500 changes, every usage slot decays by 0.15 x the
// variation stat, down to 1 (the original's clock really does include those phases).
float attack_affinity(World &w, GameObject &o, uint32_t flags) {
	float t = time_ms(w);
	float clock = t;
	float sum = 0, n = 0;
	for (int i = 0; i < 22; ++i) {
		if ((flags & (1u << i)) && affinity_stat(i)) {
			float stat = o.pilot ? o.pilot->stat(affinity_stat(i), t) : 1.0f;
			if (o.pilot) {
				auto it = o.pilot->ai.find(affinity_stat(i));
				clock += it != o.pilot->ai.end() ? it->second.phase : 0;
			}
			// usage 0 (never decayed yet) divides by zero in the original: infinite affinity
			sum += o.attack_usage[size_t(i)] > 0 ? stat / o.attack_usage[size_t(i)] : 1e6f;
			n += 1;
		}
	}
	float aff = n > 0 ? sum / n : 0.1f;
	int32_t second = int32_t(clock / 500);
	if (second != o.usage_second) {
		float variety = o.pilot ? o.pilot->stat(0x608, t) : 1.0f;
		for (auto &u : o.attack_usage) {
			u -= variety * 0.15f;
			if (u < 1) {
				u = 1;
			}
		}
		o.usage_second = second;
	}
	return aff;
}

}  // namespace

// The pilot behind a pilot object or a fighter.
const Pilot *pilot_of(GameObject *o) {
	if (o && o->pilot) return o->pilot;  // a pilot object (the session's)
	GameObject *f = fighter_of(o);
	if (!f) return nullptr;
	if (f->pilot) return f->pilot;
	for (auto *ch : f->children) {
		if (ch->pilot) return ch->pilot;
	}
	return nullptr;
}

void World::issue_command(GameObject &o, GameObject::Command c) {
	auto &m = o.commands;
	c.id = m.next_id++;
	c.step = c.pos;
	m.list.push_back(c);
	m.dirty = true;
}

// DE_CCommandManager::Forget: every command whose type lies within the mask is removed
// (RemoveCommand: out of the list, the controller's active command and its alternates).
void World::forget_commands(GameObject &o, uint32_t mask) {
	auto &m = o.commands;
	for (size_t i = m.list.size(); i-- > 0;) {
		uint32_t id = m.list[i].id;
		if ((m.list[i].type & mask) != m.list[i].type) continue;
		m.list.erase(m.list.begin() + long(i));
		m.dirty = true;
		if (m.active == id) m.active = 0;
		m.backups.erase(std::remove(m.backups.begin(), m.backups.end(), id), m.backups.end());
	}
}

namespace {

// DE_COMF2Fighter::GlobalInterest (0x3502ae40) / CActor::GlobalInterest.
float global_interest(World &w, GameObject *o) { return w.global_interest_of(o); }

// DE_CPilot::Attack (0x3504d280): CommandAttack(target, aggression); a new target restarts the
// target clock and sets the quality offset (+0x3d0) to -(quality degradation - 0.5) when that
// (+0x7cc) is above 0.5. Against a user-controlled creature the offset is cleared unless the
// master session is joinable (its +0x38 vtable +0x154, the slot IsJoinableCaller reads): only a
// network game handicaps the computer against a human. Our sessions are local, never joinable.
void attack(World &w, GameObject &pilot, GameObject &self, GameObject &target, float aggression) {
	GameObject::Command m;
	m.type = kTypeAttack;
	m.target = target.handle;
	m.value = aggression;
	w.issue_command(self, m);
	if (target.handle == pilot.pilot_target) return;
	pilot.pilot_target = target.handle;
	pilot.pilot_target_since = time_ms(w);
	const Pilot *p = pilot_of(&self);
	float q = p ? p->quality : 0;
	pilot.quality_offset = q > 0.5f ? -(q - 0.5f) : 0;
	if (target.user_controlled) pilot.quality_offset = 0;
}

// DE_CStandardHookupExecutor::Confidence: attack and aquire commands score their value, move-to
// and look-at 1, each only when the fighter registered that hook.
float confidence(const GameObject &o, const GameObject::Command &c) {
	switch (c.type) {
	case kTypeAttack: return o.hook_attack ? c.value : 0;
	case kTypeAquire: return o.hook_aquire ? c.value : 0;
	case kTypeNavigation: return o.hook_move ? 1.0f : 0;
	case kTypeLookAt: return o.hook_look ? 1.0f : 0;
	}
	return 0;
}

// DE_CCommandManager::Sort: key = priority (1), MoveTo::ModifySort drops it to 0 within the
// required distance of its path position; a stable bubble sort, highest first.
void sort_commands(GameObject &o) {
	auto &m = o.commands;
	auto me = o.world().t;
	m.order.clear();
	for (size_t i = 0; i < m.list.size(); ++i) {
		auto &c = m.list[i];
		c.sort = 1;
		if (c.type == kTypeNavigation && distance(me, c.step) < c.distance) c.sort = 0;
		m.order.push_back(i);
	}
	std::stable_sort(m.order.begin(), m.order.end(), [&](size_t x, size_t y) { return m.list[x].sort > m.list[y].sort; });
	m.dirty = false;
	m.reassign = true;
}

// DE_CCommandManager::Execute for the one controller.
void execute_command(GameObject &o, const GameObject::Command &c) {
	auto &m = o.commands;
	float conf = confidence(o, c);
	if (conf > m.candidate_conf) {
		if (m.candidate) m.backups.push_back(m.candidate);
		m.candidate = c.id;
		m.candidate_conf = conf;
	} else if (conf > 0) {
		m.backups.push_back(c.id);
	}
}

// DE_CCommandManager::ReassignExecutors: the first sorted command of each type takes part.
void reassign_commands(GameObject &o) {
	auto &m = o.commands;
	if (m.list.empty()) return;
	m.reassign = false;
	m.candidate = 0;
	m.candidate_conf = 0;
	m.backups.clear();
	if (m.dirty) sort_commands(o);  // SortedCommand
	uint32_t mask = 0x3e;
	for (size_t i = 0; i < m.order.size() && mask; ++i) {
		const auto &c = m.list[m.order[i]];
		if (i > 0 && !(mask & c.type)) continue;
		execute_command(o, c);
		mask ^= c.type & 0x3e;
	}
	if (m.candidate) m.active = m.candidate;  // CControllerInfo::Activate
}

}  // namespace

// DE_CCommandManager::Tick (Creature 0x1501e000) with DE_CStandardHookupExecutor::Tick: the
// commands update, then the active command's hook runs; when it did nothing, or it is a move or
// look command, the alternates' hooks run too (an acting attack or aquire command excludes them).
// DE_COMF2Fighter::NumEnemies / Enemy (0x3502a3a0 / 0x3502a3e0): the fighters this one was
// linked with (+0x334: every other fighter prepared in the scene) that take it as an enemy
// (IsEnemyFighter 0x3502a740: not itself, and not on its team, +0x49c >= 0). Fighters carry no
// team here (their pilots' factions are not decoded), so that is every other fighter.
std::vector<GameObject *> World::enemies_of(GameObject &f) {
	std::vector<GameObject *> out;
	for (GameObject *e : fighters()) {
		if (e != &f && in_scene(*e)) out.push_back(e);
	}
	return out;
}

// DE_COMF2Fighter::GlobalInterest (0x3502ae40): recomputed when more than 300 ms of scene time
// have passed since the last time (+0x488): its own interest (+500) times (900 - d + 1) for every
// enemy within 900 units; until then the cached value (+0x48c, 1 to begin with) stands. Other
// creatures report their own interest.
float World::global_interest_of(GameObject *o) {
	if (!o) return 0;
	if (!o->fighter) return o->global_interest;
	uint32_t now = uint32_t(scene_ms());
	uint32_t since = now > o->interest_time ? now - o->interest_time : o->interest_time - now;
	if (since > 300) {
		o->interest_time = now;
		float v = o->global_interest;
		auto p = o->world().t;
		auto list = enemies_of(*o);
		for (auto it = list.rbegin(); it != list.rend(); ++it) {
			float d = distance(p, (*it)->world().t);
			if (d < 900) v *= 900 - d + 1;
		}
		o->interest_cached = v;
	}
	return o->interest_cached;
}

// DE_COMF2Fighter::CurrentTarget (0x3502a8b0): reconsidered once 1000 ms of scene time have
// passed since the last look (200 ms after one that kept the same target). Each other fighter in
// the scene (+0x334, filled by ObjPrepare), last first, scores 100 / (d + 100), plus its forward
// offset (the unit direction to it in this fighter's frame, z) when it is in front; the current
// pick takes x (2000 / ms since it was chosen + 1), a GlobalInterest under 1 scales it, and one
// that is not an enemy (IsEnemyFighter) takes a quarter. The best scoring one is the target.
GameObject *World::current_target_of(GameObject &f) {
	uint32_t now = uint32_t(scene_ms());
	if (now < f.target_checked) f.target_checked = f.target_chosen = 0;
	if (f.target_checked + 1000 <= now) {
		de::Transform me = f.world();
		auto enemies = enemies_of(f);
		GameObject *best = nullptr;
		float best_score = -1e11f;
		std::vector<GameObject *> others;
		for (GameObject *e : fighters()) {
			if (e != &f && in_scene(*e)) others.push_back(e);
		}
		for (auto it = others.rbegin(); it != others.rend(); ++it) {
			GameObject *p = *it;
			auto pt = p->world().t;
			auto d = me.inverse_rotate({pt[0] - me.t[0], pt[1] - me.t[1], pt[2] - me.t[2]});
			float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			float score = 100 / (dist + 100);
			if (dist > 0 && d[2] / dist > 0) score += d[2] / dist;
			if (p->handle == f.current_target) score *= 2000.0f / float(std::max<uint32_t>(1, now - f.target_chosen)) + 1;
			float interest = global_interest_of(p);
			if (interest < 1) score *= interest;
			if (std::find(enemies.begin(), enemies.end(), p) == enemies.end()) score *= 0.25f;
			if (best_score < score) best_score = score, best = p;
		}
		uint32_t pick = best ? best->handle : 0;
		if (pick != f.current_target) {
			f.target_checked = f.target_chosen = now;
			f.current_target = pick;
		} else {
			f.target_checked = now - 800;
		}
	}
	return object(f.current_target);
}

void World::run_commands(GameObject &o) {
	auto &m = o.commands;
	if (m.reassign) reassign_commands(o);
	if (m.list.empty()) return;
	std::array<float, 3> me = o.world().t;
	for (auto &c : m.list) {
		if (c.type == kTypeNavigation) {
			// DE_CMoveToCommand::Update: the next position on the navigation path.
			c.step = c.pos;
			c.step_distance = distance(me, c.pos);
			navigation->steer(o, c.pos, &c.step, &c.step_distance);
		} else if (c.type == kTypeLookAt && c.target) {
			// DE_CLookAtCommand::Update: the target's position (+ an offset, not used by scripts).
			if (GameObject *t = object(c.target)) c.pos = t->world().t;
		}
	}
	if (o.user_controlled || !o.brain || !o.spawned) return;
	if (getenv("OPENBG_DEBUG_AI") && frame_ % 30 == 0) {
		fprintf(stderr, "ai %s frame %u: %zu commands, active %u, %zu alternates, counter %d\n", o.name.c_str(), frame_,
		        m.list.size(), m.active ? m.find(m.active)->type : 0, m.backups.size(), m.counter);
	}
	// CVector arguments live in two blocks per fighter, allocated on first use.
	auto vec_arg = [&](int slot, const std::array<float, 3> &v) {
		uint32_t &a = o.ai_vec[size_t(slot)];
		if (!a) a = mem_.alloc(12);
		write_vec(mem_, a, v);
		return a;
	};
	// Runs a command's hook; false when the executor said it did nothing.
	auto run = [&](uint32_t id) -> bool {
		GameObject::Command *cp = m.find(id);
		if (!cp) return true;
		GameObject::Command c = *cp;  // the hook may change the list
		m.acted = true;
		std::vector<uint8_t> args;
		uint32_t hook = 0;
		switch (c.type) {
		case kTypeAttack:
		case kTypeAquire: {
			hook = c.type == kTypeAttack ? o.hook_attack : o.hook_aquire;
			if (!object(c.target)) {
				// The target is gone (its reference cleared). DE_CStandardHookupExecutor::Tick
				// (Creature 0x1502b650): an attack command then attacks the creature's first enemy
				// (vtable +0x16c, Enemy(0)); an aquire command calls no hook and counts as acted.
				if (c.type == kTypeAquire) return true;
				auto e = enemies_of(o);
				c.target = e.empty() ? 0 : e.front()->handle;
			}
			args.resize(8);
			memcpy(&args[0], &c.target, 4);
			memcpy(&args[4], &c.value, 4);
			break;
		}
		case kTypeNavigation: {
			hook = o.hook_move;
			args.resize(12);
			uint32_t p = vec_arg(0, c.step);
			memcpy(&args[0], &p, 4);
			memcpy(&args[4], &c.distance, 4);
			memcpy(&args[8], &c.step_distance, 4);
			break;
		}
		case kTypeLookAt: {
			hook = o.hook_look;
			args.resize(4);
			uint32_t p = vec_arg(1, c.pos);
			memcpy(&args[0], &p, 4);
			break;
		}
		}
		if (hook) o.brain->call_handle(*this, hook, args);
		return m.acted;
	};
	GameObject::Command *active = m.active ? m.find(m.active) : nullptr;
	if (!active) {
		m.active = 0;
		if (++m.counter > 20) m.reassign = true;
		return;
	}
	uint32_t type = active->type;
	if (!run(m.active)) {
		if (++m.counter > 8) m.reassign = true;
	} else if ((type & 0x30) || (6 & ~type) == 0) {
		return;
	}
	auto backups = m.backups;
	for (uint32_t id : backups) run(id);
}

void register_ai_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };

	// --- Hookup executor: the fighter registers its Executor* methods; arg 1 is the method handle.
	const std::vector<std::pair<const char *, uint32_t GameObject::*>> hooks = {
		{"CStandardHookupExecutor::RegisterAttackHook(class CClassExecutorInstance,Method(class CActorfloat))",
		 &GameObject::hook_attack},
		{"CStandardHookupExecutor::RegisterAquireHook(class CClassExecutorInstance,Method(class CActorfloat))",
		 &GameObject::hook_aquire},
		{"CStandardHookupExecutor::RegisterMoveToHook(class CClassExecutorInstance,Method(class CVectorfloatfloat))",
		 &GameObject::hook_move},
		{"CStandardHookupExecutor::RegisterLookAtHook(class CClassExecutorInstance,Method(class CVector))",
		 &GameObject::hook_look}};
	for (auto &[key, field] : hooks) {
		auto f = field;
		reg(key, [f](const NativeContext &c) {
			if (GameObject *o = c.owner()) {
				o->*f = c.arg<uint32_t>(1);
			}
		});
	}
	// ExecutorDidNothingCaller: clears +0x30, so the command manager runs the alternates.
	reg("CStandardHookupExecutor::ExecutorDidNothing()", [](const NativeContext &c) {
		if (GameObject *o = c.owner()) o->commands.acted = false;
	});

	// --- Commands (DE_CCreature::Command*, Creature 0x150177b0..0x15018060) --------------------
	reg("CCreature::CommandMoveTo(class CVector,float)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			GameObject::Command m;
			m.type = kTypeNavigation;
			m.pos = read_vec(c.mem(), c.arg<uint32_t>(0));
			m.distance = c.arg<float>(1);
			c.world.issue_command(*o, m);
			if (getenv("OPENBG_DEBUG_AI")) {
				fprintf(stderr, "command %s frame %u: move to (%.1f %.1f %.1f) within %.1f\n", o->name.c_str(),
				        c.world.frame(), m.pos[0], m.pos[1], m.pos[2], m.distance);
			}
		}
	});
	reg("CCreature::CommandLookAt(class CActor)", [](const NativeContext &c) {
		GameObject *o = c.self(), *t = c.arg_object(0);
		if (o && t) {
			GameObject::Command m;
			m.type = kTypeLookAt;
			m.target = t->handle;
			m.pos = t->world().t;
			c.world.issue_command(*o, m);
		}
	});
	reg("CCreature::CommandLookAt(class CVector)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			GameObject::Command m;
			m.type = kTypeLookAt;
			m.pos = read_vec(c.mem(), c.arg<uint32_t>(0));
			c.world.issue_command(*o, m);
		}
	});
	// CommandAttack / CommandAquire (0x15017cb0 / 0x15017e80): a new command with the value at +0x78.
	reg("CCreature::CommandAttack(class CActor,float)", [](const NativeContext &c) {
		GameObject *o = c.self(), *t = c.arg_object(0);
		if (o && t) {
			GameObject::Command m;
			m.type = kTypeAttack;
			m.target = t->handle;
			m.value = c.arg<float>(1);
			c.world.issue_command(*o, m);
		}
	});
	reg("CCreature::CommandAquire(class CActor,float)", [](const NativeContext &c) {
		GameObject *o = c.self(), *t = c.arg_object(0);
		if (o && t) {
			GameObject::Command m;
			m.type = kTypeAquire;
			m.target = t->handle;
			m.value = c.arg<float>(1);
			c.world.issue_command(*o, m);
		}
	});
	reg("CCreature::CommandForget(dword)", [](const NativeContext &c) {
		if (auto *o = c.self()) c.world.forget_commands(*o, c.arg<uint32_t>(0));
	});
	reg("CCreature::CommandForgetAll()", [](const NativeContext &c) {
		if (auto *o = c.self()) c.world.forget_commands(*o, ~0u);
	});
	reg("CCreature::bool IsCommandOfTypeActive(dword)", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_bool(o && o->commands.type_active(c.arg<uint32_t>(0)));
	});

	// --- Geometry ----------------------------------------------------------------------------
	reg("CActor::float DistanceTo(class CActor)", [](const NativeContext &c) {
		auto *a = c.self();
		auto *b = c.arg_object(0);
		c.ret_f32(a && b ? distance(a->world().t, b->world().t) : 0);
	});
	reg("CObject::CVector::float DistanceTo(class CVector)", [](const NativeContext &c) {
		c.ret_f32(distance(read_vec(c.mem(), c.self_handle()), read_vec(c.mem(), c.arg<uint32_t>(0))));
	});
	// Convert a CVector in place between world and the actor's local frame.
	reg("CActor::ConvertFromWorldToLocal(class CVector)", [](const NativeContext &c) {
		auto *o = c.self();
		uint32_t v = c.arg<uint32_t>(0);
		if (o && v) {
			de::Transform t = o->world();
			auto p = read_vec(c.mem(), v);
			write_vec(c.mem(), v, t.inverse_rotate({p[0] - t.t[0], p[1] - t.t[1], p[2] - t.t[2]}));
		}
	});
	reg("CActor::ConvertFromLocalToWorld(class CVector)", [](const NativeContext &c) {
		auto *o = c.self();
		uint32_t v = c.arg<uint32_t>(0);
		if (o && v) {
			write_vec(c.mem(), v, o->world().apply(read_vec(c.mem(), v)));
		}
	});

	// Directions only (no translation): the actor's world orientation, and its transpose.
	reg("CActor::RotateFromLocalToWorld(class CVector)", [](const NativeContext &c) {
		auto *o = c.self();
		uint32_t v = c.arg<uint32_t>(0);
		if (o && v) write_vec(c.mem(), v, o->world().rotate(read_vec(c.mem(), v)));
	});
	reg("CActor::RotateFromWorldToLocal(class CVector)", [](const NativeContext &c) {
		auto *o = c.self();
		uint32_t v = c.arg<uint32_t>(0);
		if (o && v) write_vec(c.mem(), v, o->world().inverse_rotate(read_vec(c.mem(), v)));
	});

	// --- Perception ------------------------------------------------------------------------------
	// IsEnemy (vtable +0x178): a plain creature has no enemies (Creature 0x15008350); a pilot asks
	// its controlled creature (DE_CPilot::IsEnemy 0x3504c9a0, +0x320); a fighter (0x3502a6f0)
	// takes everything but itself as an enemy, except a fighter on its own team (IsEnemyFighter
	// 0x3502a740: TeamIndex +0x49c, -1 for none; nobody has a team yet).
	reg("CCreature::bool IsEnemy(class CActor)", [](const NativeContext &c) {
		GameObject *a = c.self(), *b = c.arg_object(0);
		if (a && a->is("Actor\\Creature\\Pilot")) a = fighter_of(a->parent);
		c.ret_bool(a && a->fighter && b != a);
	});
	reg("CPilot::bool IsEnemy(class CActor)", [](const NativeContext &c) {
		GameObject *a = c.self(), *b = c.arg_object(0);
		a = a ? fighter_of(a->parent) : nullptr;
		c.ret_bool(a && a->fighter && b != a);
	});
	// Line of sight between the two actors' focal points against the arena collision mesh.
	reg("CCreature::bool TestLineOfSight(class CActor)", [](const NativeContext &c) {
		auto *a = c.self();
		auto *b = c.arg_object(0);
		c.ret_bool(a && b && !c.world.segment_blocked(focal_point(*a), focal_point(*b)));
	});

	// --- Math and vectors (Core CVector / FLOATPrimitive callers) ------------------------------
	reg("CObject::float Clamp(float,float,float)", [](const NativeContext &c) {
		c.ret_f32(std::min(std::max(c.arg<float>(0), c.arg<float>(1)), c.arg<float>(2)));
	});
	reg("CObject::float Abs(float)", [](const NativeContext &c) { c.ret_f32(std::fabs(c.arg<float>(0))); });
	// FLOATPrimitive operators act on the float `this` points at and return the result.
	auto float_op = [&](const char *key, float (*f)(float, float)) {
		reg(key, [f](const NativeContext &c) {
			uint32_t p = c.self_handle();
			float v = p ? f(c.mem().get<float>(p), c.arg<float>(0)) : 0;
			if (p) {
				c.mem().set<float>(p, v);
			}
			c.ret_f32(v);
		});
	};
	float_op("CObject::FLOATPrimitive::float (opp *=)(float)", [](float a, float b) { return a * b; });
	float_op("CObject::FLOATPrimitive::float (opp +=)(float)", [](float a, float b) { return a + b; });
	float_op("CObject::FLOATPrimitive::float (opp -)(float)", [](float a, float b) { return a - b; });
	float_op("CObject::FLOATPrimitive::float (opp -=)(float)", [](float a, float b) { return a - b; });
	float_op("CObject::FLOATPrimitive::float (opp /=)(float)", [](float a, float b) { return a / b; });
	reg("CObject::CVector::(opp -)(class CVector)", [](const NativeContext &c) {  // EqualsSubtractOpperatorCaller
		auto a = read_vec(c.mem(), c.self_handle()), b = read_vec(c.mem(), c.arg<uint32_t>(0));
		write_vec(c.mem(), c.self_handle(), {a[0] - b[0], a[1] - b[1], a[2] - b[2]});
	});
	reg("CObject::CVector::float Dot(class CVector)", [](const NativeContext &c) {
		auto a = read_vec(c.mem(), c.self_handle()), b = read_vec(c.mem(), c.arg<uint32_t>(0));
		c.ret_f32(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
	});
	reg("CObject::CVector::CapLength(float)", [](const NativeContext &c) {
		auto a = read_vec(c.mem(), c.self_handle());
		float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), cap = c.arg<float>(0);
		if (l > cap && l > 0) {
			write_vec(c.mem(), c.self_handle(), {a[0] * cap / l, a[1] * cap / l, a[2] * cap / l});
		}
	});
	// VectorRotateYDegCaller: same handedness as TurnLeft (positive turns towards local +x).
	reg("CObject::CVector::RotateYDeg(float)", [](const NativeContext &c) {
		auto a = read_vec(c.mem(), c.self_handle());
		float r = c.arg<float>(0) * 0.017453292f, cs = std::cos(r), sn = std::sin(r);
		write_vec(c.mem(), c.self_handle(), {cs * a[0] + sn * a[2], a[1], -sn * a[0] + cs * a[2]});
	});
	reg("CObject::CVector::Reset()", [](const NativeContext &c) { write_vec(c.mem(), c.self_handle(), {0, 0, 0}); });
	reg("CActor::RotateFromWorldToLocal(class CVector)", [](const NativeContext &c) {
		auto *o = c.self();
		uint32_t v = c.arg<uint32_t>(0);
		if (o && v) {
			write_vec(c.mem(), v, o->world().inverse_rotate(read_vec(c.mem(), v)));
		}
	});
	reg("CActor::class CVector Momentum()", [](const NativeContext &c) {
		auto *o = c.self();
		if (!o) {
			c.ret_u32(0);
			return;
		}
		if (!o->momentum_vec) {
			o->momentum_vec = c.mem().alloc(12);
		}
		// MomentumCaller (Core): each physics component's momentum x its TimeScale; a fighter's
		// physics runs at 1/16 (1 / ms per frame), so this is units per millisecond.
		float k = o->fighter ? 1.0f / 16 : 1.0f;
		write_vec(c.mem(), o->momentum_vec, {o->momentum[0] * k, o->momentum[1] * k, o->momentum[2] * k});
		c.ret_u32(o->momentum_vec);
	});

	// --- Controller (CCreatureController defaults: how the AI presses buttons) ------------------
	// CreatureControllerCaller / NumCreatureControllerCaller (Creature 0x15013620): the creature's
	// controllers (+0x13c, count +0x140): its CreatureController children, in order. A fighter's
	// are made at run time by its client (not modelled): it gets a stand-in controller.
	auto controllers = [](GameObject *o) {
		std::vector<GameObject *> out;
		for (auto *ch : o->children) {
			if (ch->class_path.rfind("CreatureController\\", 0) == 0) out.push_back(ch);
		}
		return out;
	};
	reg("CCreature::int NumControllers()", [controllers](const NativeContext &c) {
		auto *o = c.self();
		c.ret_u32(o ? uint32_t(controllers(o).size()) : 0);
	});
	// DE_CCreatureController::RetrieveAndClearAnalogAxisMovement (0x150019a0): the axis's
	// accumulated movement, cleared. A mouse controller bound to a device (+0x2c != -1) reads
	// the mouse instead; none is bound here, so it returns what its command executors left.
	reg("CCreatureController::float RetrieveAndClearAnalogAxisMovement(int)", [](const NativeContext &c) {
		GameObject *o = c.self();
		int axis = c.arg<int32_t>(0);
		float v = 0;
		if (o && axis >= 0 && axis < 4) std::swap(v, o->analog_axis[size_t(axis)]);
		c.ret_f32(v);
	});
	reg("CCreature::class CCreatureController Controller(int)", [controllers](const NativeContext &c) {
		auto *o = c.self();
		if (!o) {
			c.ret_u32(0);
			return;
		}
		auto list = controllers(o);
		int i = c.arg<int32_t>(0);
		if (i >= 0 && size_t(i) < list.size()) {
			c.ret_object(list[size_t(i)]);
			return;
		}
		for (auto *ch : o->children) {
			if (ch->class_path == "CreatureController") {
				c.ret_object(ch);
				return;
			}
		}
		c.ret_object(c.world.create_child(o, "Controller", "CreatureController"));
	});
	auto button = [](Input &in, int32_t id) -> bool * {
		switch (id) {
		case 0: return &in.left_punch;
		case 1: return &in.right_punch;
		case 2: return &in.left_kick;
		case 3: return &in.right_kick;
		case 4: return &in.jump;
		case 5: return &in.duck;
		default: return nullptr;
		}
	};
	reg("CCreatureController::SetDefaultButtonDown(int)", [=](const NativeContext &c) {
		if (auto *f = fighter_of(c.self())) {
			if (bool *b = button(f->ai_input, c.arg<int32_t>(0))) *b = true;
		}
	});
	reg("CCreatureController::SetDefaultButtonUp(int)", [=](const NativeContext &c) {
		if (auto *f = fighter_of(c.self())) {
			if (bool *b = button(f->ai_input, c.arg<int32_t>(0))) *b = false;
		}
	});
	reg("CCreatureController::bool IsButtonDown(int)", [=](const NativeContext &c) {
		auto *f = fighter_of(c.self());
		bool *b = f ? button(f->ai_input, c.arg<int32_t>(0)) : nullptr;
		c.ret_bool(b && *b);
	});
	reg("CCreatureController::ClearAllDefaults()", [](const NativeContext &c) {
		if (auto *f = fighter_of(c.self())) f->ai_input = {};
	});
	// 2D orientation defaults: absolute directions, or "towards" one axis keeping the other.
	struct Dir {
		const char *name;
		int v, h;  // vertical: 1 up, -1 down, 0 middle, 2 keep; horizontal: 1 right, -1 left, 0, 2 keep
	};
	const Dir dirs[] = {{"Up", 1, 0}, {"UpRight", 1, 1}, {"Right", 0, 1}, {"DownRight", -1, 1},
	                    {"Down", -1, 0}, {"DownLeft", -1, -1}, {"Left", 0, -1}, {"UpLeft", 1, -1},
	                    {"Middle", 0, 0}, {"TowardsUp", 1, 2}, {"TowardsDown", -1, 2},
	                    {"TowardsMiddleVertical", 0, 2}, {"TowardsRight", 2, 1}, {"TowardsLeft", 2, -1},
	                    {"TowardsMiddleHorizontal", 2, 0}};
	for (const Dir &d : dirs) {
		int v = d.v, h = d.h;
		reg(std::string("CCreatureController::SetDefault2DOrientation") + d.name + "(int)", [=](const NativeContext &c) {
			if (auto *f = fighter_of(c.self())) {
				auto &in = f->ai_input;
				if (v != 2) {
					in.up = v == 1;
					in.down = v == -1;
				}
				if (h != 2) {
					in.right = h == 1;
					in.left = h == -1;
				}
			}
		});
	}
	// Get2DOrientationCaller (Creature 0x15026f00, controller vtable +0x130): 0 up .. 7 up-left
	// clockwise, 8 neutral; the user's from the joystick, a computer fighter's from its defaults.
	// Is2DOrientationRight/LeftCaller (0x15026cf0, 0x15026cc0) ask for exactly 2 / 6.
	auto orientation = [](const NativeContext &c) -> int {
		auto *f = fighter_of(c.self());
		if (!f) return 8;
		if (f->user_controlled) return c.world.orientation();
		const Input &in = f->ai_input;
		int v = in.up && !in.down ? 1 : in.down && !in.up ? -1 : 0;
		int h = in.right && !in.left ? 1 : in.left && !in.right ? -1 : 0;
		static const int table[3][3] = {{5, 4, 3}, {6, 8, 2}, {7, 0, 1}};  // [v + 1][h + 1]
		return table[v + 1][h + 1];
	};
	reg("CCreatureController::byte Get2DOrientation(int)", [orientation](const NativeContext &c) { c.ret_u32(uint32_t(orientation(c))); });
	reg("CCreatureController::bool Is2DOrientationRight(int)", [orientation](const NativeContext &c) { c.ret_bool(orientation(c) == 2); });
	reg("CCreatureController::bool Is2DOrientationLeft(int)", [orientation](const NativeContext &c) { c.ret_bool(orientation(c) == 6); });
	reg("CCreatureController::bool Is2DOrientationTowardsRight(int)", [](const NativeContext &c) {
		auto *f = fighter_of(c.self());
		c.ret_bool(f && f->ai_input.right);
	});
	reg("CCreatureController::bool Is2DOrientationTowardsLeft(int)", [](const NativeContext &c) {
		auto *f = fighter_of(c.self());
		c.ret_bool(f && f->ai_input.left);
	});
	reg("CCreatureController::bool Is2DOrientationTowardsDown(int)", [](const NativeContext &c) {
		auto *f = fighter_of(c.self());
		c.ret_bool(f && f->ai_input.down);
	});
	reg("CCreatureController::bool Is2DOrientationDown(int)", [](const NativeContext &c) {
		auto *f = fighter_of(c.self());
		c.ret_bool(f && f->ai_input.down && !f->ai_input.left && !f->ai_input.right);
	});

	// --- Pilot control (DE_CPilot +800 controlled creature; AttackBestEnemy 0x3504d370) ----------
	reg("CPilot::class CCreature ControlledCreature()", [](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_object(o ? fighter_of(o->parent) : nullptr);
	});
	// DE_CPilot::CurrentPhoto (0x3500fa10): the first of its photos (+0x310) -- the pilot's
	// image (e.g. Pilots.DEObjects Lucas/Lucas [Image\\Standard]).
	reg("CPilot::class CImage CurrentPhoto()", [](const NativeContext &c) {
		GameObject *o = c.self(), *img = nullptr;
		if (o) {
			for (auto *ch : o->children) {
				if (!img && ch->is("Image")) img = ch;
			}
			if (!img && o->file) {
				for (auto &r : c.world.actor_properties({o->file, o->index})) {
					if (!img && r && r.info().class_path.rfind("Image", 0) == 0) img = c.world.global_object(r);
				}
			}
		}
		c.ret_object(img);
	});
	// FactionCaller / SetFactionCaller (OMF 0x350482e0): the pilot's faction reference (+0x328,
	// pointer +0x32c). Only BCreatureAndPilotPicker sets it, and only in team games; otherwise
	// it stays null.
	reg("CPilot::class CFaction Faction()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_object(o ? c.world.object(o->faction) : nullptr);
	});
	reg("CPilot::SetFaction(class CFaction)", [](const NativeContext &c) {
		GameObject *f = c.arg_object(0);
		if (auto *o = c.self()) o->faction = f ? f->handle : 0;
	});
	// PhotoCaller (0x35051ba0): +0x30c, from DE_CFaction::ObjProcessReferences (0x35051ef0): after
	// the creature's references, a v1 chunk [BOOL][photo reference][RGBA advanced reference].
	reg("CFaction::class CImage Photo()", [](const NativeContext &c) {
		GameObject *o = c.self();
		GameObject *img = nullptr;
		if (o && o->file) {
			try {
				auto d = o->file->object_data(o->file->objects()[o->index]);
				de::Contents rc(d.references);
				size_t last = 0;
				int version = 0;
				while (rc.remaining() > 1) {
					de::Chunk ch = de::begin_chunk(rc);
					last = rc.pos(), version = ch.version;
					rc.seek(ch.end);
				}
				rc.seek(last);
				if (version == 1 && rc.boolean()) {
					if (auto r = c.world.data().resolve(*o->file, de::Reference::read(rc))) img = c.world.global_object(r);
				}
			} catch (const std::exception &) {
			}
		}
		c.ret_object(img);
	});
	reg("CActor::SetGlobalInterest(float)", [](const NativeContext &c) { if (auto *o = c.self()) o->global_interest = c.arg<float>(0); });
	// DE_CCreature::IsAlive: GlobalInterest() != 0 (a fighter drops its interest when it dies).
	reg("CCreature::bool IsAlive()", [](const NativeContext &c) { auto *o = c.self(); c.ret_bool(o && c.world.global_interest_of(o) != 0); });
	// DE_COMF2Fighter::GlobalInterest (0x3502ae40): see World::global_interest_of.
	reg("CActor::float GlobalInterest()", [](const NativeContext &c) { c.ret_f32(global_interest(c.world, c.self())); });
	// AttackBestEnemy(current, aggression) (0x3504d370): over the owners of the creature's known
	// needs, distance / (the need's DesireLevel(4) + 0.01), the current target's distance weighed
	// by how long it has been the target ((start - now) / 3500 ms: below 1, x (1 - t/2)), a fighter
	// whose pilot is loved x Valor x 2 and one hated / (Anger x 2); the least wins among those with
	// global interest > 0. Then Attack.
	reg("CPilot::AttackBestEnemy(class CCreature,float)", [](const NativeContext &c) {
		GameObject *pilot = c.self();
		GameObject *self = pilot ? fighter_of(pilot->parent) : nullptr;
		if (!self || !c.world.need_owners) return;
		float now = time_ms(c.world), clock = now;
		const Pilot *stats = pilot_of(self);
		GameObject *best = nullptr;
		float best_score = 100000;
		auto p = self->world().t;
		auto owners = c.world.need_owners(self);
		for (auto it = owners.rbegin(); it != owners.rend(); ++it) {
			GameObject *o = it->first;
			float d = distance(p, o->world().t);
			if (o->handle == pilot->pilot_target) {
				float t = (pilot->pilot_target_since - now) / 3500.0f;
				if (t < 1) d *= 1 - t * 0.5f;
			}
			if (o->fighter) {
				GameObject *their = nullptr;
				for (auto *ch : o->children) {
					if (ch->class_path == "Actor\\Creature\\Pilot") their = ch;
				}
				auto listed = [&](const std::vector<uint32_t> &l) { return their && std::find(l.begin(), l.end(), their->handle) != l.end(); };
				// CVariedStat values at the clock advanced by each stat's phase, as the original does.
				if (listed(pilot->love) && stats) d = stats->stat(0x7a8, clock) * d * 2;
				if (listed(pilot->hate) && stats) d = d / (stats->stat(0x768, clock) * 2);
			}
			float score = d / (it->second + 0.01f);
			if ((score < best_score || !best) && global_interest(c.world, o) > 0) {
				best = o;
				best_score = score;
			}
		}
		if (best) attack(c.world, *pilot, *self, *best, c.arg<float>(1));
	});
	// Love / hate lists (pilot +0x348 / +0x35c; LoveCaller, AddIfAbsentHateCaller, ...).
	for (auto [name, field] : {std::pair{"Love", &GameObject::love}, std::pair{"Hate", &GameObject::hate}}) {
		auto f = field;
		std::string n = name;
		reg("CPilot::AddIfAbsent" + n + "(class CPilot)", [f](const NativeContext &c) {
			GameObject *o = c.self(), *q = c.arg_object(0);
			if (o && q && std::find((o->*f).begin(), (o->*f).end(), q->handle) == (o->*f).end()) (o->*f).push_back(q->handle);
		});
		reg("CPilot::Add" + n + "(class CPilot)", [f](const NativeContext &c) {
			GameObject *o = c.self(), *q = c.arg_object(0);
			if (o && q) (o->*f).push_back(q->handle);
		});
		reg("CPilot::Remove" + n + "(class CPilot)", [f](const NativeContext &c) {
			GameObject *o = c.self(), *q = c.arg_object(0);
			if (o && q) (o->*f).erase(std::remove((o->*f).begin(), (o->*f).end(), q->handle), (o->*f).end());
		});
		reg("CPilot::RemoveAll" + n + "()", [f](const NativeContext &c) {
			if (GameObject *o = c.self()) (o->*f).clear();
		});
		reg("CPilot::int Num" + n + "()", [f](const NativeContext &c) {
			GameObject *o = c.self();
			c.ret_u32(o ? uint32_t((o->*f).size()) : 0);
		});
		reg("CPilot::class CPilot " + n + "(int)", [f](const NativeContext &c) {
			GameObject *o = c.self();
			int32_t i = c.arg<int32_t>(0);
			c.ret_object(o && i >= 0 && size_t(i) < (o->*f).size() ? c.world.object((o->*f)[size_t(i)]) : nullptr);
		});
	}

	// --- Pilot (each fighter's own DE_CPilot; game::Pilot) --------------------------------------
	reg("COMF2Fighter::class CPilot Pilot()", [](const NativeContext &c) {
		auto *o = c.self();
		if (!o) {
			c.ret_u32(0);
			return;
		}
		for (auto *ch : o->children) {
			if (ch->class_path == "Actor\\Creature\\Pilot") {
				c.ret_object(ch);
				return;
			}
		}
		c.ret_object(c.world.create_child(o, o->pilot ? o->pilot->name : "Pilot", "Actor\\Creature\\Pilot"));
	});
	// Normalized*Caller: stat / 100; the Clamped variants clamp to 0..1.
	const std::vector<std::pair<std::string, float PilotStats::*>> base_stats = {
		{"Power", &PilotStats::power}, {"Agility", &PilotStats::agility},
		{"Endurance", &PilotStats::endurance}, {"Focus", &PilotStats::focus}};
	for (auto &[n, field] : base_stats) {
		auto f = field;
		reg("CPilot::float Normalized" + n + "()", [f](const NativeContext &c) {
			const Pilot *p = pilot_of(c.self());
			c.ret_f32(p ? p->stats.*f / 100.0f : 0.5f);
		});
		reg("CPilot::float NormalizedClamped" + n + "()", [f](const NativeContext &c) {
			const Pilot *p = pilot_of(c.self());
			c.ret_f32(std::clamp(p ? p->stats.*f / 100.0f : 0.5f, 0.0f, 1.0f));
		});
	}
	// SetAIDifficultyOffsetCaller (OMF 0x35048...): +0x3e4, then CalculateAIValues again.
	reg("CPilot::SetAIDifficultyOffset(float)", [](const NativeContext &c) {
		GameObject *f = fighter_of(c.self());
		if (f && f->pilot) {
			auto *p = const_cast<Pilot *>(f->pilot);
			p->difficulty_offset = c.arg<float>(0);
			p->apply_difficulty();
		}
	});
	// NormalizedAIQualityDegredationCaller: +0x7cc + +0x3d0 (the offset Attack sets).
	reg("CPilot::float NormalizedAIQualityDegredation()", [](const NativeContext &c) {
		GameObject *o = c.self();
		const Pilot *p = pilot_of(o);
		c.ret_f32((p ? p->quality : 0.43f) + (o ? o->quality_offset : 0));
	});
	// GetStat*: the varied stats at their DE_CPilot offsets (the *GetValueCaller functions).
	const std::vector<std::pair<const char *, uint32_t>> ai_stats = {
		{"AffinityLeft", 0x3e8}, {"AffinityRight", 0x408}, {"AffinityLight", 0x428}, {"AffinityHeavy", 0x448},
		{"AffinityPunch", 0x468}, {"AffinityKick", 0x488}, {"AffinitySpecial", 0x4a8}, {"AffinityMelee", 0x4e8},
		{"AffinityPreimptiveAttack", 0x508}, {"AffinityBackAttack", 0x528}, {"AffinityCounter", 0x548},
		{"AffinityJump", 0x568}, {"AffinityGrapple", 0x588}, {"AffinityCarryable", 0x5a8},
		{"AffinityPowerup", 0x5c8}, {"AffinityAttackRange", 0x5e8}, {"AffinityVariation", 0x608},
		{"AffinityMovementUrgency", 0x628}, {"AffinityEvade", 0x648}, {"AffinityMercy", 0x668},
		{"AffinityDefendObjective", 0x688}, {"PerceptionDistance", 0x6a8}, {"PerceptionTiming", 0x6c8},
		{"PerceptionTargetAngle", 0x6e8}, {"PerceptionSelfAngle", 0x708}, {"EmotionExcitability", 0x728},
		{"EmotionCalmDown", 0x748}, {"EmotionAnger", 0x768}, {"EmotionFear", 0x788}, {"EmotionValor", 0x7a8}};
	for (auto &[n, off] : ai_stats) {
		uint32_t o = off;
		reg(std::string("CPilot::float GetStat") + n + "()", [o](const NativeContext &c) {
			const Pilot *p = pilot_of(c.self());
			c.ret_f32(p ? p->stat(o, time_ms(c.world)) : 1.0f);
		});
	}

	// --- Fighter AI (OMF DE_COMF2Fighter::AI*) --------------------------------------------------
	reg("COMF2Fighter::float AIAttackAffinity(dword)", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_f32(o ? attack_affinity(c.world, *o, c.arg<uint32_t>(0)) : 0);
	});
	// DE_CPilot::TestAttackAffinity (0x3504d0c0): attack when affinity x confidence > 1, and
	// count the attack's categories as used ((quality x 0.2 + 0.7) x the variation stat each).
	reg("COMF2Fighter::bool AITestAttackAffinity(float,dword)", [](const NativeContext &c) {
		auto *o = c.self();
		if (!o) {
			c.ret_bool(false);
			return;
		}
		uint32_t flags = c.arg<uint32_t>(1);
		float aff = attack_affinity(c.world, *o, flags);
		if (getenv("OPENBG_DEBUG_AI")) {
			fprintf(stderr, "test attack %s frame %u: confidence %.3f flags %x affinity %.3f\n", o->name.c_str(),
			        c.world.frame(), c.arg<float>(0), flags, aff);
		}
		if (aff * c.arg<float>(0) <= 1) {
			c.ret_bool(false);
			return;
		}
		float bump = (o->pilot ? o->pilot->quality : 0.43f) * 0.2f + 0.7f;
		float variety = o->pilot ? o->pilot->stat(0x608, time_ms(c.world)) : 1.0f;
		for (int i = 0; i < 22; ++i) {
			if (flags & (1u << i)) {
				o->attack_usage[size_t(i)] += bump * variety;
			}
		}
		c.ret_bool(true);
	});
	reg("COMF2Fighter::float AIAttackRange()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_f32(o ? attack_range(c.world, *o) : 40);
	});
	reg("COMF2Fighter::AISetAttackRangeMultiplier(float)", [](const NativeContext &c) {
		if (auto *o = c.self()) o->ai_range_multiplier = c.arg<float>(0);
	});
	// DE_COMF2Fighter::FindDesirableAttackPosition (0x3502b980): no target -> stay (0); target
	// in view -> the best spot 80 units around; view blocked -> head for the target (1).
	reg("COMF2Fighter::float FindDesirableAttackPosition(class CVector)", [](const NativeContext &c) {
		auto *o = c.self();
		uint32_t v = c.arg<uint32_t>(0);
		if (!o) {
			c.ret_f32(0);
			return;
		}
		std::array<float, 3> out = o->world().t;
		float r = 0;
		if (GameObject *t = pilot_target(c.world, *o)) {
			if (c.world.segment_blocked(focal_point(*o), focal_point(*t))) {
				out = t->world().t;
				r = 1;
			} else {
				r = find_desirable_move(c.world, *o, 80, out);
			}
		}
		write_vec(c.mem(), v, out);
		c.ret_f32(r);
	});
	reg("COMF2Fighter::int AIDifficulty()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_u32(o && o->pilot ? uint32_t(o->pilot->difficulty) : 2);
	});

	reg("CObject::BInterface Interface(BGlobalID)", [](const NativeContext &c) {
		c.ret_u32(c.world.interface_for(c.self(), c.arg<uint32_t>(0)));
	});

	reg("CObject::bool IsDebuggingActive()", [](const NativeContext &c) { c.ret_bool(false); });

	reg("CObject::float Random(float)", [](const NativeContext &c) { c.ret_f32(random01(c.world) * c.arg<float>(0)); });
	// MATH::CenterRand: -x/2 .. x/2; MATH::Rand(int): 0 .. n-1.
	reg("CObject::float CenterRandom(float)", [](const NativeContext &c) {
		c.ret_f32((random01(c.world) - 0.5f) * c.arg<float>(0));
	});
	reg("CObject::int Random(int)", [](const NativeContext &c) {
		c.ret_u32(uint32_t(int32_t(random01(c.world) * float(c.arg<int32_t>(0)))));
	});
}

}  // namespace game
