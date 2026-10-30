#include "game/runtime.h"
#include "game/impulses.h"

#include "de/value.h"

#include "game/forces.h"
#include "game/music.h"
#include "game/particles.h"
#include "game/observer.h"
#include "game/simple_physics.h"
#include "game/navigation.h"
#include "game/replay.h"
#include "game/session.h"

#include <algorithm>
#include <set>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>
#include <new>
#include <sys/mman.h>

#include "vm/compiler.h"
#include "vm/disasm.h"

namespace game {

namespace {

constexpr uint32_t kObjectBlock = 0x100;  // script-visible block per object
constexpr uint32_t kWorldPosOffset = 0x70;  // as DE_CActor (WorldPositionCaller returns this + 0x70)
constexpr uint32_t kLocalPosOffset = 0x80;

uint32_t type_size(const std::string &t) {
	if (t == "byte" || t == "char") {
		return 1;
	}
	if (t == "short" || t == "word") {
		return 2;
	}
	return 4;  // int, dword, float, bool, string, class pointers, BRef, Method(...)
}

// Parameter types of the final parenthesised group, e.g. "Set(float,float,float)",
// "(opp =)(string)", "AddEventRelay(class X,Method(class CEvent))".
std::vector<std::string> param_types(const std::string &sig) {
	std::vector<std::string> out;
	if (sig.empty() || sig.back() != ')') {
		return out;
	}
	int depth = 0;
	size_t open = std::string::npos;
	for (size_t i = sig.size(); i-- > 0;) {
		if (sig[i] == ')') {
			++depth;
		} else if (sig[i] == '(' && --depth == 0) {
			open = i;
			break;
		}
	}
	if (open == std::string::npos) {
		return out;
	}
	std::string inner = sig.substr(open + 1, sig.size() - open - 2);
	depth = 0;
	std::string cur;
	for (char ch : inner) {
		if (ch == '(') {
			++depth;
		} else if (ch == ')') {
			--depth;
		}
		if (ch == ',' && depth == 0) {
			out.push_back(cur);
			cur.clear();
		} else {
			cur += ch;
		}
	}
	if (!cur.empty()) {
		out.push_back(cur);
	}
	return out;
}

}  // namespace

de::Transform GameObject::world() const {
	de::Transform t = local;
	for (const GameObject *p = parent; p; p = p->parent) {
		if (p->is_actor) {
			t = p->local.then(t);
		}
	}
	return t;
}

// A call from outside the brain (a hook, an event relay, another brain's interface call) that
// switched its state runs the new state's entry at once -- up to its first wait -- rather than
// on the brain's next pulse: scripts rely on it (BLastManStanding's FighterDefeated state drops
// the defeated fighter before its Update hook counts who is left).
void settle(World &w, Brain &b, uint64_t before) {
	if (b.exec->generation() == before || b.exec->executing()) {
		return;
	}
	try {
		b.exec->pulse();  // the prologue (op 1) suspends once
		b.exec->pulse();
	} catch (const std::exception &e) {
		w.log.push_back(b.owner->name + ": " + e.what());
		b.inst.shut_down = true;
	}
}

bool Brain::call(World &w, const std::string &name, std::vector<uint8_t> &args) {
	vm::ScriptMethod *m = cls.find_by_name(name);
	if (!m) {
		return false;
	}
	args.resize(std::max<size_t>(args.size(), m->return_size() + m->param_size()));
	uint64_t gen = exec->generation();
	try {
		exec->call(*m, args);
	} catch (const std::exception &e) {
		w.log.push_back(owner->name + ": " + name + ": " + e.what());
		return false;
	}
	settle(w, *this, gen);
	return true;
}

bool Brain::call_handle(World &w, uint32_t handle, std::vector<uint8_t> &args) {
	vm::ScriptMethod *m = nullptr;
	for (auto &cand : cls.methods) {
		if (cand.handle == handle) {
			m = &cand;
			break;
		}
	}
	if (!m) {
		return false;
	}
	if (m->vslot >= 0) {
		if (auto *v = cls.resolve_virtual(uint32_t(m->vslot), exec->current_state())) {
			m = v;
		}
	}
	// `args` holds the parameters only; the frame starts with the method's return slot.
	std::vector<uint8_t> frame(m->return_size(), 0);
	frame.insert(frame.end(), args.begin(), args.end());
	frame.resize(std::max<size_t>(frame.size(), m->return_size() + m->param_size()));
	uint64_t gen = exec->generation();
	try {
		exec->call(*m, frame);
	} catch (const std::exception &e) {
		w.log.push_back(owner->name + ": " + m->cm->signature + ": " + e.what());
		return false;
	}
	settle(w, *this, gen);
	return true;
}

vm::Memory &NativeContext::mem() const { return world.memory(); }
GameObject *NativeContext::owner() const {
	Brain *b = world.brain_of(ex);
	return b ? b->owner : nullptr;
}
const Input &NativeContext::input() const {
	static const Input neutral;
	GameObject *o = owner();
	return !o ? neutral : (o->user_controlled ? world.input : o->ai_input);
}
GameObject *NativeContext::self() const { return world.object(mem().get<uint32_t>(args)); }
uint32_t NativeContext::self_handle() const { return mem().get<uint32_t>(args); }

template <typename T>
T NativeContext::arg(size_t i) const {
	if (i >= params.size()) {
		return T{};
	}
	uint32_t a = args + params[i].second;
	if constexpr (sizeof(T) == 1) {
		return T(mem().get<uint8_t>(a));
	} else if constexpr (sizeof(T) == 2) {
		return T(mem().get<int16_t>(a));
	} else {
		return mem().get<T>(a);
	}
}
template int32_t NativeContext::arg<int32_t>(size_t) const;
template uint32_t NativeContext::arg<uint32_t>(size_t) const;
template float NativeContext::arg<float>(size_t) const;
template uint8_t NativeContext::arg<uint8_t>(size_t) const;
template int16_t NativeContext::arg<int16_t>(size_t) const;

GameObject *NativeContext::arg_object(size_t i) const { return world.object(arg<uint32_t>(i)); }
std::string NativeContext::arg_string(size_t i) const { return world.read_string(arg<uint32_t>(i)); }
void NativeContext::ret_u32(uint32_t v) const { mem().set<uint32_t>(args, v); }
void NativeContext::ret_f32(float v) const { mem().set<float>(args, v); }

ObjectArena::ObjectArena() {
	const size_t size = size_t(16) << 30;
	void *p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (p == MAP_FAILED) throw std::bad_alloc();
	base_ = next_ = committed_ = static_cast<char *>(p);
	end_ = base_ + size;
}

ObjectArena::~ObjectArena() { munmap(base_, size_t(end_ - base_)); }

void *ObjectArena::take(size_t bytes) {
	bytes = (bytes + 63) & ~size_t(63);
	if (bytes > size_t(end_ - next_)) throw std::bad_alloc();
	char *p = next_;
	next_ += bytes;
	if (next_ > committed_) {
		const size_t step = size_t(64) << 20;
		size_t grow = (size_t(next_ - committed_) + step - 1) / step * step;
		grow = std::min(grow, size_t(end_ - committed_));
		if (mprotect(committed_, grow, PROT_READ | PROT_WRITE) != 0) throw std::bad_alloc();
		committed_ += grow;
	}
	return p;
}

World::World(const std::filesystem::path &dir) : data_(dir) {
	particles = std::make_shared<ParticleSystem>(*this);
	forces = std::make_shared<ForceSystem>(*this);
	music = std::make_shared<MusicSystem>(*this);
	simple_physics = std::make_shared<SimplePhysicsSystem>(*this);
	observers = std::make_shared<ObserverSystem>(*this);
	impulses = std::make_shared<ImpulseRobot>(*this);
	navigation = std::make_shared<Navigation>(*this);
	// Line of sight and segment tests against the arena's fighting surfaces (DE_CScene::
	// TestLineSegment on group 0x200000).
	segment_blocked = [this](const std::array<float, 3> &a, const std::array<float, 3> &b) { return segment(*this, a, b, 0x200000).has_value(); };
	segment_hit = [this](const std::array<float, 3> &a, const std::array<float, 3> &b) -> std::optional<std::array<float, 3>> {
		if (auto c = segment(*this, a, b, 0x200000)) return c->point;
		return std::nullopt;
	};
	register_core_natives(*this);
	register_ai_natives(*this);
	register_combat_natives(*this);
	register_transform_natives(*this);
	register_particle_natives(*this);
	register_session_natives(*this);
	register_script_natives(*this);
	register_force_natives(*this);
	register_grapple_natives(*this);
	register_creature_natives(*this);
	register_need_natives(*this);
	register_music_natives(*this);
	register_simple_physics_natives(*this);
	register_observer_natives(*this);
	// OPENBG_LIST_NATIVES=<file>: every registered native key, one a line (for the docs' inventory).
	if (const char *path = getenv("OPENBG_LIST_NATIVES")) {
		if (FILE *f = fopen(path, "w")) {
			for (auto &[k, fn] : natives_) fprintf(f, "%s\n", k.c_str());
			fclose(f);
		}
	}
}

uint32_t World::class_handle(vm::Executor &ex, uint32_t index) {
	auto &classes = ex.current_class()->code.classes;
	return index < classes.size() ? string(classes[index].name) : 0;
}

std::string World::class_path_of_handle(uint32_t h) {
	std::string n = read_string(h);
	if (n.rfind("CObject::", 0) == 0) n = n.substr(9);
	if (!n.empty() && n[0] == 'C') n = n.substr(1);
	auto it = class_paths().find(n);
	return it == class_paths().end() ? std::string() : it->second;
}

std::vector<GameObject *> World::scene_objects() {
	std::vector<GameObject *> out;
	std::vector<GameObject *> todo(scene_roots.begin(), scene_roots.end());
	std::set<GameObject *> seen;
	for (size_t i = 0; i < todo.size(); ++i) {
		if (!todo[i] || !seen.insert(todo[i]).second || !by_handle_.count(todo[i]->handle)) continue;
		// Out of the scene with its branch: removed (CScene::Remove), not at this detail level, or
		// (below a root) parked inactive.
		if (todo[i]->removed || (todo[i]->is_actor && !de::shown_at_detail(todo[i]->actor_flags))) continue;
		if (todo[i]->parent && parked(*todo[i])) continue;
		out.push_back(todo[i]);
		todo.insert(todo.end(), todo[i]->children.begin(), todo[i]->children.end());
	}
	return out;
}

bool World::has_behavior_property(GameObject *o, uint32_t h) {
	std::string path = class_path_of_handle(h);
	if (path.empty()) return false;
	for (auto *ch : o->children) {
		if (ch->is(path.c_str())) return true;
	}
	return false;
}

void World::clean_later(GameObject *o) {
	if (o && std::find(to_clean_.begin(), to_clean_.end(), o->handle) == to_clean_.end()) to_clean_.push_back(o->handle);
}

// DE_GLOBAL::HouskeepingObjects: the queued objects are destroyed, the last queued first.
void World::housekeeping() {
	auto queued = std::move(to_clean_);
	to_clean_.clear();
	for (auto it = queued.rbegin(); it != queued.rend(); ++it) {
		if (GameObject *o = object(*it)) destroy(o);
	}
}

void World::destroy(GameObject *o) {
	if (!o) return;
	replay_destroying(*this, *o);
	end_session_object(*this, o);
	if (o->parent) {
		auto &v = o->parent->children;
		v.erase(std::remove(v.begin(), v.end(), o), v.end());
		o->parent = nullptr;
	}
	scene_roots.erase(std::remove(scene_roots.begin(), scene_roots.end(), o), scene_roots.end());
	std::vector<GameObject *> branch = {o};
	for (size_t i = 0; i < branch.size(); ++i) {
		branch.insert(branch.end(), branch[i]->children.begin(), branch[i]->children.end());
	}
	std::set<uint32_t> gone;
	for (GameObject *b : branch) {
		by_handle_.erase(b->handle);
		relays.erase(b);
		root_session.erase(b);
		b->spawned = false;
		b->active = false;
		if (b->brain) b->brain->inst.shut_down = true;
		gone.insert(b->handle);
	}
	// Its primitives (text, quads, bars) go with it.
	for (auto it = texts.begin(); it != texts.end();) {
		if (gone.count(it->first->handle) || (it->second.mesh && gone.count(it->second.mesh->handle))) {
			screen_space_meshes.erase(it->second.mesh);
			it = texts.erase(it);
		} else {
			++it;
		}
	}
	// RefNotifyDestroyingTarget: reference members (DE_CSingleReference, ops 62/64) naming a
	// destroyed object read as null from now on.
	for (auto &bp : brains_) {
		for (uint32_t &r : bp->inst.ref_members) {
			if (gone.count(r)) r = 0;
		}
	}
}

GameObject *World::object(uint32_t h) const {
	auto it = by_handle_.find(h);
	return it == by_handle_.end() ? nullptr : it->second;
}

GameObject *World::create(const std::string &name, const std::string &class_path) {
	for (auto &o : objects_) {
		if (!o.file && o.name == name) {
			return &o;
		}
	}
	GameObject &g = objects_.emplace_back();
	g.name = name;
	g.class_path = class_path;
	g.handle = mem_.alloc(kObjectBlock);
	g.vec_world_pos = g.handle + kWorldPosOffset;
	g.vec_local_pos = g.handle + kLocalPosOffset;
	g.is_actor = g.is("Actor");
	by_handle_[g.handle] = &g;
	return &g;
}

std::vector<de::ObjectRef> World::referenced_behaviors(const de::ObjectRef &behavior) const {
	std::vector<de::ObjectRef> out;
	try {
		auto d = behavior.file->object_data(behavior.info());
		de::Contents c(d.references);
		de::Chunk obj = de::begin_chunk(c);
		c.seek(obj.end);
		de::Chunk outer = de::begin_chunk(c);
		de::Chunk globals = de::begin_chunk(c);
		c.seek(globals.end);
		de::Chunk pcodes = de::begin_chunk(c);
		while (c.pos() < pcodes.end && c.boolean()) {
			de::Reference r = de::Reference::read(c);
			if (de::ObjectRef t = data_.resolve(*behavior.file, r)) {
				out.push_back(t);
			}
		}
		(void)outer;
	} catch (const std::exception &) {
	}
	return out;
}

// DE_CPCodeGlobal::ProcessReferences (Core): the globals chunk (v2) holds, per global entry, a
// BOOL and -- when set -- the reference to the object it names.
std::vector<de::ObjectRef> World::behavior_globals(const de::ObjectRef &behavior) const {
	std::vector<de::ObjectRef> out;
	try {
		auto d = behavior.file->object_data(behavior.info());
		de::Contents c(d.references);
		de::Chunk obj = de::begin_chunk(c);
		c.seek(obj.end);
		de::begin_chunk(c);
		de::Chunk globals = de::begin_chunk(c);
		if (globals.version == 2) {
			while (c.pos() < globals.end) {
				out.push_back(c.boolean() ? data_.resolve(*behavior.file, de::Reference::read(c)) : de::ObjectRef{});
			}
		}
	} catch (const std::exception &) {
	}
	return out;
}

uint32_t World::behavior_id(const de::ObjectRef &behavior) {
	auto key = std::make_pair(behavior.file, behavior.index);
	auto it = behavior_ids_.find(key);
	if (it != behavior_ids_.end()) {
		return it->second;
	}
	uint32_t id = uint32_t(behavior_ids_.size()) + 1;
	behavior_ids_[key] = id;
	return id;
}

GameObject *World::create_child(GameObject *parent, const std::string &name, const std::string &class_path) {
	GameObject &g = objects_.emplace_back();
	g.name = name;
	g.class_path = class_path;
	g.handle = mem_.alloc(kObjectBlock);
	g.vec_world_pos = g.handle + kWorldPosOffset;
	g.vec_local_pos = g.handle + kLocalPosOffset;
	g.is_actor = g.is("Actor");
	g.parent = parent;
	if (parent) {
		parent->children.push_back(&g);
	}
	by_handle_[g.handle] = &g;
	return &g;
}

GameObject *World::copy_tree(const GameObject &o) {
	GameObject &g = objects_.emplace_back(o);
	g.copied = true;
	g.handle = mem_.alloc(kObjectBlock);
	memcpy(mem_.ptr(g.handle, kObjectBlock), mem_.ptr(o.handle, kObjectBlock), kObjectBlock);
	g.vec_world_pos = g.handle + kWorldPosOffset;
	g.vec_local_pos = g.handle + kLocalPosOffset;
	g.parent = nullptr;
	g.brain = nullptr;
	g.momentum_vec = 0;
	g.orientation_block = 0;
	g.local_orientation_block = 0;
	g.ai_vec = {0, 0};
	g.children.clear();
	g.detached = false;
	g.data_encompasser = nullptr;
	g.data_encompasser_read = false;
	by_handle_[g.handle] = &g;
	for (const GameObject *ch : o.children) {
		GameObject *cc = copy_tree(*ch);
		cc->parent = &g;
		g.children.push_back(cc);
	}
	return &g;
}

void World::pose_skeleton(GameObject &f) {
	if (!f.bank || ((f.sequence < 0 || size_t(f.sequence) >= f.bank->sequences.size()) && !f.captured_blend)) {
		return;
	}
	const de::AnimBank &bank = *f.bank;
	std::vector<de::Transform> pose;
	auto valid = [&](int q) { return q >= 0 && size_t(q) < bank.sequences.size(); };
	if (f.captured_blend && valid(f.capture_initial.sequence) && valid(f.capture_final.sequence)) {
		pose = de::blend_poses(bank.pose(bank.sequences[size_t(f.capture_initial.sequence)], f.capture_initial.time),
		                       bank.pose(bank.sequences[size_t(f.capture_final.sequence)], f.capture_final.time),
		                       std::clamp(f.anim_time, 0.0f, 1.0f));
	} else if (f.prelude && !f.pose_captured.empty()) {
		// A prelude blends from the pose captured when it was activated to the sequence's frame.
		pose = de::blend_poses(f.pose_captured, bank.pose(bank.sequences[size_t(f.sequence)], f.prelude_time), std::clamp(f.anim_time, 0.0f, 1.0f));
	} else {
		pose = bank.pose(bank.sequences[size_t(f.sequence)], f.prelude ? f.prelude_time : f.anim_time);
	}
	f.pose_now = pose;
	// A bone actor another manager also drives takes the body's pose only from a stream of the
	// sequence the body is moving to (DE_CAnimationSequenceManager::SetTime leaves the rest).
	const int target = f.captured_blend ? f.capture_final.sequence : f.sequence;
	auto streamed = [&](int ch) {
		if (!valid(target)) return false;
		for (auto &s : bank.sequences[size_t(target)].streams) {
			if (s.channel == ch) return true;
		}
		return false;
	};
	// Walk the fighter's actor tree in its model space; bone actors take their channel's pose,
	// everything else (hit detectors, effect anchors) keeps its offset from its parent.
	std::function<void(GameObject &, const de::Transform &)> walk = [&](GameObject &o, const de::Transform &parent) {
		for (GameObject *ch : o.children) {
			if (!ch->is_actor) {
				continue;
			}
			if (ch->channel == -2) {
				ch->channel = bank.channel(ch->name);
			}
			de::Transform model;
			if (ch->channel >= 0 && size_t(ch->channel) < pose.size() && (!ch->managed || streamed(ch->channel))) {
				model = pose[size_t(ch->channel)];
				ch->local = parent.inverse().then(model);
			} else {
				model = parent.then(ch->local);
			}
			walk(*ch, model);
		}
	};
	walk(f, de::Transform{});
}

const de::AnimBank *World::script_bank(vm::Executor &ex) {
	Brain *b = brain_of(ex);
	if (!b) return nullptr;
	if (b->owner->bank) return b->owner->bank;
	for (auto &gr : b->globals) {
		if (!gr || gr.info().class_path != "AnimationSequenceGroup") continue;
		auto &slot = global_banks_[{gr.file, gr.index}];
		if (!slot) {
			try {
				slot = std::make_unique<de::AnimBank>(de::read_anim_bank(*gr.file, gr.info()));
			} catch (const std::exception &) {
				slot = std::make_unique<de::AnimBank>();
			}
		}
		return slot.get();
	}
	return nullptr;
}

void World::manager_activate(GameObject &m, const de::AnimBank *bank, int sequence, bool prelude, float t) {
	if (bank && bank != m.manager_bank) {
		// Each channel's actor, by name under the manager's parent (its references' actor lists).
		m.manager_bank = bank;
		m.manager_actors.assign(bank->channels.size(), nullptr);
		std::function<GameObject *(GameObject *, const std::string &)> find = [&](GameObject *o, const std::string &n) -> GameObject * {
			for (GameObject *c : o->children) {
				if (c->is_actor && c->name == n) return c;
				if (GameObject *r = find(c, n)) return r;
			}
			return nullptr;
		};
		for (size_t i = 0; i < bank->channels.size() && m.parent; ++i) {
			if ((m.manager_actors[i] = find(m.parent, bank->channels[i].name))) m.manager_actors[i]->managed = true;
		}
	}
	++m.activations;
	m.anim_time = 0;
	m.prelude = false;
	m.sequence = sequence;
	if (!prelude) return;
	// The channels' current locals, the sequence's at t over them, then no active sequence.
	auto locals = [&] {
		std::vector<de::Transform> v(m.manager_actors.size());
		for (size_t i = 0; i < v.size(); ++i) {
			if (m.manager_actors[i]) v[i] = m.manager_actors[i]->local;
		}
		return v;
	};
	m.manager_initial = locals();
	manager_set_time(m, t);
	m.manager_final = locals();
	m.sequence = -1;
	m.prelude = true;
}

void World::manager_set_time(GameObject &m, float t) {
	m.anim_time = t;
	if (!m.manager_bank) return;
	const de::AnimBank &bank = *m.manager_bank;
	if (m.sequence >= 0 && size_t(m.sequence) < bank.sequences.size()) {
		// Only channels with a stream are written; the others keep their locals.
		for (size_t i = 0; i < m.manager_actors.size(); ++i) {
			de::Transform local;
			if (m.manager_actors[i] && bank.local(bank.sequences[size_t(m.sequence)], i, t, local)) m.manager_actors[i]->local = local;
		}
	} else if (m.prelude && m.manager_initial.size() == m.manager_actors.size()) {
		// SetEmptyTime: each channel from its initial to its final local, t unclamped.
		auto now = de::blend_poses(m.manager_initial, m.manager_final, t);
		for (size_t i = 0; i < m.manager_actors.size(); ++i) {
			if (m.manager_actors[i]) m.manager_actors[i]->local = now[i];
		}
	}
}

void World::validate_animation(GameObject &f) {
	if (f.pose_valid) return;
	f.pose_valid = true;
	pose_skeleton(f);
	// The body rotation turns the Pelvis (its orientation and its place) about the fighter's axes.
	static const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
	if (f.anim_rot != identity) {
		for (GameObject *ch : f.children) {
			if (ch->name != "Pelvis") continue;
			de::Transform turn;
			turn.r = f.anim_rot;
			ch->local = turn.then(ch->local);
			break;
		}
	}
	f.anim_rot = identity;
}

std::vector<de::Transform> World::current_pose(GameObject &f) {
	std::vector<de::Transform> pose = f.pose_now;
	if (pose.empty()) return pose;
	de::Transform inv = f.world().inverse();
	std::function<void(GameObject &)> walk = [&](GameObject &o) {
		for (GameObject *ch : o.children) {
			if (!ch->is_actor) continue;
			if (ch->channel >= 0 && size_t(ch->channel) < pose.size()) pose[size_t(ch->channel)] = inv.then(ch->world());
			walk(*ch);
		}
	};
	walk(f);
	return pose;
}

GameObject *World::object_for(const de::ObjectsFile *f, uint32_t i) const {
	auto it = by_source_.find({f, i});
	return it == by_source_.end() ? nullptr : it->second;
}

uint32_t World::string(const std::string &s) {
	auto it = strings_.find(s);
	if (it != strings_.end()) {
		return it->second;
	}
	uint32_t a = mem_.alloc_copy(s.c_str(), uint32_t(s.size() + 1));
	strings_[s] = a;
	return a;
}

std::string World::read_string(uint32_t a) {
	if (!a) {
		return {};
	}
	return reinterpret_cast<const char *>(mem_.ptr(a));
}

GameObject &World::make_object(const de::ObjectsFile *f, uint32_t i) {
	auto &objs = f->objects();
	GameObject &g = objects_.emplace_back();
	g.file = f;
	g.index = i;
	g.name = objs[i].name;
	g.class_path = objs[i].class_path;
	g.handle = mem_.alloc(kObjectBlock);
	g.vec_world_pos = g.handle + kWorldPosOffset;
	g.vec_local_pos = g.handle + kLocalPosOffset;
	g.is_actor = g.is("Actor");
	if (g.is_actor) {
		try {
			de::ActorInfo a = de::read_actor(*f, objs[i]);
			g.local = a.local;
			g.position_mode = a.position_mode;
			g.actor_flags = a.flags;
			g.active = (a.flags & 8) != 0;  // +0x110 bit 8: active (else parked under its parent)
		} catch (const std::exception &) {
		}
	}
	if (g.is("Actor\\Affector") || g.is("UniqueActorProperty\\Danger")) {
		read_affector(g, data_, *f, objs[i]);
	}
	if (g.is("Actor\\Creature\\OMF2Fighter")) {
		if (auto st = read_fighter_stats(*f, objs[i])) {
			g.fighter = true;
			g.stats = *st;
		}
	}
	by_handle_[g.handle] = &g;
	by_source_[{f, i}] = &g;
	return g;
}

// A global entry's target (a template such as BFighter's DangerSphere): that object and its
// children, kept out of the scene until a copy is parented into it.
GameObject *World::global_object(const de::ObjectRef &r) {
	if (!r) return nullptr;
	if (auto *o = object_for(r.file, r.index)) return o;
	auto parents = r.file->parents();
	std::vector<GameObject *> made(parents.size(), nullptr);
	made[r.index] = &make_object(r.file, r.index);
	made[r.index]->detached = true;
	for (bool grew = true; grew;) {
		grew = false;
		for (uint32_t i = 0; i < parents.size(); ++i) {
			if (made[i] || parents[i] < 0 || !made[size_t(parents[i])] || object_for(r.file, i)) continue;
			made[i] = &make_object(r.file, i);
			made[i]->parent = made[size_t(parents[i])];
			made[i]->parent->children.push_back(made[i]);
			grew = true;
		}
	}
	return made[r.index];
}

uint32_t World::global_entry(vm::Executor &ex, uint32_t index) {
	Brain *b = brain_of(ex);
	if (!b || index >= b->globals.size()) return 0;
	GameObject *o = global_object(b->globals[index]);
	return o ? o->handle : 0;
}

GameObject *World::instantiate_file(const std::string &stem) {
	const de::ObjectsFile *f = data_.file(stem);
	if (!f) {
		return nullptr;
	}
	auto &objs = f->objects();
	std::vector<GameObject *> made(objs.size());
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (auto *existing = object_for(f, i)) {
			made[i] = existing;
			continue;
		}
		made[i] = &make_object(f, i);
	}
	auto parents = f->parents();
	GameObject *root = nullptr;
	for (uint32_t i = 0; i < objs.size(); ++i) {
		if (parents[i] >= 0 && !made[i]->parent) {
			made[i]->parent = made[size_t(parents[i])];
			made[size_t(parents[i])]->children.push_back(made[i]);
		} else if (!root) {
			root = made[i];
		}
	}
	return root;
}

Brain *World::attach_brain(GameObject *owner, const de::ObjectRef &cls_obj, const std::string &entry) {
	auto d = cls_obj.file->object_data(cls_obj.info());
	auto vmc2 = cls_obj.file->ex_payload(d, "VMC2");
	auto behavior_refs = vm::class_behavior_refs(d.references);
	vm::VMClass code;
	if (vmc2) {
		code = vm::VMClass::parse(vmc2->data, behavior_refs.size());
	} else {
		// DE_CClass::PrepareForExecution compiles a class shipped without VMC2 from its sources.
		auto key = std::make_pair(cls_obj.file, cls_obj.index);
		auto it = compiled_classes_.find(key);
		if (it == compiled_classes_.end()) {
			std::vector<std::string> notes;
			vm::VMClass compiled = vm::compile_class(data_, cls_obj, &notes);
			for (auto &n : notes) log.push_back("compiler: " + n);
			it = compiled_classes_.emplace(key, std::move(compiled)).first;
		}
		code = it->second;
	}
	auto b = std::make_unique<Brain>();
	b->owner = owner;
	static const bool debug_attach = getenv("OPENBG_DEBUG_ATTACH") != nullptr;
	if (debug_attach) fprintf(stderr, "[attach %u] %s on %s%s\n", frame_, cls_obj.info().name.c_str(), owner->name.c_str(), vmc2 ? "" : " (compiled)");
	vm::load_class(b->cls, mem_, d.contents, std::move(code));
	// The class's behaviors (Behavior objects, possibly in other files) and their global IDs.
	for (auto &r : behavior_refs) {
		de::ObjectRef br = data_.resolve(*cls_obj.file, r);
		b->behaviors.push_back(br ? behavior_id(br) : 0);
		b->behavior_names.push_back(br ? br.info().name : "?");
	}
	// The class's visible behaviors (op 26 indexes them): the attached behaviors, then -- from the
	// most-derived behavior back to the first -- every behavior they reference, depth first,
	// each added once. (Recovered by aligning op 26 operands with the source's Interface(B...)
	// calls: e.g. PJaguar 9 BLaser, 13 BLauncher, 15 BFighterStats, 20 BStandardOMFSession.)
	{
		std::vector<de::ObjectRef> list;
		for (auto &r : behavior_refs) {
			list.push_back(data_.resolve(*cls_obj.file, r));
		}
		auto known = [&](const de::ObjectRef &t) {
			for (auto &x : list) {
				if (x && x.file == t.file && x.index == t.index) return true;
			}
			return false;
		};
		std::function<void(const de::ObjectRef &)> visit = [&](const de::ObjectRef &beh) {
			for (const de::ObjectRef &t : referenced_behaviors(beh)) {
				if (!known(t)) {
					list.push_back(t);
					visit(t);
				}
			}
		};
		const size_t attached = list.size();
		b->attached = attached;
		for (size_t i = attached; i-- > 0;) {
			if (list[i]) visit(list[i]);
		}
		for (size_t i = attached; i < list.size(); ++i) {
			b->behaviors.push_back(behavior_id(list[i]));
			b->behavior_names.push_back(list[i].info().name);
		}
		// Op 8 (DE_CClass::GlobalEntry) numbers the global entries of these behaviors in turn.
		for (auto &beh : list) {
			if (!beh) continue;
			for (auto &g : behavior_globals(beh)) b->globals.push_back(g);
		}
	}
	if (getenv("OPENBG_DEBUG_BEHAVIORS")) {
		for (auto &r : behavior_refs) {
			de::ObjectRef br = data_.resolve(*cls_obj.file, r);
			if (!br) continue;
			fprintf(stderr, "  %s uses:", br.info().name.c_str());
			for (auto &t : referenced_behaviors(br)) fprintf(stderr, " %s", t.info().name.c_str());
			fprintf(stderr, "\n");
		}
		for (size_t i = 0; i < b->behavior_names.size(); ++i) {
			fprintf(stderr, "%s behavior %zu: %s (id %u)\n", cls_obj.info().name.c_str(), i, b->behavior_names[i].c_str(),
			        b->behaviors[i]);
		}
	}
	uint32_t extent = 0;
	for (auto &m : b->cls.methods) {
		for (auto &ins : vm::decode(m.cm->code)) {
			if (ins.opcode == 65 && ins.args.size() >= 3) {
				extent = std::max(extent, uint32_t(ins.args[0] + ins.args[2]));
			} else if (ins.opcode == 66 && ins.args.size() >= 3) {
				extent = std::max(extent, uint32_t(ins.args[1] + ins.args[2]));
			} else if (((ins.opcode >= 53 && ins.opcode <= 59) || ins.opcode == 27 || ins.opcode == 29) && !ins.args.empty()) {
				extent = std::max(extent, uint32_t(ins.args[0] + 4));
			}
		}
	}
	for (auto &beh : b->cls.code.behaviors) {
		for (auto &ci : beh.instances) extent = std::max(extent, ci.value + 4);
	}
	b->inst.cls = &b->cls;
	b->inst.members_size = extent + 64;
	b->inst.members = mem_.alloc(b->inst.members_size);
	b->inst.self = owner->handle;
	b->inst.handle = mem_.alloc(kObjectBlock);
	b->exec = std::make_unique<vm::Executor>(mem_, *this, b->inst);
	// Behavior-level "instance" members (BehaviorCode instances; offsets into the member stack).
	for (auto &beh : b->cls.code.behaviors) {
		for (auto &ci : beh.instances) {
			if (!ci.flag && ci.value + 4 <= b->inst.members_size) {
				const std::string &cname = ci.class_index < b->cls.code.classes.size() ? b->cls.code.classes[ci.class_index].name : "";
				mem_.set<uint32_t>(b->inst.members + ci.value, make_instance(cname, ci.count));
			}
		}
	}
	vm::ScriptMethod *start = b->cls.find_by_name(entry + "()");
	if (!start) {
		throw std::runtime_error("no entry " + entry + " in " + b->cls.name);
	}
	b->exec->start(*start);
	// The object's behaviour is its first executor (DE_CCreature +0x1a0, GatherBehaviorInstances).
	Brain *made = b.get();
	if (!owner->brain) owner->brain = made;
	brains_.push_back(std::move(b));
	return made;
}

std::map<std::string, InputSequence> read_input_bank(const de::ObjectsFile &f, const de::ObjectInfo &o) {
	std::map<std::string, InputSequence> out;
	de::Contents c(f.object_data(o).contents);
	de::Chunk ch = de::begin_chunk(c);
	if (ch.version != 1) {
		return out;
	}
	auto text = [&] {
		auto b = c.block();
		std::string s(b.begin(), b.end());
		while (!s.empty() && s.back() == 0) {
			s.pop_back();
		}
		return s;
	};
	text();
	int32_t n = c.i32();
	for (int32_t i = 0; i < n; ++i) {
		std::string name = text();
		InputSequence seq(size_t(c.i32()));
		for (auto &r : seq) {
			r = {c.u16(), c.u16(), c.u16(), c.u16(), c.u16(), c.u16(), c.u16()};
		}
		out[name] = std::move(seq);
	}
	return out;
}

float VariedStat::value(float t) const {
	static const float k[4] = {0.001f, 0.002f, 0.004f, 0.015f};
	float x = (t + phase) * frequency;
	float w = wave < 4 ? std::sin(x * k[wave]) : x;
	return base + w * amplitude;
}

void Pilot::apply_difficulty() {
	quality = std::clamp(1.0f - (float(difficulty) + difficulty_offset) / 3.5f, 0.01f, 0.99f);
	// Perception: VariationAroundNormalized(phase, frequency, quality), then + quality x 0.4.
	static const float around[9] = {0, 0.1f, 0.2f, 0.3f, 0.45f, 0.6f, 0.75f, 0.9f, 1.0f};
	const std::pair<uint32_t, std::pair<float, float>> perception[4] = {
		{0x6a8, {5125, 1.21f}}, {0x6c8, {9842, 1.3154f}}, {0x6e8, {7231, 1.12f}}, {0x708, {8041, 1.01145f}}};
	for (auto &[off, pf] : perception) {
		VariedStat &v = ai[off];
		v.phase = pf.first, v.frequency = pf.second, v.base = 1;
		v.amplitude = (v.level < 9 ? around[v.level] : 1.0f) * quality + quality * 0.4f;
	}
}

float Pilot::stat(uint32_t offset, float t) const {
	auto it = ai.find(offset);
	return it == ai.end() ? 1.0f : it->second.value(t);
}

std::optional<Pilot> read_pilot(const de::ObjectsFile &f, const de::ObjectInfo &o, int difficulty) {
	de::Contents c(f.object_data(o).contents);
	size_t body = 0, end = 0;
	int version = -1;
	while (c.remaining() > 1) {
		de::Chunk ch = de::begin_chunk(c);
		if (ch.end > c.size() || ch.end < c.pos()) {
			return std::nullopt;
		}
		version = ch.version, body = c.pos(), end = ch.end;
		c.seek(ch.end);
	}
	// v4: 3 x {CValueRGBA::CAdvancedReference, f32}, the four stats, then 31 AI stat descriptors
	// {u8, squeezed level, variation, wave} (DE_CPilot::ObjProcessContents 0x3504b450). The stats
	// are rejected unless they look like stats.
	if (version != 4) {
		return std::nullopt;
	}
	Pilot p;
	c.seek(body);
	try {
		for (auto &col : p.colours) {
			auto v = de::read_embedded_color(c);
			col = {v.rgba[0], v.rgba[1], v.rgba[2]};
			c.f32();
		}
	} catch (const std::exception &) {
		return std::nullopt;
	}
	if (c.pos() + 16 > end) {
		return std::nullopt;
	}
	p.name = o.name;
	p.stats = {c.f32(), c.f32(), c.f32(), c.f32()};
	for (float v : {p.stats.power, p.stats.agility, p.stats.endurance, p.stats.focus}) {
		if (!(v >= 0 && v <= 200)) {
			return std::nullopt;
		}
	}
	static const uint32_t order[31] = {0x3e8, 0x408, 0x428, 0x448, 0x468, 0x488, 0x4a8, 0x4c8, 0x4e8, 0x508, 0x528,
	                                   0x548, 0x568, 0x588, 0x5a8, 0x5c8, 0x5e8, 0x6a8, 0x6c8, 0x6e8, 0x708, 0x728,
	                                   0x748, 0x768, 0x788, 0x7a8, 0x608, 0x628, 0x648, 0x668, 0x688};
	try {
		for (uint32_t off : order) {
			c.u8();
			VariedStat &v = p.ai[off];
			v.level = c.squeezed();
			v.variation = c.squeezed();
			v.wave = c.squeezed();
		}
	} catch (const std::exception &) {
		return std::nullopt;
	}
	p.difficulty = difficulty;

	// CalculateAIValues.
	static const double level_table[9] = {0, 0.1, 0.3, 0.6, 1.0, 1.3, 1.6, 2.0, 3.0};
	static const float variation_table[6] = {0, 0.1f, 0.2f, 0.4f, 0.7f, 1.0f};
	auto setup_variation = [](VariedStat &v) {
		v.amplitude = v.variation < 6 ? v.base * variation_table[v.variation] : v.base;
	};
	auto normalized = [&](uint32_t off, float phase, float freq, double exponent) {
		VariedStat &v = p.ai[off];
		v.phase = phase, v.frequency = freq;
		if (v.level < 9) {
			v.base = float(std::pow(level_table[v.level], exponent));
		}
		setup_variation(v);
	};
	normalized(0x3e8, 7965, 1.23f, 0.5);
	normalized(0x408, 2115, 1.0912f, 0.5);
	normalized(0x448, 1525, 1.291f, 0.5);
	normalized(0x428, 9612, 1.0145f, 0.5);
	normalized(0x468, 1152, 1.04f, 0.5);
	normalized(0x488, 7684, 1.3215f, 0.5);
	normalized(0x4a8, 5421, 1.512f, 0.5);
	normalized(0x4c8, 4421, 1.142f, 0.5);
	normalized(0x4e8, 7652, 0.93f, 0.5);
	normalized(0x508, 5132, 1.212f, 0.5);
	normalized(0x528, 5968, 1.512f, 0.5);
	normalized(0x548, 512, 1.3221f, 0.5);
	normalized(0x568, 5151, 1.12411f, 0.5);
	normalized(0x588, 9051, 1.19212f, 0.5);
	normalized(0x5a8, 3451, 0.5f, 0.7);
	normalized(0x5c8, 1691, 0.124f, 0.5);
	normalized(0x5e8, 1245, 2500, 1.0);
	{  // attack range: the level picks a distance
		static const float range[9] = {30, 50, 75, 120, 160, 220, 300, 400, 600};
		VariedStat &v = p.ai[0x5e8];
		if (v.level < 9) {
			v.base = range[v.level];
		}
		setup_variation(v);
	}
	normalized(0x608, 1562, 0.1f, 0.5);
	normalized(0x628, 0, 0.145f, 1.0);
	{  // movement urgency: SetBase table
		static const float urgency[9] = {0.3f, 0.5f, 0.75f, 1.0f, 1.3f, 1.7f, 2.0f, 3.5f, 6.0f};
		VariedStat &v = p.ai[0x628];
		if (v.level < 9) {
			v.base = urgency[v.level];
		}
	}
	normalized(0x648, 0, 0.1f, 1.0);
	normalized(0x668, 0, 3.1f, 1.0);
	normalized(0x688, 1512, 12412, 1.5);
	normalized(0x728, 1562, 0.052f, 1.0);
	normalized(0x748, 590, 0.057f, 1.0);
	normalized(0x768, 1562, 0.05f, 1.0);
	normalized(0x788, 562, 0.061f, 1.0);
	normalized(0x7a8, 673, 0.069f, 1.0);
	p.apply_difficulty();
	return p;
}

// Contents chunks: [actor] [affector v3: u32 groups, BOOLs] [shape]. Box v2: min, max corners.
// Sphere v2: hotspot, falloff
// (DE_CSphereAffector::ObjProcessContents). Cone v2: far begin, far end, near begin, near end,
// umbra, penumbra (DE_CConeAffector::ObjProcessContents, setters 0x1a8, 0x1a4, 0x1a0, 0x19c,
// 0x1ac, 0x1b0).
void read_affector(GameObject &g, const de::World &data, const de::ObjectsFile &f, const de::ObjectInfo &o) {
	try {
		de::Contents c(f.object_data(o).contents);
		std::vector<std::pair<size_t, size_t>> chunks;
		std::vector<uint8_t> versions;
		while (c.remaining() > 1) {
			de::Chunk ch = de::begin_chunk(c);
			if (ch.end > c.size() || ch.end < c.pos()) break;
			chunks.emplace_back(c.pos(), ch.end);
			versions.push_back(ch.version);
			c.seek(ch.end);
		}
		// DE_CDanger::ObjProcessContents (Creature 0x1502d930): its v1 chunk holds the level
		// (+0x50, 1 by default) and the delay (+0x54).
		if (g.class_path.rfind("UniqueActorProperty\\Danger", 0) == 0) {
			g.danger_level = 1;
			if (chunks.size() >= 2 && versions[1] == 1) {
				c.seek(chunks[1].first);
				g.danger_level = c.f32();
				g.danger_delay = c.f32();
			}
			return;
		}
		if (chunks.size() < 3) return;
		c.seek(chunks[1].first);
		g.groups = c.u32();
		if (versions[1] == 3) {
			c.boolean();
			g.encompasser = c.boolean();
			g.auto_encompass = c.boolean();
		}
		c.seek(chunks[2].first);
		if (g.class_path == "Actor\\Affector\\Sphere" && versions[2] == 2) {
			g.shape = GameObject::Shape::Sphere;
			g.hotspot = c.f32();
			g.falloff = c.f32();
		} else if (g.class_path == "Actor\\Affector\\Cone" && versions[2] == 2) {
			g.shape = GameObject::Shape::Cone;
			g.far_begin = c.f32(), g.far_end = c.f32(), g.near_begin = c.f32(), g.near_end = c.f32();
			g.umbra = c.f32(), g.penumbra = c.f32();
		} else if (g.class_path == "Actor\\Affector\\Box" && versions[2] == 2) {
			g.shape = GameObject::Shape::Box;
			for (auto &v : g.box_min) v = c.f32();
			for (auto &v : g.box_max) v = c.f32();
		} else if (g.class_path == "Actor\\Affector\\Mesh") {
			g.shape = GameObject::Shape::Mesh;
			// DE_CMeshAffector::ObjProcessReferences: references chunk 3 (v2) holds a v1 chunk
			// { BOOL, mesh reference }.
			de::Contents r(f.object_data(o).references);
			for (int k = 0; k < 3 && r.remaining() > 1; ++k) r.seek(de::begin_chunk(r).end);
			de::Chunk mc = de::begin_chunk(r);
			if (mc.version == 2) {
				de::Chunk inner = de::begin_chunk(r);
				if (inner.version == 1 && r.boolean()) g.mesh = data.resolve(f, de::Reference::read(r));
			}
		}
	} catch (const std::exception &) {
	}
}

std::optional<FighterStats> read_fighter_stats(const de::ObjectsFile &f, const de::ObjectInfo &o) {
	// Contents are one versioned chunk per class, base first; the fighter's own chunk is last.
	de::Contents c(f.object_data(o).contents);
	size_t body = 0, end = 0;
	int version = -1;
	while (c.remaining() > 1) {
		de::Chunk ch = de::begin_chunk(c);
		if (ch.end > c.size() || ch.end < c.pos()) {
			return std::nullopt;
		}
		version = ch.version;
		body = c.pos();
		end = ch.end;
		c.seek(ch.end);
	}
	if (version != 2 || end - body < 30 * 4 || end > c.size()) {
		return std::nullopt;
	}
	c.seek(body);
	float v[30];
	for (float &x : v) {
		x = c.f32();
	}
	// Extraction order: 4c4 4c8 4cc 4d0 4d4 4d8 4dc 4e0 4e4 4e8 4ec 4f0 4f4 4f8 4fc 500 504 508
	// 530 534 538 53c 50c 510 514 518 51c 520 524 528; 4e0 and 4f0 are then recomputed as 0.75x,
	// 4c8 as 0.6x.
	FighterStats s;
	s.run = v[0], s.run_turning = v[0] * 0.6f, s.run_turn_max = v[2], s.run_turn_step = v[3];
	s.turn_max = v[4], s.turn_step = v[5];
	s.walk = v[6], s.walk_turning = v[6] * 0.75f, s.walk_turn_max = v[8], s.walk_turn_step = v[9];
	s.back = v[10], s.back_turning = v[10] * 0.75f, s.back_turn_max = v[12], s.back_turn_step = v[13];
	s.air_turn_max = v[14], s.air_turn_step = v[15], s.air_forward = v[16], s.air_backward = v[17];
	s.jump_forward = v[24], s.jump_backward = v[25], s.jump_up = v[26];
	s.super_forward = v[27], s.super_backward = v[28], s.super_up = v[29];
	s.push_size = v[22];
	s.walk_lean = v[18], s.run_lean = v[20], s.lean_step = v[21];
	if (c.pos() + 6 <= end) {
		if (uint16_t m = c.u16()) {  // +0x44c block directions (0 -> 0xf01f)
			s.block_mask = m;
		}
		s.air_gravity = c.f32();
		if (c.pos() + 24 * 4 <= end) {
			for (float &x : s.trailer_vectors) x = c.f32();
		}
	}
	return s;
}

std::array<float, 3> focal_point(const GameObject &o) {
	if (o.fighter) {
		std::vector<const GameObject *> todo(o.children.begin(), o.children.end());
		for (size_t i = 0; i < todo.size(); ++i) {
			if (todo[i]->name == "Pelvis") return todo[i]->world().t;
			todo.insert(todo.end(), todo[i]->children.begin(), todo[i]->children.end());
		}
	}
	return o.world().t;
}

int World::orientation() const {
	if (input_history.empty()) {
		return 8;
	}
	uint16_t d = input_history.back().directions;
	for (int b : {0, 1, 7, 4, 3, 5, 2, 6}) {  // Get2DOrientation's test order
		if (d & (1u << b)) {
			return b;
		}
	}
	return 8;
}

bool World::verify(const InputSequence &seq) const {
	if (seq.empty()) {
		return true;
	}
	// Mirrors DE_CJoystickInputSequence::Verify: a record can satisfy several consecutive
	// requirements; a requirement's age/gap bounds, once met, stay met while it moves on to
	// older records that still match its buttons and directions.
	// A record's counter is its age: DE_CJoystickInputManager::Tick inserts new records at 0 and
	// increments every older record once per tick (16 ms).
	auto age = [&](size_t hi) { return input_ticks_ - input_history[hi].start; };
	int ri = int(seq.size()) - 1;
	auto unbounded_age = [&](const InputRequirement &r) { return r.min_age == 0 && r.max_age == 0; };
	auto unbounded_gap = [&](const InputRequirement &r) { return r.min_gap == 0 && r.max_gap == 0; };
	bool age_ok = unbounded_age(seq[size_t(ri)]), gap_ok = unbounded_gap(seq[size_t(ri)]);
	uint32_t gap = 0;
	for (int hi = int(input_history.size()) - 1; hi >= 0; --hi) {
		const InputRecord &rec = input_history[size_t(hi)];
		uint32_t d = age(size_t(hi));
		for (;;) {
			const InputRequirement &r = seq[size_t(ri)];
			bool match = (rec.buttons & r.mask) == r.value &&
			             ((r.directions & 0x1ff) == 0 || (rec.directions & r.directions & 0x1ff) != 0);
			if (!match) {
				break;
			}
			age_ok = age_ok || (r.min_age <= d && (r.max_age == 0 || d <= r.max_age));
			gap_ok = gap_ok || (r.min_gap <= gap && (r.max_gap == 0 || gap <= r.max_gap));
			if (!age_ok || !gap_ok) {
				break;
			}
			if (--ri < 0) {
				return true;
			}
			age_ok = unbounded_age(seq[size_t(ri)]);
			gap_ok = unbounded_gap(seq[size_t(ri)]);
		}
		gap = d;
	}
	return false;
}

// DE_CScene::TemporalElementSetForRate: the set of a rate, made on first use with its last tick at
// (now / rate) x rate, so every element of a rate ticks together on multiples of it.
World::TemporalSet &World::temporal_set(uint32_t rate, int scene) {
	for (auto &s : temporal_sets_) {
		if (s.rate == rate && s.scene == scene) return s;
	}
	uint32_t now = scene ? gui_scene_ms[scene - 1] : scene_ms_;
	TemporalSet s;
	s.rate = rate;
	s.scene = scene;
	s.next = now / rate * rate + rate;
	temporal_sets_.push_back(std::move(s));
	return temporal_sets_.back();
}

// The scene an element's object is in (DE_CScene::AddTemporal on its own scene).
int World::temporal_scene(const Temporal &t) {
	const GameObject *o = t.kind == Temporal::Brain ? (t.brain ? t.brain->owner : nullptr) : t.kind == Temporal::Creature ? object(t.handle) : nullptr;
	return o && in_gui(*o) ? std::clamp(gui_view(*o), 0, 1) + 1 : 0;
}

void World::add_temporal(uint32_t rate, Temporal t) {
	if (rate == 0) return;
	TemporalSet &s = temporal_set(rate, temporal_scene(t));
	for (auto &e : s.elements) {
		if (e.kind == t.kind && e.handle == t.handle && e.brain == t.brain) return;
	}
	static const bool debug = getenv("OPENBG_DEBUG_TEMPORALS") != nullptr;
	if (debug) {
		const char *kinds[] = {"creature", "executor", "tester", "input"};
		std::string who = t.brain && t.brain->owner ? t.brain->owner->name : t.handle && object(t.handle) ? object(t.handle)->name : "-";
		fprintf(stderr, "[temporal %u] %s %s every %u ms\n", scene_ms_, kinds[t.kind], who.c_str(), rate);
	}
	s.elements.push_back(t);
}

// The creature whose behaviour executor (+0x1a0, its first) a brain is: ticked by the creature.
static bool is_creature_behaviour(World &w, const Brain &b) {
	if (!b.owner || !b.owner->is("Actor\\Creature") || w.creature_rate(b.owner) == 0) return false;
	for (const Brain *o : w.brains()) {
		if (o->owner == b.owner) return o == &b;
	}
	return false;
}

// Elements join their sets as the scene meets them: creatures with a SceneTicksPerUpdate
// (DE_CCreature::ObjPrepare), executors with an interval (DE_CClassExecutorInstance::Setup 0x70144df0:
// AddTemporal(scene, instance, executor +0x44) once it has resumed; none without one).
void World::register_temporals() {
	if (!input_registered_) {
		input_registered_ = true;
		add_temporal(16, {Temporal::Input, 0, nullptr});  // DE_CJoystickInputManager +0x10c
	}
	for (GameObject *o : scene_objects()) register_creature(o);
	for (auto &bp : brains_) {
		Brain *b = bp.get();
		if (b->started) register_brain(b);
	}
}

// DE_CCreature::ObjPrepare (Creature 0x15016050): a creature with a SceneTicksPerUpdate joins
// its set.
void World::register_creature(GameObject *o) {
	// DE_CJoystickInputManager::ObjPrepare (Creature): each manager (a fighter's controller copy)
	// is a temporal of its +0x10c (16 ms).
	if (o->is("CreatureController\\JoystickInputManager") && !o->temporal_registered) {
		o->temporal_registered = true;
		add_temporal(16, {Temporal::Input, o->handle, nullptr});
		return;
	}
	{
		if (o->temporal_registered || !o->is("Actor\\Creature")) return;
		uint32_t rate = creature_rate(o);
		o->temporal_registered = true;
		if (rate == 0) {
			if (creature_delta_ticked(*o)) delta_creatures.push_back(o->handle);
			return;
		}
		add_temporal(rate, {Temporal::Creature, o->handle, nullptr});
		// DE_CCreature::ObjPrepare (Creature 0x15016050): one made earlier starts at the tick its
		// creation time gives.
		if (o->creation_time && o->creation_time < scene_ms_) o->creature_ticks = (scene_ms_ - o->creation_time) / rate;
		// DE_COMF2Fighter::ObjPrepare (OMF): every fighter already in the scene, in the factory's
		// order, joins this one's list (+0x334), and this one joins theirs.
		if (o->fighter && !o->fighter_prepared) {
			o->fighter_prepared = true;
			// DE_COMF2Fighter::ObjPrepare: a pilot with a faction gives the team.
			for (GameObject *ch : o->children) {
				if (ch->is("Actor\\Creature\\Pilot") && ch->faction) {
					o->team = faction_team(ch->faction);
					break;
				}
			}
			o->spawned = true;  // in play from here: physics, commands, pushes, drawing
			for (GameObject *g : fighters()) {
				if (g == o || !g->fighter_prepared || !in_scene(*g)) continue;
				o->linked_fighters.push_back(g->handle);
				g->linked_fighters.push_back(o->handle);
			}
		}
	}
}

// DE_CClassExecutorInstance::Setup (0x70144df0): once its entry has run, an executor with an
// interval joins its set. DE_CClassExecutor::ObjPrepare (0x70143840) always hands Setup a scene,
// so it does so even when it is also its creature's behaviour (+0x1a0), ticked by both.
void World::register_brain(Brain *b) {
	if (b->temporal_registered) return;
	b->temporal_registered = true;
	add_temporal(b->interval, {Temporal::Brain, 0, b});
}

void World::set_user_controlled(GameObject &creature, int32_t device) {
	creature.user_controlled = device != -1 && !all_ai;
	for (GameObject *c : creature.children) {
		if (c->class_path.rfind("CreatureController\\", 0) == 0) c->input_device = creature.user_controlled ? device : -1;
	}
}

// DE_CScene::Add (Core 0x7006ed50) prepares an actor, then adds its children, the last first:
// creatures join their sets in that order.
void World::register_branch(GameObject *o) {
	if (!o) return;
	register_creature(o);
	for (auto it = o->children.rbegin(); it != o->children.rend(); ++it) {
		if (!parked(**it)) register_branch(*it);
	}
}

static void pulse(World &w, Brain *b) {
	if (!b || b->inst.shut_down || !b->exec->running()) return;
	if (b->owner && b->owner->is_actor && !in_scene(*b->owner)) return;  // parked or removed: not ticked
	try {
		b->exec->pulse();
	} catch (const std::exception &e) {
		w.log.push_back(b->owner->name + ": " + e.what());
		b->inst.shut_down = true;
	}
}

// DE_COMF2Fighter::Tick (OMF 0x350253a0) over DE_CCreature::Tick (Creature 0x15016870): the
// command manager (+0x118), the physics (+0x1cc), the behaviour executor (+0x1a0); then the
// fighter's guide and attack trailer points, and for a computer-controlled fighter every 64th
// update a step of its attack-range base.
void World::creature_tick(GameObject &o) {
	if (o.fighter) {
		validate_animation(o);  // as InternalWorldUpdate does when something needs positions
		if (!temporal_updating) run_commands(o);
	}
	// A replay's held creature, or one re-taking its state for this tick, skips physics and brain.
	bool skip = creature_state_tick(*this, o);
	if (o.fighter && !skip) physics_step(o);
	for (auto &bp : brains_) {
		if (bp->owner == &o) {
			if (!skip && is_creature_behaviour(*this, *bp)) pulse(*this, bp.get());
			break;
		}
	}
	++o.creature_ticks;
	if (o.fighter) {
		if (!skip && o.invulnerable_frames > 0) --o.invulnerable_frames;
		if (!replay_held(*this, o)) {
			guide(o);
			particles->tick_trailers(o);
		}
		jitter_attack_range(o);
	}
}

void World::retick(GameObject &o, uint32_t at_ms) {
	at_scene_ms(at_ms, [&] { creature_tick(o); });
}

void World::at_scene_ms(uint32_t at_ms, const std::function<void()> &f) {
	uint32_t now = scene_ms_;
	scene_ms_ = at_ms;
	f();
	scene_ms_ = now;
}

// DE_CJoystickInputManager::Tick (Creature 0x150020c0): a record per input change; older records
// age by one per tick. Only the manager bound to a device records: the user's fighter's (the
// others' have none). `manager` 0 is the stand-in for a user fighter without one.
void World::input_tick(uint32_t manager) {
	// The fighters' managers share one history; another bound pad (the observer's view-switching
	// UserController) keeps only its last record, which is all Get2DOrientation reads.
	auto directions = [this] {
		int dir = -1;  // bit index: 0 up, 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left, 6 left, 7 up-left
		if (input.up && input.right) dir = 1;
		else if (input.down && input.right) dir = 3;
		else if (input.down && input.left) dir = 5;
		else if (input.up && input.left) dir = 7;
		else if (input.up) dir = 0;
		else if (input.right) dir = 2;
		else if (input.down) dir = 4;
		else if (input.left) dir = 6;
		return uint16_t(dir >= 0 ? 1u << dir : 0x100u);
	};
	auto records = [](const GameObject *x) {
		if (!x || x->input_device < 0) return false;
		for (const GameObject *p = x; p; p = p->parent) {
			if (p->fighter) return true;
		}
		return false;
	};
	if (manager) {
		GameObject *m = object(manager);
		if (m && m->input_device >= 0 && !records(m)) m->pad_record = directions();
		if (!records(m)) return;  // no device bound: nothing to record
	} else {
		for (auto &s : temporal_sets_) {
			for (auto &e : s.elements) {
				if (e.kind != Temporal::Input || !e.handle) continue;
				if (records(object(e.handle))) return;  // the bound one records
			}
		}
	}
	++input_ticks_;
	uint16_t buttons = uint16_t((input.left_punch ? 1 : 0) | (input.right_punch ? 2 : 0) | (input.left_kick ? 4 : 0) |
	                            (input.right_kick ? 8 : 0) | (input.jump ? 0x10 : 0) | (input.duck_evade() ? 0x20 : 0));
	uint16_t dirs = directions();
	if (input_history.empty() || input_history.back().buttons != buttons || input_history.back().directions != dirs) {
		input_history.push_back({buttons, dirs, input_ticks_});
		if (input_history.size() > 64) {
			input_history.erase(input_history.begin());
		}
	}
}

// DE_CTemporalElementSet::AdvanceTime (Core 0x70072090): every element, newest first.
void World::advance(TemporalSet &set) {
	auto elements = set.elements;  // a tick may add elements
	for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
		switch (it->kind) {
		case Temporal::Creature:
			if (GameObject *o = object(it->handle); o && in_scene(*o)) creature_tick(*o);
			break;
		case Temporal::Brain: pulse(*this, it->brain); break;
		case Temporal::Tester: tick_collision_tester(*this, it->handle); break;
		case Temporal::Input: input_tick(it->handle); break;
		}
	}
}

// DE_CScene::PassTime (Core 0x70070040) when the clock jumps: each set restarts at its last tick
// before the new time and each element is set to it, newest first.
void World::reset_clock() {
	std::vector<uint32_t> creatures;
	for (auto &s : temporal_sets_) {
		if (s.scene || !s.rate) continue;
		s.next = scene_ms_ / s.rate * s.rate + s.rate;
		for (auto it = s.elements.rbegin(); it != s.elements.rend(); ++it) {
			if (it->kind == Temporal::Creature) creatures.push_back(it->handle);
		}
	}
	for (uint32_t h : creatures) {
		if (GameObject *o = object(h); o && in_scene(*o)) replay_set_time(*this, *o, scene_ms_);
	}
}

uint32_t World::scene_step(float real_ms) {
	replay_temporal_update(*this);
	scene_carry_ += double(real_ms) * time_scale + passed_ms;
	passed_ms = 0;
	if (scene_carry_ < 0) {
		uint32_t back = uint32_t(std::min<double>(-scene_carry_, scene_ms_));
		scene_ms_ -= back;
		reset_clock();
		// What measures time since it last ran must not have run in the future.
		simple_physics->clock_back(scene_ms_);
		for (auto &o : objects_) {
			o.interest_time = std::min(o.interest_time, scene_ms_);
			o.target_checked = std::min(o.target_checked, scene_ms_);
			o.target_chosen = std::min(o.target_chosen, scene_ms_);
		}
		scene_carry_ = 0;
		return 0;
	}
	uint32_t ms = uint32_t(scene_carry_);
	scene_carry_ -= ms;
	// A replay passes at most 2000 ms: the clock jumps over the rest.
	if (ms > 2000 && replaying(*this)) {
		scene_ms_ += ms - 2000;
		reset_clock();
		ms = 2000;
	}
	return ms;
}

uint32_t World::scene_ms_of(const GameObject *o) const {
	return o && in_gui(*o) ? gui_scene_ms[std::clamp(gui_view(*o), 0, 1)] : scene_ms_;
}

GameObject *World::scene_object(const GameObject *o) {
	if (o && in_gui(*o)) return gui_view(*o) == 1 ? create("GUIViewerScene", "Scene") : create("GUIScene", "Scene");
	return create("Scene", "Scene");
}

int World::gui_scene_view(const GameObject *scene) {
	if (!scene || scene->file) return -1;
	return scene->name == "GUIScene" ? 0 : scene->name == "GUIViewerScene" ? 1 : -1;
}

void World::tick_gui(int view, uint32_t ms) {
	uint32_t &now = gui_scene_ms[view];
	uint32_t left = ms;
	for (;;) {
		register_temporals();
		TemporalSet *first = nullptr;
		for (auto &s : temporal_sets_) {
			if (s.scene == view + 1 && (!first || s.next - now < first->next - now)) first = &s;
		}
		if (!first || first->next - now > left) break;
		left -= first->next - now;
		now = first->next;
		first->next += first->rate;
		// Scene().Add from a GUI script adds to that script's scene.
		std::vector<GameObject *> roots = scene_roots;
		advance(*first);
		start_pending_brains();
		for (GameObject *r : scene_roots) {
			if (!r->gui && std::find(roots.begin(), roots.end(), r) == roots.end()) r->gui = true, r->view = view;
		}
	}
	now += left;
}

void World::tick(uint32_t ms) {
	++frame_;
	frame_ms_ = ms;
	housekeeping();
	impulses->flush();
	// OPENBG_CALLS="Katana@400-402" (or "*@..."): log the methods brains of that owner enter.
	static const char *calls = getenv("OPENBG_CALLS");
	if (calls) {
		char who[64] = {};
		unsigned a = 0, b = 0;
		sscanf(calls, "%63[^@]@%u-%u", who, &a, &b);
		bool window = frame_ >= a && frame_ <= b;
		for (auto &bp : brains_) {
			bool on = window && bp->owner && (bp->owner->name == who || !strcmp(who, "*") ||
			                                  (bp->owner->parent && bp->owner->parent->name == who));
			bp->inst.trace_calls = on;
		}
		if (window) fprintf(stderr, "[call] --- frame %u\n", frame_);
	}
	replay_temporal_update(*this);
	// PassTime: the set whose next tick comes first (the first made, on a tie) advances, the
	// clock standing at that tick, until the next tick lies beyond the frame.
	uint32_t left = ms;
	for (;;) {
		register_temporals();
		TemporalSet *first = nullptr;
		for (auto &s : temporal_sets_) {
			if (!s.scene && (!first || s.next - scene_ms_ < first->next - scene_ms_)) first = &s;
		}
		if (!first || first->next - scene_ms_ > left) break;
		left -= first->next - scene_ms_;
		scene_ms_ = first->next;
		first->next += first->rate;
		advance(*first);
		forces->tick();
		start_pending_brains();
	}
	scene_ms_ += left;
	update_session(*this);
	simple_physics->tick();
	particles->step();
	start_pending_brains();
	housekeeping();
}

// Affector references: [object][actor properties][affector v4: BOOL owner actor, BOOL
// encompassing affector]...
GameObject *nearest_copy(GameObject *from, const de::ObjectRef &r) {
	std::function<GameObject *(GameObject *, GameObject *)> find = [&](GameObject *o, GameObject *skip) -> GameObject * {
		if (o->file == r.file && o->index == r.index) return o;
		for (GameObject *c : o->children) {
			if (c == skip) continue;
			if (GameObject *h = find(c, nullptr)) return h;
		}
		return nullptr;
	};
	GameObject *skip = nullptr;
	for (GameObject *p = from; p; skip = p, p = p->parent) {
		if (GameObject *h = find(p, skip)) return h;
	}
	return nullptr;
}

GameObject *World::affector_owner(GameObject *a) {
	if (!a) return nullptr;
	if (a->owner_actor) return a->owner_actor;
	if (!a->data_owner_read) {
		a->data_owner_read = true;
		if (a->file) {
			auto lists = reference_lists(data_, {a->file, a->index});
			if (lists.size() > 2 && !lists[2].empty() && !lists[2][0].empty() && lists[2][0][0]) {
				// The nearest copy: several copies of one template (a pickup on each spawner)
				// share the data object, so search outwards from the affector.
				a->data_owner = nearest_copy(a, lists[2][0][0]);
			}
		}
	}
	if (a->data_owner) return a->data_owner;
	for (GameObject *p = a; p; p = p->parent) {
		if (p->fighter) return p;
	}
	return a;
}

GameObject *World::encompasser_of(GameObject *a) {
	if (!a) return nullptr;
	if (!a->data_encompasser_read) {
		a->data_encompasser_read = true;
		if (a->file) {
			try {
				de::Contents c(a->file->object_data(a->file->objects()[a->index]).references);
				for (int k = 0; k < 2 && c.remaining() > 1; ++k) c.seek(de::begin_chunk(c).end);
				if (c.remaining() > 1 && de::begin_chunk(c).version == 4) {
					if (c.boolean()) de::Reference::read(c);
					if (c.boolean()) {
						if (de::ObjectRef r = data_.resolve(*a->file, de::Reference::read(c))) a->data_encompasser = nearest_copy(a, r);
					}
				}
			} catch (const std::exception &) {
			}
		}
	}
	if (a->data_encompasser && a->data_encompasser != a) return a->data_encompasser;
	if (!a->auto_encompass) return nullptr;
	for (GameObject *p = a->parent; p; p = p->parent) {
		if (p->shape != GameObject::Shape::None && p->encompasser) return p;
		for (auto it = p->children.rbegin(); it != p->children.rend(); ++it) {
			GameObject *ch = *it;
			if (ch != a && !parked(*ch) && ch->shape != GameObject::Shape::None && ch->encompasser) return ch;
		}
	}
	return nullptr;
}

void World::walk_affectors(uint32_t groups, const std::function<bool(GameObject *, bool)> &test) {
	std::vector<GameObject *> list = affectors();
	std::unordered_map<GameObject *, GameObject *> enc;
	std::unordered_map<GameObject *, uint32_t> all;
	std::unordered_map<GameObject *, std::vector<GameObject *>> subs;
	for (GameObject *a : list) {
		all[a] |= a->groups;
		GameObject *e = encompasser_of(a);
		if (!e) continue;
		enc[a] = e;
		// ObjPrepare / InternalAutoEncompase: the groups pass up the encompasser chain.
		size_t guard = 0;
		for (GameObject *x = e; x && x != a && guard < 16; x = enc.count(x) ? enc[x] : encompasser_of(x), ++guard) all[x] |= a->groups;
	}
	auto is_ancestor = [](const GameObject *p, const GameObject *o) {
		for (o = o->parent; o; o = o->parent) {
			if (o == p) return true;
		}
		return false;
	};
	std::vector<GameObject *> top;
	for (GameObject *a : list) {
		if (!all[a]) continue;
		auto it = enc.find(a);
		if (it == enc.end()) top.push_back(a);
		else subs[it->second].push_back(a);
	}
	// A sub-affector joins its encompasser's list when it is prepared: pre-order, children last first.
	for (auto &[e, v] : subs) {
		std::stable_sort(v.begin(), v.end(), [&](const GameObject *x, const GameObject *y) {
			if (is_ancestor(x, y)) return true;
			if (is_ancestor(y, x)) return false;
			return x->affector_seq < y->affector_seq;
		});
	}
	std::function<bool(GameObject *, int)> inner = [&](GameObject *t, int depth) {
		if (!(all[t] & groups) || depth > 16) return false;
		bool record = (t->groups & groups) != 0;
		if (!test(t, record)) return false;
		if (record) return true;
		bool any = false;
		auto it = subs.find(t);
		if (it == subs.end()) return false;
		for (auto s = it->second.rbegin(); s != it->second.rend(); ++s) any |= inner(*s, depth + 1);
		return any;
	};
	static const bool debug = getenv("OPENBG_DEBUG_ENCOMPASS") != nullptr;
	static uint32_t shown = ~0u;
	if (debug && frame_ % 300 == 1 && shown != frame_) {
		shown = frame_;
		for (auto &[e, v] : subs) {
			fprintf(stderr, "[encompass %u] %s shape %d under %s (own %#x, all %#x, %s):", frame_, e->name.c_str(), int(e->shape), e->parent ? e->parent->name.c_str() : "-", e->groups, all[e], enc.count(e) ? "sub" : "top");
			for (GameObject *x : v) fprintf(stderr, " %s(%#x)", x->name.c_str(), x->groups);
			fprintf(stderr, "\n");
		}
	}
	for (GameObject *t : top) inner(t, 0);
}

// Creature contents: [actor][creature v2-v4: u32 scene ticks per update, i32, float, flags...].
uint32_t World::creature_rate(GameObject *o) {
	for (; o; o = o->parent) {
		if (!o->is("Actor\\Creature")) continue;
		if (o->ticks_per_update < 0) {
			o->ticks_per_update = 0;
			if (o->file) {
				try {
					de::Contents c(o->file->object_data(o->file->objects()[o->index]).contents);
					c.seek(de::begin_chunk(c).end);
					de::Chunk ch = de::begin_chunk(c);
					if (ch.version >= 2 && ch.version <= 4) o->ticks_per_update = int32_t(c.u32());
				} catch (const std::exception &) {
				}
			}
		}
		return uint32_t(o->ticks_per_update);
	}
	return 0;
}

std::vector<GameObject *> World::fighters() {
	std::vector<GameObject *> out;
	for (auto &o : objects_) {
		if (o.fighter) out.push_back(&o);
	}
	return out;
}

// DE_CFighterPhysics::Tick without surfaces (arenas lacking fighting-surface affectors): gravity
// while airborne, surface friction on the floor, then a height field under the fighter.
// DE_CSimplePhysics::ApplySingleSurfaceFriction against a static surface: friction f = material
// x traction; slowdown = 0 while propelled, else 0.1 f. Momentum loses slowdown x min(f, 1) of
// itself per frame and stops once under slowdown x f.
void World::surface_friction(GameObject &o, float material) {
	// SurfaceFrictionSlowdown (0x3503a5b0) is 0 while SkipFriction's flag is set.
	float f = material * o.traction, slow = o.propelled || o.skip_friction ? 0 : 0.1f * f;
	float len = std::sqrt(o.momentum[0] * o.momentum[0] + o.momentum[1] * o.momentum[1] + o.momentum[2] * o.momentum[2]);
	if (f != 0) {
		if (slow * f <= len) {
			float k = f >= 1 ? slow : slow * f;
			for (auto &v : o.momentum) {
				v -= v * k;
			}
		} else {
			o.momentum = {0, 0, 0};
		}
	}
}

void World::physics_step(GameObject &o) {
	// DE_CFighterPhysics::Tick runs once prepared into the scene (+0x1a8) and enabled (+0x38).
	if (!o.spawned || !o.physics_enabled) {
		return;
	}
	static const bool debug_momentum = getenv("OPENBG_DEBUG_MOMENTUM") != nullptr;
	if (debug_momentum) {
		fprintf(stderr, "[momentum] frame %u %s (%.2f %.2f %.2f)\n", frame_, o.name.c_str(), o.momentum[0], o.momentum[1], o.momentum[2]);
	}
	// DE_CFighterPhysics::Tick against the arena's surface affectors.
	if (forces->fighter_tick(o)) {
		return;
	}
	// Stand-in without surfaces: a height field under the fighter.
	if (o.on_floor && o.momentum[1] > 1e-4f) {
		o.on_floor = false;  // the floor only stops downward motion: upward momentum lifts off
	}
	if (o.on_floor) {
		surface_friction(o);
		o.skip_friction = false;
	} else {
		o.momentum[1] -= 0.037f * o.gravity;
	}
	if (o.gravity < 1) {
		o.gravity = std::min(1.0f, o.gravity + o.gravity_recover);
	} else if (o.gravity > 1) {
		o.gravity = std::max(1.0f, o.gravity - o.gravity_recover);
	}
	o.propelled = false;
	float before = o.local.t[1];
	for (int k = 0; k < 3; ++k) {
		o.local.t[size_t(k)] += o.momentum[size_t(k)];
	}
	forces->push_against_fighters(o);
	float ground = floor(o.local.t[0], o.local.t[2], std::max(before, o.local.t[1]) + 8.0f);
	if (o.on_floor) {
		if (ground > -1e29f && ground >= o.local.t[1] - 3.0f) {
			o.local.t[1] = ground;  // follow the surface (steps up to 8, down to 3 units)
			o.momentum[1] = 0;
		} else {
			o.on_floor = false;  // walked off an edge
		}
	} else if (o.momentum[1] <= 0 && ground > -1e29f && o.local.t[1] <= ground) {
		o.local.t[1] = ground;
		o.momentum[1] = 0;
		o.on_floor = true;
	}
}

Brain *World::brain_of(vm::Executor &ex) const {
	for (auto &b : brains_) {
		if (b->exec.get() == &ex) {
			return b.get();
		}
	}
	return nullptr;
}

const World::Bound &World::bind(const vm::ScriptClass &cls, uint32_t index) {
	auto key = std::make_pair(&cls, index);
	auto it = bound_.find(key);
	if (it != bound_.end()) {
		return it->second;
	}
	Bound b;
	if (index < cls.code.intrinsics.size()) {
		auto &in = cls.code.intrinsics[index];
		b.key = cls.code.classes[in.class_index].name + "::" + in.signature;
		uint32_t off = 4;  // slot 0 is `this` / return value
		for (auto &t : param_types(in.signature)) {
			b.params.emplace_back(t, off);
			off += type_size(t);
		}
		auto n = natives_.find(b.key);
		if (n != natives_.end()) {
			b.fn = &n->second;
		}
	}
	return bound_[key] = std::move(b);
}

void World::intrinsic(vm::Executor &ex, uint32_t index, uint32_t args) {
	const Bound &b = bind(*ex.current_class(), index);
	if (!b.fn) {
		++missing_[b.key];
		mem_.set<uint32_t>(args, 0);
		return;
	}
	NativeContext ctx{*this, ex, args, b.params};
	(*b.fn)(ctx);
	// A CVector changed in place may be an actor's LocalPosition() block: the actor moves too.
	if (b.key.compare(0, 18, "CObject::CVector::") == 0) {
		uint32_t v = ctx.self_handle();
		if (GameObject *o = v > kLocalPosOffset ? object(v - kLocalPosOffset) : nullptr; o && o->vec_local_pos == v) {
			o->local.t = read_vec(mem_, v);
		}
	}
}

void World::message(vm::Executor &ex, const std::string &text) {
	Brain *b = brain_of(ex);
	log.push_back((b ? b->owner->name : std::string("?")) + ": " + text);
	if (getenv("OPENBG_SCRIPT_TRACE")) {
		fprintf(stderr, "frame %4u  %-8s %s\n", frame_, b ? b->owner->name.c_str() : "?", text.c_str());
	}
}

uint32_t World::final_instance(vm::Executor &ex, uint32_t index) {
	// DE_CClass::GetFinalClassInstanceByIndex: a class's "final" value instances (CNamedHandle
	// event names, CText, CColorRGBA, CVector, ...), built once from the values LoadVMCode read.
	Brain *b = brain_of(ex);
	if (!b) {
		return 0;
	}
	auto it = b->final_instances.find(index);
	if (it != b->final_instances.end()) {
		return it->second;
	}
	const vm::VMClass &code = ex.current_class()->code;
	if (index >= code.finals.size()) {
		return b->final_instances[index] = make_instance("", 1);
	}
	const auto &f = code.finals[index];
	const std::string name = f.class_index < code.classes.size() ? code.classes[f.class_index].name : "";
	uint32_t a = make_instance(name, 1);
	de::Contents c(f.data);
	if (name == "CGenericEvent::CNamedHandle" || name == "CObject::CText") {
		uint32_t n = name == "CObject::CText" ? c.squeezed() : c.u32();
		auto bytes = c.bytes(n);
		std::string text(bytes.begin(), bytes.end());
		while (!text.empty() && text.back() == 0) text.pop_back();
		mem_.set<uint32_t>(a, string(text));
	} else if (!f.data.empty()) {
		memcpy(mem_.ptr(a, uint32_t(f.data.size())), f.data.data(), f.data.size());
	}
	return b->final_instances[index] = a;
}

uint32_t World::new_instance(vm::Executor &ex, uint32_t class_index, uint32_t count) {
	const vm::ScriptClass *cls = ex.current_class();
	std::string name = cls && class_index < cls->code.classes.size() ? cls->code.classes[class_index].name : "";
	return make_instance(name, count);
}

uint32_t World::make_instance(const std::string &name, uint32_t count) {
	uint32_t n = count ? count : 1;
	uint32_t a = mem_.alloc(vm::kNativeInstanceSize * n);
	for (uint32_t i = 0; i < n; ++i) {
		instance_classes_[a + i * vm::kNativeInstanceSize] = name;
	}
	if (name == "CObject::CAttackInfo") {
		for (uint32_t i = 0; i < n; ++i) {
			reset_attack_info(a + i * vm::kNativeInstanceSize);
		}
	}
	if (name == "CObject::CColorRGBA") {  // ColorRGBANewInstanceCaller: opaque white
		for (uint32_t i = 0; i < n; ++i) {
			for (uint32_t k = 0; k < 4; ++k) mem_.set<float>(a + i * vm::kNativeInstanceSize + k * 4, 1.0f);
		}
	}
	return a;
}

uint32_t World::behavior_global_id(vm::Executor &ex, uint32_t index) {
	Brain *b = brain_of(ex);
	return b && index < b->behaviors.size() ? b->behaviors[index] : 0;
}

uint32_t World::interface_for(GameObject *o, uint32_t gid) {
	if (!o || !gid) {
		return 0;
	}
	for (Brain *b : brain_list(o)) {
		for (uint32_t i = 0; i < b->attached && i < b->behaviors.size(); ++i) {
			if (b->behaviors[i] != gid) {
				continue;
			}
			auto key = std::make_pair(static_cast<const Brain *>(b), gid);
			auto it = interface_handles_.find(key);
			if (it != interface_handles_.end()) {
				return it->second;
			}
			uint32_t h = mem_.alloc(16);
			interfaces_[h] = {b, i, gid};
			interface_handles_[key] = h;
			return h;
		}
	}
	static const bool debug = getenv("OPENBG_DEBUG_INTERFACES") != nullptr;
	if (debug) fprintf(stderr, "[interface %u] %s has no behavior %u\n", frame_, o->name.c_str(), gid);
	return 0;
}

bool World::has_interface_named(GameObject *o, const std::string &behavior) {
	if (!o) return false;
	for (Brain *bp : brain_list(o)) {
		for (uint32_t i = 0; i < bp->attached && i < bp->behaviors.size(); ++i) {
			for (auto &[key, id] : behavior_ids_) {
				if (id == bp->behaviors[i] && key.first->objects()[key.second].name == behavior) return true;
			}
		}
	}
	return false;
}

bool World::interface_valid(vm::Executor &, uint32_t iface) { return interfaces_.count(iface) != 0; }

uint32_t World::interface_owner(vm::Executor &, uint32_t iface) {
	auto it = interfaces_.find(iface);
	return it == interfaces_.end() || !it->second.brain->owner ? 0 : it->second.brain->owner->handle;
}

uint32_t World::interface_global_id(vm::Executor &, uint32_t iface) {
	auto it = interfaces_.find(iface);
	return it == interfaces_.end() ? 0 : it->second.gid;
}

// DE_CBehaviorInterface::Execute (Core 0x7013e390): the behavior's method `method`, resolved
// virtually through the target's class; a state method switches the target brain's state, an
// instant one runs at once and its return value is copied to `result`.
void World::interface_execute(vm::Executor &ex, uint32_t iface, uint32_t method, uint32_t args, uint32_t result) {
	auto it = interfaces_.find(iface);
	if (it == interfaces_.end()) {
		mem_.set<uint32_t>(result, 0);
		return;
	}
	Brain *t = it->second.brain;
	auto &table = t->cls.behavior_methods;
	if (it->second.behavior >= table.size() || method >= table[it->second.behavior].size()) {
		return;
	}
	vm::ScriptMethod *m = table[it->second.behavior][method];
	if (!m) {
		return;
	}
	if (m->vslot >= 0) {
		if (auto *v = t->cls.resolve_virtual(uint32_t(m->vslot), t->exec->current_state())) {
			m = v;
		}
	}
	uint32_t n = m->return_size() + m->param_size();
	std::vector<uint8_t> frame(std::max<uint32_t>(n, 4));
	if (n) {
		memcpy(frame.data(), mem_.ptr(args, n), n);
	}
	uint64_t gen = t->exec->generation();
	if (!m->is_instant()) {
		t->exec->set_state(*m, frame);
		if (t != brain_of(ex)) settle(*this, *t, gen);
		return;
	}
	try {
		t->exec->call(*m, frame);
	} catch (const std::exception &e) {
		log.push_back(t->owner->name + ": " + m->cm->signature + ": " + e.what());
		return;
	}
	if (t != brain_of(ex)) settle(*this, *t, gen);
	if (m->return_size()) {
		memcpy(mem_.ptr(result, m->return_size()), frame.data(), m->return_size());
	}
	(void)ex;
}

uint32_t World::global_reference(vm::Executor &ex, uint32_t index) {
	Brain *b = brain_of(ex);
	auto &refs = ex.current_class()->code.global_refs;
	if (!b || index >= refs.size()) {
		return 0;
	}
	auto &g = refs[index];
	// A ref's kind indexes the class's own list of resolvable containers, so the numbers differ
	// per class (Jaguar: 24 its move list, 25 animations, 26 standard moves; Katana: 23, 24).
	// Each kind is classified once by which bank holds most of its names.
	GameObject *o = b->owner;
	const auto &fighter_bank = o->inputs ? *o->inputs : fighter_inputs;
	const de::AnimBank *anim = script_bank(ex);
	auto &kinds = ref_kinds_[&b->cls];
	auto kt = kinds.find(g.kind);
	if (kt == kinds.end()) {
		int votes[3] = {0, 0, 0};  // animation, standard input, fighter input
		for (auto &r : refs) {
			if (r.kind != g.kind || r.name.rfind("seq", 0) != 0) {
				continue;
			}
			std::string n = r.name.substr(3);
			if (anim) {
				for (auto &sq : anim->sequences) {
					if (sq.name == n) {
						++votes[0];
						break;
					}
				}
			}
			votes[1] += standard_inputs.count(n) ? 1 : 0;
			votes[2] += fighter_bank.count(n) ? 1 : 0;
		}
		int best = votes[0] >= votes[1] && votes[0] >= votes[2] ? 0 : (votes[1] >= votes[2] ? 1 : 2);
		kt = kinds.emplace(g.kind, votes[best] ? best : -1).first;
	}
	std::string key = std::to_string(g.kind) + ":" + g.name;
	auto it = b->sequence_handles.find(key);
	if (it != b->sequence_handles.end()) {
		return it->second;
	}
	uint32_t h = 0;
	if ((kt->second == 1 || kt->second == 2) && g.name.rfind("seq", 0) == 0) {
		auto &bank = kt->second == 1 ? standard_inputs : fighter_bank;
		auto s = bank.find(g.name.substr(3));
		if (s != bank.end()) {
			h = mem_.alloc(16);
			input_handles[h] = &s->second;
		}
	} else if (kt->second == 0 && g.name.rfind("seq", 0) == 0 && anim) {
		for (size_t i = 0; i < anim->sequences.size(); ++i) {
			if (anim->sequences[i].name == g.name.substr(3)) {
				// A sequence handle is a small block holding the sequence index.
				h = mem_.alloc(16);
				mem_.set<int32_t>(h, int32_t(i));
				break;
			}
		}
	}
	if (!h && getenv("OPENBG_DEBUG_ANIM")) {
		fprintf(stderr, "global ref %s kind %u name %s unresolved (bank %p, %zu seqs)\n", o->name.c_str(), g.kind,
		        g.name.c_str(), (const void *)anim, anim ? anim->sequences.size() : 0);
	}
	b->sequence_handles[key] = h;
	return h;
}

}  // namespace game
