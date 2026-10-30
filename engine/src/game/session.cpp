// Session, slots, modifiers and clients (see session.h), and descriptions (DE_CDescription:
// what the creature and pilot pickers build fighters and pilots from).
#include "game/observer.h"
#include "game/session.h"
#include "game/replay.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/descriptions.h"

namespace game {

// An object's reference block as consecutive chunks, each a run of BOOL-terminated reference
// lists (DE_CSessionModifier/DE_CStandardSessionModifier/DE_CStandardSessionSlotModifier/
// DE_CActor::ObjProcessReferences all store their lists this way).
std::vector<std::vector<std::vector<de::ObjectRef>>> reference_lists(const de::World &data, const de::ObjectRef &r) {
	std::vector<std::vector<std::vector<de::ObjectRef>>> out;
	if (!r) return out;
	try {
		auto d = r.file->object_data(r.info());
		de::Contents c(d.references);
		while (c.remaining() > 1) {
			de::Chunk ch = de::begin_chunk(c);
			if (ch.end > c.size()) break;
			auto &lists = out.emplace_back();
			while (c.pos() < ch.end) {
				auto &list = lists.emplace_back();
				while (c.pos() < ch.end && c.boolean()) {
					list.push_back(data.resolve(*r.file, de::Reference::read(c)));
				}
			}
			c.seek(ch.end);
		}
	} catch (const std::exception &) {
	}
	return out;
}

namespace {

const std::vector<de::ObjectRef> &list_at(const std::vector<std::vector<std::vector<de::ObjectRef>>> &l, size_t chunk, size_t list) {
	static const std::vector<de::ObjectRef> none;
	return chunk < l.size() && list < l[chunk].size() ? l[chunk][list] : none;
}

de::ObjectRef source_of(const GameObject *o) { return o && o->file ? de::ObjectRef{o->file, o->index} : de::ObjectRef{}; }

}  // namespace

// DE_CTransferAgent::CPassiveReference: BOOL set, a 0x20-byte block of {file GUID, object GUID}.
// Returns every passive reference's object in a contents block, in order.
std::vector<de::ObjectRef> passive_references(const de::World &data, const de::ObjectRef &r) {
	std::vector<de::ObjectRef> out;
	auto d = r.file->object_data(r.info());
	const auto &b = d.contents;
	for (size_t i = 0; i + 34 <= b.size(); ++i) {
		if (b[i] == 1 && b[i + 1] == 0x20) {
			de::Guid g;
			memcpy(g.bytes, &b[i + 18], 16);
			if (de::ObjectRef t = data.by_guid(g)) {
				out.push_back(t);
				i += 33;
			}
		}
	}
	return out;
}

namespace {

// ClassExecutor (Core 0x701434b0): contents [name][entry state name]; references: its class.
struct ExecutorClass {
	de::ObjectRef cls;
	std::string entry;
	uint32_t interval = 0;  // +0x44: v2 16, v3+ stored
	bool initial_tick = true;  // +0x48 (v4+): ticked when the scene is done adding it
};
ExecutorClass executor_class(const de::World &data, const de::ObjectRef &exec) {
	de::ObjectRef cls;
	for (auto &lists : reference_lists(data, exec)) {
		for (auto &l : lists) {
			for (auto &t : l) {
				if (t && t.info().class_path == "Class") cls = t;
			}
		}
	}
	std::string entry = "Entry";
	uint32_t interval = 0;
	bool initial_tick = true;
	try {
		de::Contents c(exec.file->object_data(exec.info()).contents);
		de::Chunk ch = de::begin_chunk(c);
		uint32_t n = c.u32();
		c.seek(c.pos() + n);
		uint32_t m = c.u32();
		if (m > 1 && m < 64) {
			auto bytes = c.bytes(m);
			entry.assign(bytes.begin(), bytes.end());
			while (!entry.empty() && entry.back() == 0) entry.pop_back();
		} else {
			c.seek(c.pos() + m);
		}
		if (ch.version == 2) interval = 16;
		else if (ch.version >= 3) interval = uint32_t(c.i32());
		if (ch.version >= 4) initial_tick = c.boolean();
	} catch (const std::exception &) {
	}
	return {cls, entry, interval, initial_tick};
}

}  // namespace

std::vector<Brain *> World::activate_executors(GameObject *o, const std::vector<de::ObjectRef> &properties, bool run) {
	std::vector<Brain *> out;
	if (!o || !activated_.insert(o).second) return out;
	for (auto &p : properties) {
		if (!p || p.info().class_path != "ClassExecutor") continue;
		auto [cls, entry, interval, initial_tick] = executor_class(data_, p);
		if (!cls) continue;
		try {
			Brain *b = attach_brain(o, cls, entry);
			b->interval = interval;
			b->initial_tick = initial_tick;
			b->started = true;
			if (run) start_brain(b);
			out.push_back(b);
		} catch (const std::exception &e) {
			log.push_back(o->name + ": " + e.what());
		}
	}
	return out;
}

void World::add_executor_property(GameObject *o, const de::ObjectRef &executor) {
	if (!o || !executor) return;
	for (auto &p : o->extra_properties) {
		if (p.file == executor.file && p.index == executor.index) return;
	}
	o->extra_properties.push_back(executor);
	static const bool debug = getenv("OPENBG_DEBUG_REFS") != nullptr;
	if (debug) fprintf(stderr, "[executor %u] %s gets %s%s\n", frame_, o->name.c_str(), executor.info().name.c_str(), activated_.count(o) ? " (running)" : "");
	if (!activated_.count(o)) return;  // enter_scene attaches it
	auto [cls, entry, interval, initial_tick] = executor_class(data_, executor);
	if (!cls) return;
	try {
		Brain *b = attach_brain(o, cls, entry);
		b->interval = interval;
		b->initial_tick = initial_tick;
		b->started = true;
		start_brain(b);
	} catch (const std::exception &e) {
		log.push_back(o->name + ": " + e.what());
	}
}

std::vector<Brain *> World::brain_list(GameObject *o) {
	std::vector<Brain *> out;
	if (!o) return out;
	auto own = [&](GameObject *x) {
		for (auto &bp : brains_) {
			if (bp->owner == x) out.push_back(bp.get());
		}
	};
	SessionState *ss = session_of(*this, o);
	if (ss && o == ss->obj) {
		own(o);
		for (GameObject *m : ss->modifiers) {
			auto v = brain_list(m);
			out.insert(out.end(), v.begin(), v.end());
		}
	} else if (SessionSlot *sl = ss ? ss->slot_of(o) : nullptr; sl && sl->obj == o) {
		for (GameObject *m : sl->modifiers) {
			auto v = brain_list(m);
			out.insert(out.end(), v.begin(), v.end());
		}
	} else if (o->is("Actor\\Creature")) {
		own(o);
		for (auto &c : objects_) {
			if (c.component_owner == o) {
				auto v = brain_list(&c);
				out.insert(out.end(), v.begin(), v.end());
			}
		}
	} else {
		own(o);
	}
	return out;
}

void World::start_brain(Brain *b) {
	try {
		b->exec->pulse();  // the entry state's prologue suspends once (op 1)...
		b->exec->pulse();  // ...then AssureActivated has run it up to its first wait
	} catch (const std::exception &e) {
		log.push_back(b->owner->name + ": " + e.what());
		b->inst.shut_down = true;
	}
	if (b->exec->running() && !b->inst.shut_down) register_brain(b);
}

std::vector<de::ObjectRef> World::actor_properties(const de::ObjectRef &r) const {
	return list_at(reference_lists(data_, r), 1, 0);
}

std::vector<de::ObjectRef> World::properties_of(const GameObject &o) const {
	std::vector<de::ObjectRef> out;
	if (o.file) out = actor_properties({o.file, o.index});
	out.insert(out.end(), o.extra_properties.begin(), o.extra_properties.end());
	return out;
}

void World::start_pending_brains() {
	auto pending = std::move(pending_starts_);
	pending_starts_.clear();
	for (Brain *b : pending) start_brain(b);
}

void World::enter_scene(GameObject *root) {
	if (!root) return;
	if (std::find(scene_roots.begin(), scene_roots.end(), root) == scene_roots.end()) scene_roots.push_back(root);
	root_session.try_emplace(root, session ? session->obj : nullptr);
	std::vector<GameObject *> branch = {root};
	for (size_t i = 0; i < branch.size(); ++i) {
		GameObject *o = branch[i];
		// DE_CPassiveActorReference::ObjPrepare: a reference in mode 2/3 creates its target and
		// gives it its place (name, transform, children), then goes away.
		if (o->is("Actor\\PassiveReference") && o->file && o != root) {
			if (GameObject *n = resolve_passive_reference(o)) {
				o = branch[i] = n;
			} else if (!object(o->handle)) {
				branch[i] = nullptr;  // created outside the branch
				continue;
			}
		}
		// DE_CPassiveObjectReference::ObjPrepare (Core 0x7004db70): with +0x2c, a copy of its target
		// is made and prepared, under the reference's parent with +0x30 (a fighter's
		// OMFFighterControllerReference: its JoystickInputManager with the hookup executor).
		if (o->class_path == "PassiveObjectReference" && o->file && !o->reference_created) {
			o->reference_created = true;
			if (GameObject *n = create_object_reference(o)) branch.push_back(n);
		}
		// DE_CScene::Add adds only the children that are not parked (inactive or not at this
		// detail level); they join when SetActive brings them back.
		for (GameObject *ch : o->children) {
			if (!parked(*ch)) branch.push_back(ch);
		}
	}
	// Every actor of the branch is prepared (its executors attached) before any entry runs, so
	// that a script's Interface() calls find the behaviors of the actors around it.
	register_branch(root);
	number_affectors(root);
	std::vector<Brain *> started;
	for (GameObject *o : branch) {
		if (o && o->is_actor && o->file && !o->brain) {
			auto b = activate_executors(o, properties_of(*o), false);
			started.insert(started.end(), b.begin(), b.end());
		}
	}
	// DE_CScene::Add ends with DoneAddingExecutors (Core 0x70146450): the executors whose +0x48 is set
	// tick there, running their entries before Add returns; the others wait for their first tick.
	for (Brain *b : started) {
		if (b->initial_tick) start_brain(b);
		else pending_starts_.push_back(b);
	}
}

GameObject *World::scene_session_of(const GameObject *o) const {
	while (o && o->parent) o = o->parent;
	auto it = o ? root_session.find(o) : root_session.end();
	return it == root_session.end() ? nullptr : it->second;
}

void World::prepare_branch(GameObject *o) {
	if (!o) return;
	parented(o, false);
	bool root = std::find(scene_roots.begin(), scene_roots.end(), o) != scene_roots.end();
	enter_scene(o);
	if (!root && o->parent) scene_roots.erase(std::remove(scene_roots.begin(), scene_roots.end(), o), scene_roots.end());
}

// DE_CPassiveActorReference contents (Core 0x70092540), v7: [passive reference: BOOL, 0x20,
// file GUID, object GUID][u32 +0x11c placement][u32 +0x120 add][u32 +0x124 deactivate][BOOL +0x128
// rename][squeezed mode].
// Create (0x700929a0): a copy of the target with the reference's name only with +0x128 set (else
// the target keeps its own); +0x124: the created actor is deactivated (vtable +0x128), a template
// kept for copying into the scene (the fighters' destruction explosion). Placement 0 puts it where
// the reference is (its transform over the target's) under the reference's parent
// (ObjSetParent(ObjParent())); otherwise it is placed by the reference's world transform with no
// parent. +0x120: a created actor that should not be inactive is added to the reference's scene.
// Modes 2 and 3 create it when the scene prepares the reference, which then goes away: its
// children move to the created actor and references to it now name the created actor
// (RefTargetReplace).
struct ReferenceFlags {
	uint32_t place = 0, mode = 0;
	bool add = false, deactivate = false;
};

static bool read_reference(const GameObject *o, ReferenceFlags &flags, de::Guid &target, bool &rename) {
	try {
		de::Contents c(o->file->object_data(o->file->objects()[o->index]).contents);
		size_t body = 0;
		int version = 0;
		while (c.remaining() > 1) {
			de::Chunk ch = de::begin_chunk(c);
			if (ch.end > c.size()) return false;
			body = c.pos(), version = ch.version;
			c.seek(ch.end);
		}
		if (version != 7) return false;
		c.seek(body);
		if (!c.boolean() || c.u8() != 0x20) return false;
		c.guid();
		target = c.guid();
		flags.place = c.u32();
		flags.add = c.u32() != 0;
		flags.deactivate = c.u32() != 0;
		rename = c.boolean();
		flags.mode = c.squeezed();
		return true;
	} catch (const std::exception &) {
		return false;
	}
}

// DE_CPassiveObjectReference contents (Core 0x7004d9e0), v1: [name][passive reference: BOOL, 0x20,
// file GUID, object GUID][BOOL +0x2c create when prepared][BOOL +0x30 under the reference's parent].
GameObject *World::create_object_reference(GameObject *o) {
	try {
		de::Contents c(o->file->object_data(o->file->objects()[o->index]).contents);
		size_t body = 0;
		int version = 0;
		while (c.remaining() > 1) {  // the class's own chunk is the last
			de::Chunk ch = de::begin_chunk(c);
			if (ch.end > c.size()) return nullptr;
			body = c.pos(), version = ch.version;
			c.seek(ch.end);
		}
		if (version != 1) return nullptr;
		c.seek(body);
		uint32_t name_len = c.u32();
		c.seek(c.pos() + name_len);
		if (!c.boolean() || c.u8() != 0x20) return nullptr;
		c.guid();
		de::Guid target = c.guid();
		bool create = c.boolean(), under_parent = c.boolean();
		static const bool debug = getenv("OPENBG_DEBUG_REFS") != nullptr;
		if (debug) fprintf(stderr, "[objref %u] %s: create %d under parent %d -> %s\n", frame_, o->name.c_str(), create, under_parent,
		                   data_.by_guid(target) ? data_.by_guid(target).info().name.c_str() : "?");
		if (!create) return nullptr;
		GameObject *tpl = global_object(data_.by_guid(target));
		if (!tpl) return nullptr;
		GameObject *n = copy_tree(*tpl);
		if (under_parent && o->parent) {
			n->parent = o->parent;
			o->parent->children.push_back(n);
		}
		return n;
	} catch (const std::exception &e) {
		log.push_back(o->name + ": " + e.what());
		return nullptr;
	}
}

static GameObject *reference_target(World &w, GameObject *o, const ReferenceFlags &flags, const de::Guid &target, bool rename) {
	GameObject *tpl = w.global_object(w.data().by_guid(target));
	if (!tpl) return nullptr;
	GameObject *n = w.copy_tree(*tpl);
	if (rename) n->name = o->name;
	n->local = (flags.place == 0 ? o->local : o->world()).then(tpl->local);
	for (auto &p : w.actor_properties({o->file, o->index})) {
		if (p) n->extra_properties.push_back(p);
	}
	if (flags.deactivate) n->active = false;  // vtable +0x128, SetInnactive: the created actor parked
	static const bool debug = getenv("OPENBG_DEBUG_REFS") != nullptr;
	if (debug) {
		fprintf(stderr, "[ref %u] %s -> %s (%s), %zu reference propert%s%s%s, placement %u\n", w.frame(), o->name.c_str(), n->name.c_str(), n->class_path.c_str(),
		        n->extra_properties.size(), n->extra_properties.size() == 1 ? "y" : "ies", flags.deactivate ? ", deactivated" : "", flags.add ? ", added" : "", flags.place);
	}
	return n;
}

GameObject *World::resolve_passive_reference(GameObject *o) {
	ReferenceFlags flags;
	de::Guid target;
	bool rename = false;
	bool ok = read_reference(o, flags, target, rename);
	static const bool debug = getenv("OPENBG_DEBUG_REFS") != nullptr;
	if (debug) fprintf(stderr, "[passive %u] %s: mode %d place %d add %d\n", frame_, o->name.c_str(), ok ? flags.mode : -1, flags.place, flags.add);
	if (!ok || (flags.mode != 2 && flags.mode != 3)) return nullptr;
	GameObject *n = reference_target(*this, o, flags, target, rename);
	if (!n) return nullptr;
	if (flags.place == 0) {
		n->parent = o->parent;
		if (o->parent) {
			auto &v = o->parent->children;
			std::replace(v.begin(), v.end(), o, n);
		}
	} else if (o->parent) {
		auto &v = o->parent->children;
		v.erase(std::remove(v.begin(), v.end(), o), v.end());
	}
	for (GameObject *ch : o->children) {
		ch->parent = n;
		n->children.push_back(ch);
	}
	o->children.clear();
	o->parent = nullptr;
	// RefTargetReplace: what named the reference names the created actor.
	if (object_for(o->file, o->index) == o) by_source_[{o->file, o->index}] = n;
	destroy(o);
	// Not under the reference's parent: in the scene only when the reference adds it.
	if (flags.place != 0 && flags.add && !flags.deactivate) {
		root_session.try_emplace(n, scene_session_of(o));
		enter_scene(n);
	}
	return flags.place == 0 ? n : nullptr;
}

GameObject *World::create_from_reference(GameObject *o) {
	if (!o || !o->file) return nullptr;
	ReferenceFlags flags;
	de::Guid target;
	bool rename = false;
	if (!read_reference(o, flags, target, rename)) return nullptr;
	GameObject *n = reference_target(*this, o, flags, target, rename);
	if (!n) return nullptr;
	if (flags.place == 0 && o->parent) {
		n->parent = o->parent;
		o->parent->children.push_back(n);
	}
	if (flags.add && !flags.deactivate) {
		bool in_scene = false;
		for (const GameObject *p = o; p && !in_scene; p = p->parent) {
			in_scene = std::find(scene_roots.begin(), scene_roots.end(), p) != scene_roots.end();
		}
		if (in_scene) {
			if (!n->parent) root_session.try_emplace(n, scene_session_of(o));
			enter_scene(n);
		}
	}
	return n;
}

void World::send_event(GameObject *target, uint32_t event) {
	if (!target || !event) return;
	auto it = relays.find(target);
	if (it == relays.end()) return;
	auto hooks = it->second;  // a relay may add or remove relays
	for (auto &h : hooks) {
		std::vector<uint8_t> args(4);
		memcpy(args.data(), &event, 4);
		h.first->call_handle(*this, h.second, args);
	}
}

namespace {

SessionNode &make_node(World &w, SessionState &s, GameObject *obj) {
	auto &n = s.nodes[obj];
	if (!n) {
		n = std::make_unique<SessionNode>();
		n->obj = obj;
		auto lists = reference_lists(w.data(), source_of(obj));
		if (obj->is("SessionModifier")) {
			for (auto &t : list_at(lists, 2, 0)) n->properties.push_back(w.global_object(t));
			for (size_t k = 1; k < 3; ++k) {
				for (auto &t : list_at(lists, 2, k)) n->attached.push_back(t);
			}
		} else {  // StandardSessionSlotModifier
			for (auto &t : list_at(lists, 1, 0)) n->properties.push_back(w.global_object(t));
			for (auto &t : list_at(lists, 1, 2)) n->components.push_back(t);
		}
	}
	return *n;
}

std::vector<de::ObjectRef> property_refs(const SessionNode &n) {
	std::vector<de::ObjectRef> out;
	for (auto *p : n.properties) out.push_back(source_of(p));
	return out;
}

// DE_CStandardSessionModifier::Initialize: start the executor properties on the modifier, then
// copy in the modifiers it attaches (they are initialised in turn by the session's loop).
void initialize_node(World &w, SessionState &s, SessionNode &n) {
	if (n.initialized) return;
	n.initialized = true;
	w.activate_executors(n.obj, property_refs(n));
	// The attached modifiers follow the one that attaches them (so they are running before any
	// later modifier -- e.g. the score limit -- looks up their interfaces).
	auto at = std::find(s.modifiers.begin(), s.modifiers.end(), n.obj);
	size_t pos = at == s.modifiers.end() ? s.modifiers.size() : size_t(at - s.modifiers.begin()) + 1;
	for (auto &t : n.attached) {
		if (GameObject *tpl = w.global_object(t)) {
			GameObject *m = w.copy_tree(*tpl);
			// Under the modifier that attaches it (the master); the session's brain list reaches it.
			m->parent = n.obj;
			n.obj->children.push_back(m);
			s.modifiers.insert(s.modifiers.begin() + long(std::min(pos, s.modifiers.size())), m);
			++pos;
			make_node(w, s, m);
		}
	}
}

void call_hook(World &w, const ScriptHook &h, uint32_t arg = 0, bool with_arg = false) {
	if (!h) return;
	std::vector<uint8_t> args(with_arg ? 4 : 0);
	if (with_arg) memcpy(args.data(), &arg, 4);
	h.brain->call_handle(w, h.method, args);
}

}  // namespace

// DE_CDescription::ObjProcessReferences (Core 0x701f17a0), v6: [member casts][BOOL icon]
// [BOOL large representation][resource components][replacement objects][BOOL medium].
static std::pair<std::vector<de::ObjectRef>, std::vector<de::ObjectRef>> description_lists(World &w, const de::ObjectRef &d) {
	std::vector<de::ObjectRef> components, replacements;
	try {
		de::Contents c(d.file->object_data(d.info()).references);
		c.seek(de::begin_chunk(c).end);  // DE_CObject's
		if (c.remaining() < 2) return {};
		de::Chunk ch = de::begin_chunk(c);
		if (ch.version != 6) return {};
		auto list = [&](std::vector<de::ObjectRef> *out) {
			while (c.pos() < ch.end && c.boolean()) {
				de::ObjectRef r = w.data().resolve(*d.file, de::Reference::read(c));
				if (out && r) out->push_back(r);
			}
		};
		auto single = [&] {
			if (c.pos() < ch.end && c.boolean()) de::Reference::read(c);
		};
		list(nullptr);
		single();
		single();
		list(&components);
		list(&replacements);
	} catch (const std::exception &) {
	}
	return {components, replacements};
}

GameObject *new_resource(World &w, GameObject *desc, const de::Guid *model) {
	de::ObjectRef src = source_of(desc);
	if (!src) return nullptr;
	auto refs = passive_references(w.data(), src);
	if (refs.empty()) return nullptr;
	GameObject *res = nullptr;
	if (refs[0].info().class_path == "Actor\\Creature\\OMF2Fighter" && w.make_fighter) {
		res = w.make_fighter(refs[0]);
	} else if (GameObject *tpl = w.global_object(refs[0])) {
		res = w.copy_tree(*tpl);
		if (res->is("Actor\\Creature\\Pilot")) {
			if (auto p = read_pilot(*refs[0].file, refs[0].info(), w.ai_difficulty)) {
				static std::deque<Pilot> pilots;
				res->pilot = &pilots.emplace_back(*p);
			}
		}
	}
	if (res && src.info().class_path == "Description\\Creature\\Fighter") {
		// [resource][default model][extra actor (the HUD)]: the extra and the model (the
		// chosen one, else the default) become children of the fighter.
		auto add = [&](const de::ObjectRef &r) {
			if (GameObject *tpl = w.global_object(r)) {
				GameObject *ch = w.copy_tree(*tpl);
				ch->parent = res;
				res->children.push_back(ch);
			}
		};
		if (refs.size() > 2) add(refs[2]);
		de::ObjectRef m = model ? w.data().by_guid(*model) : de::ObjectRef{};
		if (!m && refs.size() > 1) m = refs[1];
		if (m) add(m);
	}
	// DE_CDescription::NewResource: each of the description's replacement objects (+0x80) takes
	// the place of the resource's descendant of its name, and a copy of each resource component
	// (+0x6c: a score limit's ScoreLimit value, an arena's extra actors) joins the resource.
	if (res) {
		auto [components, replacements] = description_lists(w, src);
		for (auto &r : replacements) {
			GameObject *tpl = w.global_object(r);
			if (!tpl) continue;
			std::function<GameObject *(GameObject *)> find = [&](GameObject *o) -> GameObject * {
				for (GameObject *ch : o->children) {
					if (ch->name == tpl->name) return ch;
					if (GameObject *x = find(ch)) return x;
				}
				return nullptr;
			};
			if (GameObject *old = find(res)) {
				GameObject *n = w.copy_tree(*tpl);
				n->parent = old->parent;
				auto &v = old->parent->children;
				std::replace(v.begin(), v.end(), old, n);
				old->parent = nullptr;
				w.destroy(old);
			}
		}
		for (auto &r : components) {
			if (GameObject *tpl = w.global_object(r)) {
				GameObject *v = w.copy_tree(*tpl);
				v->parent = res;
				res->children.push_back(v);
			}
		}
	}
	return res;
}

SessionState *session_of(World &w, const GameObject *o) {
	for (const GameObject *p = o; p; p = p->parent) {
		for (auto &s : w.sessions) {
			if (s->obj == p) return s.get();
		}
	}
	return nullptr;
}

// DE_CStandardSession::End deletes the session's own scene (+0x16c), whose destruction
// (DE_CScene::ObjDestroyContents, Core 0x7006ea60) destroys its actors and owned trees.
static void end_scene_of(World &w, SessionState &s) {
	size_t n = 0;
	for (GameObject *r : std::vector<GameObject *>(w.scene_roots)) {
		if (r && !r->gui && w.scene_session_of(r) == s.obj) w.clean_later(r), ++n;
	}
	if (getenv("OPENBG_DEBUG_SESSION")) fprintf(stderr, "[session %u] %s: %zu of %zu scene roots go with its scene\n", w.frame(), s.obj->name.c_str(), n, w.scene_roots.size());
}

// DE_CSession::End (0x701f9400): every client destroyed, the attached modifier descriptions,
// master modifier (+0x6c) and master description (+0x60) deleted; DE_CStandardSession::End
// (Session 0x4500ef40) also its scene and slots, whose owned creatures go with them.
void end_session(World &w, SessionState &s) {
	if (s.ended) return;
	if (getenv("OPENBG_DEBUG_SESSION")) fprintf(stderr, "[session %u] end %s (environment %s)\n", w.frame(), s.obj->name.c_str(), s.environment ? s.environment->name.c_str() : "-");
	replay_session_ended(w, s);
	s.ended = true;
	for (auto &cl : s.clients) w.clean_later(cl->obj);
	for (auto &sl : s.slots) {
		if (sl->creature) w.clean_later(sl->creature);
		w.clean_later(sl->obj);
	}
	if (s.environment) w.clean_later(s.environment);
	end_scene_of(w, s);
	for (GameObject *m : s.modifiers) w.clean_later(m);
	w.clean_later(s.obj);
}

// DE_CSession::ObjDestroyContents (0x701f7f20) calls End (vtable +0x174): a deleted session
// ends when housekeeping destroys it.
void end_session_object(World &w, GameObject *o) {
	for (auto &s : w.sessions) {
		if (s->obj == o) end_session(w, *s);
	}
}

void destroy_all_sessions(World &w) {
	for (auto &sp : std::vector<std::shared_ptr<SessionState>>(w.sessions)) end_session(w, *sp);
}

void add_session_modifier(World &w, GameObject *m) {
	auto &s = *w.session;
	m->parent = s.obj;
	s.obj->children.push_back(m);
	s.modifiers.push_back(m);
	make_node(w, s, m);
}

namespace {

std::shared_ptr<SessionState> new_session(World &w, const char *name) {
	auto s = std::make_shared<SessionState>();
	s->obj = w.create_child(nullptr, name, "Session\\Standard");
	w.sessions.push_back(s);
	return s;
}

void add_modifier_copy(World &w, SessionState &s, const de::ObjectRef &r) {
	GameObject *tpl = w.global_object(r);
	if (!tpl) return;
	GameObject *m = w.copy_tree(*tpl);
	m->parent = s.obj;
	s.obj->children.push_back(m);
	s.modifiers.push_back(m);
	make_node(w, s, m);
}

// DE_CSession::Begin for `s`: its attached modifier descriptions' resources join it, then every
// modifier is initialised and begun (DE_CStandardSession::Begin).
void begin(World &w, SessionState &s) {
	for (const de::Guid &g : s.attached_guids) {
		GameObject *tpl = w.global_object(w.data().by_guid(g));
		GameObject *m = tpl ? new_resource(w, w.copy_tree(*tpl), nullptr) : nullptr;
		if (!m) continue;
		m->parent = s.obj;
		s.obj->children.push_back(m);
		s.modifiers.push_back(m);
		make_node(w, s, m);
	}
	// InitializeModifiers walks the growing list: attached modifiers are initialised too.
	for (size_t i = 0; i < s.modifiers.size(); ++i) {
		initialize_node(w, s, *s.node(s.modifiers[i]));
	}
	if (getenv("OPENBG_DEBUG_SESSION")) {
		for (auto *m : s.modifiers) {
			auto &n = *s.node(m);
			fprintf(stderr, "[session %s] modifier %s: %zu properties, brain %s, hooks%s%s%s%s\n", s.obj->name.c_str(), m->name.c_str(), n.properties.size(),
			        m->brain ? m->brain->cls.name.c_str() : "-", n.begin ? " begin" : "", n.update ? " update" : "",
			        n.set_as_master ? " master" : "", n.ui_complete ? " ui" : "");
		}
	}
	if (!s.modifiers.empty()) call_hook(w, s.node(s.modifiers[0])->set_as_master);
	for (size_t i = 0; i < s.modifiers.size(); ++i) {
		SessionNode &n = *s.node(s.modifiers[i]);
		if (!n.begun) {
			n.begun = true;
			call_hook(w, n.begin);
		}
	}
	s.begun = true;
	replay_session_begun(w, s);
}

}  // namespace

SessionState &start_session(World &w, const std::vector<de::ObjectRef> &modifiers) {
	auto s = new_session(w, "Session");
	w.session = s;
	for (auto &r : modifiers) add_modifier_copy(w, *s, r);
	return *s;
}

void begin_session(World &w) { begin(w, *w.session); }

static SessionClient &add_client_to(World &w, SessionState &s, bool user, const std::string &name) {
	auto &c = *s.clients.emplace_back(std::make_unique<SessionClient>());
	c.obj = w.create_child(s.obj, "Client" + std::to_string(s.clients.size()), "Client\\Standard");
	c.user = user;
	c.local = true;
	c.active = true;
	c.player_name = name;
	return c;
}

SessionClient &add_client(World &w, bool user, const std::string &name) { return add_client_to(w, *w.session, user, name); }

void complete_ui(World &w, GameObject *ui, const de::Guid &chosen, const de::Guid *model) {
	if (!ui) return;
	SessionState *s = nullptr;
	for (auto &sp : w.sessions) {
		if (sp->ui.count(ui)) s = sp.get();
	}
	if (!s) return;
	auto &u = s->ui[ui];
	memcpy(u.chosen.data(), chosen.bytes, 16);
	if (model) memcpy(u.model.data(), model->bytes, 16);
	u.shown = false;
	u.pending = true;
}

uint32_t new_event(World &w, const std::string &handle) {
	uint32_t e = w.make_instance("CGenericEvent", 1);
	w.memory().set<uint32_t>(e, w.string(handle));
	return e;
}

void render_tick(World &w, float ms) {
	auto hooks = w.render_tick_hooks;
	for (auto &[b, m] : hooks) {
		std::vector<uint8_t> args(4);
		memcpy(args.data(), &ms, 4);
		b->call_handle(w, m, args);
	}
}

// A session begun during the frame's updates becomes the current one after them.
static std::weak_ptr<SessionState> g_begun;

// One session's update (DE_CStandardSession::Update).
static void update_one(World &w, SessionState &s) {
	// A preset UI choice completes: every node's UIManagerComplete hook hears about it (each
	// checks whether the manager is its own).
	for (auto &[ui, u] : s.ui) {
		if (!u.pending) continue;
		u.pending = false;
		std::vector<SessionNode *> nodes;
		for (auto &[o, n] : s.nodes) nodes.push_back(n.get());
		for (auto *n : nodes) call_hook(w, n->ui_complete, ui->handle, true);
	}
	// Loading is the match sub-session's BRulesDisplay behaviour (Session.DEObjects): it asks
	// SessionQuerryIsLoadingRequired and sends one SessionLoadElements per 6 session updates,
	// showing "Loading <name>" on the rules display; the engine itself loads nothing.
	for (size_t i = 0; i < s.modifiers.size(); ++i) {
		call_hook(w, s.node(s.modifiers[i])->update);
	}
	for (size_t i = 0; i < s.slots.size(); ++i) {
		auto mods = s.slots[i]->modifiers;
		for (auto *m : mods) {
			if (auto *n = s.node(m)) call_hook(w, n->update);
		}
	}
	// DE_CStandardSession::Update (Session 0x4500e770): after DE_CSession::Update (the modifiers),
	// connect each active client without a slot to the next connectable one.
	for (auto &c : s.clients) {
		if (!c->active || c->slot) continue;
		for (auto &sl : s.slots) {
			if (sl->connectable && !sl->client && sl->begun) {
				sl->client = c.get();
				c->slot = sl.get();
				for (auto &h : s.slot_connect_hooks) call_hook(w, h, sl->obj->handle, true);
				uint32_t e = new_event(w, "SessionSlotNewClient");
				w.send_event(sl->obj, e);
				break;
			}
		}
	}
}

void update_session(World &w) {
	{
		auto hooks = w.delta_tick_hooks;
		float ms = float(w.frame_ms());
		for (auto &[b, m] : hooks) {
			if (b->inst.shut_down) continue;
			std::vector<uint8_t> args(4);
			memcpy(args.data(), &ms, 4);
			b->call_handle(w, m, args);
		}
	}
	if (w.render_ticks_per_frame) render_tick(w, float(w.frame_ms()));
	// Sessions whose object has been deleted (EndAndDie) are gone; the current one falls back to
	// the session that made it.
	for (size_t i = 0; i < w.sessions.size();) {
		auto sp = w.sessions[i];
		if (w.object(sp->obj->handle)) {
			++i;
			continue;
		}
		w.sessions.erase(w.sessions.begin() + long(i));
		if (w.session == sp) {
			w.session = nullptr;
			for (auto &o : w.sessions) {
				if (o.get() == sp->owner) w.session = o;
			}
			if (!w.session && !w.sessions.empty()) w.session = w.sessions.back();
		}
		for (auto &o : w.sessions) {
			if (o->owner == sp.get()) o->owner = nullptr;
		}
	}
	replay_update(w);
	auto current = w.session;
	auto all = w.sessions;
	for (auto &sp : all) {
		if (!sp->begun || sp->ended) continue;
		w.session = sp;
		update_one(w, *sp);
	}
	if (std::find(w.sessions.begin(), w.sessions.end(), current) != w.sessions.end() || !current) w.session = current;
	if (auto b = g_begun.lock()) {
		if (std::find(w.sessions.begin(), w.sessions.end(), b) != w.sessions.end()) w.session = b;
		g_begun.reset();
	}
	if (w.session && std::find(w.sessions.begin(), w.sessions.end(), w.session) == w.sessions.end()) w.session = w.sessions.empty() ? nullptr : w.sessions.back();
}

void register_session_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };
	auto stub = [&](const char *key) { w.register_native(key, [](const NativeContext &) {}); };
	// The session a call is about: the one its object belongs to, else the one the calling
	// script's object belongs to, else the current one.
	auto S = [](const NativeContext &c) -> SessionState * {
		if (SessionState *s = session_of(c.world, c.self())) return s;
		if (Brain *b = c.world.brain_of(c.ex)) {
			if (SessionState *s = session_of(c.world, b->owner)) return s;
		}
		return c.world.session.get();
	};
	auto shared = [](const NativeContext &c, SessionState *s) -> std::shared_ptr<SessionState> {
		for (auto &sp : c.world.sessions) {
			if (sp.get() == s) return sp;
		}
		return nullptr;
	};
	auto node = [S](const NativeContext &c) -> SessionNode * {
		auto *s = S(c);
		return s ? s->node(c.self()) : nullptr;
	};
	auto slot_at = [S](const NativeContext &c, int i) -> GameObject * {
		auto *s = S(c);
		return s && i >= 0 && size_t(i) < s->slots.size() ? s->slots[size_t(i)]->obj : nullptr;
	};
	auto user_client = [S](const NativeContext &c) -> SessionClient * {
		if (auto *s = S(c)) {
			for (auto &cl : s->clients) {
				if (cl->user) return cl.get();
			}
		}
		return nullptr;
	};

	// --- Hooks: Register*Hook(exThis(), Method) on a session or slot modifier -------------------
	const std::vector<std::pair<std::string, ScriptHook SessionNode::*>> hooks = {
		{"RegisterBeginHook(class CClassExecutorInstance,Method())", &SessionNode::begin},
		{"RegisterUpdateHook(class CClassExecutorInstance,Method())", &SessionNode::update},
		{"RegisterSetAsMasterHook(class CClassExecutorInstance,Method())", &SessionNode::set_as_master},
		{"RegisterEndHook(class CClassExecutorInstance,Method())", &SessionNode::end},
		{"RegisterStateChangedHook(class CClassExecutorInstance,Method())", &SessionNode::state_changed},
		{"RegisterUIManagerActivateHook(class CClassExecutorInstance,Method(class CUIManager))", &SessionNode::ui_activate},
		{"RegisterUIManagerCompleteHook(class CClassExecutorInstance,Method(class CUIManager))", &SessionNode::ui_complete},
	};
	for (const char *cls : {"CStandardSessionModifier::", "CStandardSessionSlotModifier::"}) {
		for (auto &[sig, field] : hooks) {
			auto f = field;
			reg(cls + sig, [node, f](const NativeContext &c) {
				if (auto *n = node(c)) n->*f = {c.world.brain_of(c.ex), c.arg<uint32_t>(1)};
			});
		}
		reg(std::string(cls) + "UnregisterUpdateHook(class CClassExecutorInstance,Method())", [node](const NativeContext &c) {
			if (auto *n = node(c); n && n->update.brain == c.world.brain_of(c.ex)) n->update = {};
		});
		reg(std::string(cls) + "RegisterState()", [](const NativeContext &c) { register_state(c.world, c.self()); });
		stub((std::string(cls) + "RegisterUrgentState()").c_str());
		reg(std::string(cls) + "class CStandardSession Session()", [S](const NativeContext &c) {
			auto *s = S(c);
			c.ret_object(s ? s->obj : nullptr);
		});
		reg(std::string(cls) + "class CScene Scene()", [](const NativeContext &c) { c.ret_object(c.world.create("Scene", "Scene")); });
		reg(std::string(cls) + "bool IsServer()", [](const NativeContext &c) { c.ret_bool(true); });
		reg(std::string(cls) + "bool IsSimulating()", [](const NativeContext &c) { c.ret_bool(true); });
		// AddPropertyCaller (Session 0x45014ba0 / 0x45019e60): appended to the property list.
		reg(std::string(cls) + "AddProperty(class CObject)", [node](const NativeContext &c) {
			if (auto *n = node(c); n && c.arg_object(0)) n->properties.push_back(c.arg_object(0));
		});
		reg(std::string(cls) + "int NumProperty()", [node](const NativeContext &c) {
			auto *n = node(c);
			c.ret_u32(n ? uint32_t(n->properties.size()) : 0);
		});
		reg(std::string(cls) + "class CObject Property(int)", [node](const NativeContext &c) {
			auto *n = node(c);
			int i = c.arg<int32_t>(0);
			c.ret_object(n && i >= 0 && size_t(i) < n->properties.size() ? n->properties[size_t(i)] : nullptr);
		});
		reg(std::string(cls) + "class CObject FindPropertyByName(string)", [node](const NativeContext &c) {
			auto *n = node(c);
			std::string name = c.arg_string(0);
			GameObject *found = nullptr;
			if (n) {
				for (auto *p : n->properties) {
					if (p && p->name == name) found = p;
				}
			}
			c.ret_object(found);
		});
	}

	// --- Session modifiers ----------------------------------------------------------------------
	reg("CStandardSessionModifier::class CEnvironment Environment()", [S](const NativeContext &c) {
		auto *s = S(c);
		c.ret_object(s ? s->environment : nullptr);
	});
	reg("CStandardSessionModifier::OwnEnvironment(class CEnvironment)", [S](const NativeContext &c) {
		if (auto *s = S(c)) {
			s->environment = c.arg_object(0);
			if (getenv("OPENBG_DEBUG_SESSION")) fprintf(stderr, "[session %u] %s owns environment %s\n", c.world.frame(), s->obj->name.c_str(), s->environment ? s->environment->name.c_str() : "-");
			if (s->environment) c.world.root_session[s->environment] = s->obj;
			c.world.enter_scene(s->environment);
			if (s->environment) s->scene_started = true;
		}
	});
	// SetEnvironmentGUIDCaller / EnvironmentGUIDCaller (Session 0x4500e680): the chosen
	// environment's GUID, kept so connecting players can learn it.
	reg("CObject::SetEnvironmentGUID(class CGUID)", [S](const NativeContext &c) {
		uint32_t g = c.arg<uint32_t>(0);
		if (auto *s = S(c); s && g) memcpy(s->environment_guid, c.mem().ptr(g, 16), 16);
	});
	reg("CObject::class CGUID EnvironmentGUID()", [S](const NativeContext &c) {
		uint32_t g = c.world.make_instance("CObject::CGUID", 1);
		if (auto *s = S(c)) memcpy(c.mem().ptr(g, 16), s->environment_guid, 16);
		c.ret_u32(g);
	});
	// DE_CStandardSessionModifier::CreateNewSlot: a new slot appended to the session.
	reg("CStandardSessionModifier::class CStandardSessionSlot CreateSlot()", [S](const NativeContext &c) {
		auto *s = S(c);
		if (!s) {
			c.ret_u32(0);
			return;
		}
		auto &sl = *s->slots.emplace_back(std::make_unique<SessionSlot>());
		sl.index = int(s->slots.size()) - 1;
		sl.obj = c.world.create_child(s->obj, "Slot" + std::to_string(sl.index), "StandardSessionSlot");
		c.ret_object(sl.obj);
	});
	reg("CStandardSessionModifier::int NumSlot()", [S](const NativeContext &c) { c.ret_u32(S(c) ? uint32_t(S(c)->slots.size()) : 0); });
	reg("CStandardSessionModifier::class CStandardSessionSlot Slot(int)", [slot_at](const NativeContext &c) { c.ret_object(slot_at(c, c.arg<int32_t>(0))); });
	reg("CStandardSessionModifier::SetCamera(class CActor)", [S](const NativeContext &c) {
		if (auto *s = S(c)) s->camera = c.arg_object(0);
		set_renderer_camera(c.world, c.arg_object(0));
	});

	// --- Slot modifiers -------------------------------------------------------------------------
	reg("CStandardSessionSlotModifier::class CStandardSessionSlot Slot()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_object(o ? o->parent : nullptr);
	});
	reg("CStandardSessionSlotModifier::class CStandardSessionModifier Master()", [S](const NativeContext &c) {
		auto *s = S(c);
		c.ret_object(s && !s->modifiers.empty() ? s->modifiers[0] : nullptr);
	});
	reg("CStandardSessionSlotModifier::class CStandardClient CurrentClient()", [S](const NativeContext &c) {
		auto *s = S(c);
		auto *o = c.self();
		auto *sl = s && o ? s->slot_of(o->parent) : nullptr;
		c.ret_object(sl && sl->client ? sl->client->obj : nullptr);
	});
	reg("CStandardSessionSlotModifier::dword LocalTime()", [](const NativeContext &c) { c.ret_u32(uint32_t(c.world.scene_ms())); });
	// AttachComponents: copies of the modifier's creature components (e.g. FighterStats) under
	// the creature, their executors started.
	reg("CStandardSessionSlotModifier::AttachComponents(class CCreature)", [node](const NativeContext &c) {
		auto *n = node(c);
		GameObject *creature = c.arg_object(0);
		if (!n || !creature) return;
		// DE_CStandardSessionSlotModifier::AttachComponents (Session): a copy of each component
		// becomes a component of the creature (CCreature::AddComponent, vtable +0x188), so the
		// creature's brain list -- Interface(BFighterStats) -- reaches it.
		for (auto &t : n->components) {
			GameObject *tpl = c.world.global_object(t);
			if (!tpl) continue;
			GameObject *comp = c.world.copy_tree(*tpl);
			comp->parent = creature;
			comp->component_owner = creature;
			creature->children.push_back(comp);
			c.world.activate_executors(comp, c.world.actor_properties(t));
		}
	});

	// --- Slots ----------------------------------------------------------------------------------
	auto slot = [S](const NativeContext &c) -> SessionSlot * {
		auto *s = S(c);
		return s ? s->slot_of(c.self()) : nullptr;
	};
	reg("CStandardSessionSlot::int SlotIndex()", [slot](const NativeContext &c) {
		auto *sl = slot(c);
		c.ret_u32(uint32_t(sl ? sl->index : -1));
	});
	reg("CStandardSessionSlot::SetAsLocallyControlled()", [slot](const NativeContext &c) { if (auto *sl = slot(c)) sl->local = true; });
	reg("CStandardSessionSlot::SetAsRemoteControllable()", [slot](const NativeContext &c) { if (auto *sl = slot(c)) sl->remote_controllable = true; });
	reg("CStandardSessionSlot::SetClientUnconnectable()", [slot](const NativeContext &c) { if (auto *sl = slot(c)) sl->connectable = false; });
	reg("CStandardSessionSlot::bool IsLocallyControlled()", [slot](const NativeContext &c) { auto *sl = slot(c); c.ret_bool(sl && sl->local); });
	reg("CStandardSessionSlot::bool IsUserControlled()", [slot](const NativeContext &c) {
		auto *sl = slot(c);
		c.ret_bool(sl && sl->client && sl->client->user);
	});
	reg("CStandardSessionSlot::AddModifier(class CStandardSessionSlotModifier)", [S, slot](const NativeContext &c) {
		auto *sl = slot(c);
		GameObject *m = c.arg_object(0);
		if (!sl || !m) return;
		sl->modifiers.push_back(m);
		make_node(c.world, *S(c), m).slot = sl;
		if (sl->begun) {  // added to a running slot: start it now
			auto &n = *S(c)->node(m);
			initialize_node(c.world, *S(c), n);
			n.begun = true;
			call_hook(c.world, n.begin);
		}
	});
	// DE_CStandardSessionSlot::Begin: start every slot modifier (executors, then Begin hooks).
	reg("CStandardSessionSlot::Begin()", [S, slot](const NativeContext &c) {
		auto *sl = slot(c);
		if (!sl || sl->begun) return;
		sl->begun = true;
		auto mods = sl->modifiers;
		for (auto *m : mods) initialize_node(c.world, *S(c), *S(c)->node(m));
		for (auto *m : mods) {
			auto &n = *S(c)->node(m);
			if (!n.begun) {
				n.begun = true;
				call_hook(c.world, n.begin);
			}
		}
	});
	reg("CStandardSessionSlot::class CCreature OwnedCreature()", [slot](const NativeContext &c) {
		auto *sl = slot(c);
		c.ret_object(sl ? sl->creature : nullptr);
	});
	// OwnCreature: the slot takes the creature; its client decides who controls it
	// (SetupCreatureControl: the user's client, or the computer).
	reg("CStandardSessionSlot::OwnCreature(class CCreature)", [slot, S](const NativeContext &c) {
		auto *sl = slot(c);
		if (!sl) return;
		sl->creature = c.arg_object(0);
		if (sl->creature) {
			if (SessionState *s = S(c); s && !sl->creature->parent) c.world.root_session[sl->creature] = s->obj;
			// The creature is in the scene now (its controller made, its scripts -- and its
			// pilot's, HUD's... -- started, the fighter's entry at once as AssureActivated does);
			// then SetupCreatureControl binds the user's client's device to it.
			c.world.enter_scene(sl->creature);
			c.world.set_user_controlled(*sl->creature, sl->client && sl->client->user ? 0 : -1);
			if (Brain *b = sl->creature->brain; b && !b->started) {
				b->started = true;
				b->exec->pulse();
				b->exec->pulse();
			}
		}
		if (c.world.on_creature_owned && sl->creature) c.world.on_creature_owned(sl->creature, sl->index);
	});
	reg("CStandardSessionSlot::class CStandardClient CurrentClient()", [slot](const NativeContext &c) {
		auto *sl = slot(c);
		c.ret_object(sl && sl->client ? sl->client->obj : nullptr);
	});
	reg("CStandardSessionSlot::bool IsInFocus()", [slot](const NativeContext &c) {
		auto *sl = slot(c);
		c.ret_bool(sl && sl->client && sl->client->user);
	});
	reg("CStandardSessionSlot::bool IsPlayerSlot()", [slot](const NativeContext &c) { c.ret_bool(slot(c) != nullptr); });
	reg("CStandardSessionSlot::bool IsCreatureActive()", [slot](const NativeContext &c) {
		auto *sl = slot(c);
		c.ret_bool(sl && sl->creature);
	});

	// --- The session (CSession / CStandardSession) ----------------------------------------------
	// DE_CSession::MaxPlayerClients (Core 0x70010f80): the top-level session's session info's.
	reg("CSession::int MaxPlayerClients()", [S](const NativeContext &c) {
		SessionState *s = S(c);
		while (s && s->owner) s = s->owner;
		c.ret_u32(s ? uint32_t(s->max_player_clients) : 2);
	});
	reg("CSession::bool IsReplaying()", [S](const NativeContext &c) {
		SessionState *s = S(c);
		c.ret_bool(s ? s->replaying : replaying(c.world));
	});
	reg("CSession::bool IsLocal()", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CSession::bool IsReadyToLoad()", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CSession::bool IsFullySequenced()", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CSession::bool AreMasterRequirementsSatisfied()", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CSession::string StatusDescription()", [](const NativeContext &c) { c.ret_u32(c.world.string("")); });
	stub("CSession::LoadingComplete()");
	// EndAndDieCaller (Core 0x701f7d90): DeleteThisLater, then DE_CSession::End (0x701f9400):
	// every client destroyed, the modifiers, scene and environment deleted; DE_CStandardSession::End
	// also its slots, whose owned creatures go with them.
	reg("CSession::EndAndDie()", [S](const NativeContext &c) {
		if (SessionState *s = S(c)) end_session(c.world, *s);
	});
	// NewSubSessionCaller: a standard session made by this one (its SubSessionOwner), with no
	// modifiers until OwnMasterDescription; the id is the caller's.
	reg("CObject::class CStandardSession NewSubSession(word)", [S](const NativeContext &c) {
		SessionState *owner = S(c);
		auto sub = new_session(c.world, "SubSession");
		sub->owner = owner;
		c.ret_object(sub->obj);
	});
	// The master description's resource is the session's master modifier.
	reg("CSession::OwnMasterDescription(class CSessionMasterDescription)", [S](const NativeContext &c) {
		SessionState *s = S(c);
		GameObject *d = c.arg_object(0);
		if (!s || !d) return;
		s->master_description = d;
		d->parent = s->obj;
		s->obj->children.push_back(d);
		auto res = passive_references(c.world.data(), source_of(d));
		if (!res.empty()) {
			add_modifier_copy(c.world, *s, res[0]);
			// The master first.
			std::rotate(s->modifiers.begin(), s->modifiers.end() - 1, s->modifiers.end());
		}
	});
	reg("CSession::class CSessionMasterDescription MasterDescription()", [S](const NativeContext &c) {
		SessionState *s = S(c);
		c.ret_object(s ? s->master_description : nullptr);
	});
	reg("CSession::SetAttachedSessionModifierDescriptionGUIDs(class CGUIDPool)", [S](const NativeContext &c) {
		SessionState *s = S(c);
		uint32_t p = c.arg<uint32_t>(0);
		if (!s || !p) return;
		s->attached_guids.clear();
		uint32_t data = c.mem().get<uint32_t>(p), n = c.mem().get<uint32_t>(p + 4);
		for (uint32_t i = 0; i < n; ++i) {
			de::Guid g;
			memcpy(g.bytes, c.mem().ptr(data + i * 16, 16), 16);
			s->attached_guids.push_back(g);
		}
	});
	reg("CSession::class CGUIDPool AttachedSessionModifierDescriptionGUIDs()", [S](const NativeContext &c) {
		SessionState *s = S(c);
		uint32_t p = c.world.make_instance("CObject::CGUIDPool", 1);
		size_t n = s ? s->attached_guids.size() : 0;
		uint32_t data = c.mem().alloc(uint32_t(std::max<size_t>(1, n)) * 16);
		for (size_t i = 0; i < n; ++i) memcpy(c.mem().ptr(data + uint32_t(i) * 16, 16), s->attached_guids[i].bytes, 16);
		c.mem().set<uint32_t>(p, data);
		c.mem().set<uint32_t>(p + 4, uint32_t(n));
		c.ret_u32(p);
	});
	reg("CSession::Begin()", [S, shared](const NativeContext &c) {
		SessionState *s = S(c);
		if (!s || s->begun) return;
		auto keep = c.world.session;
		auto sp = shared(c, s);
		c.world.session = sp;
		begin(c.world, *s);
		c.world.session = keep;
		g_begun = sp;
	});
	reg("CObject::class CSession SubSessionOwner()", [S](const NativeContext &c) {
		SessionState *s = S(c);
		c.ret_object(s && s->owner ? s->owner->obj : nullptr);
	});
	reg("CObject::class CSession MasterSession()", [S](const NativeContext &c) {
		SessionState *s = S(c);
		while (s && s->owner) s = s->owner;
		c.ret_object(s ? s->obj : nullptr);
	});
	stub("CSession::SetJoinable(bool)");
	reg("CSession::class CClient NewClient()", [S](const NativeContext &c) {
		SessionState *s = S(c);
		if (!s) {
			c.ret_u32(0);
			return;
		}
		SessionClient &cl = add_client_to(c.world, *s, false, "");
		cl.active = false;
		cl.local = false;
		c.ret_object(cl.obj);
	});
	reg("CSession::class CClient LastClient()", [S](const NativeContext &c) {
		SessionState *s = S(c);
		c.ret_object(s && !s->clients.empty() ? s->clients.back()->obj : nullptr);
	});
	reg("CSession::class CClient Client(int)", [S](const NativeContext &c) {
		SessionState *s = S(c);
		int i = c.arg<int32_t>(0);
		c.ret_object(s && i >= 0 && size_t(i) < s->clients.size() ? s->clients[size_t(i)]->obj : nullptr);
	});
	stub("CStandardSession::ForceClientsToLoad()");
	reg("CSession::class CClient ControllingClient()", [user_client](const NativeContext &c) {
		auto *cl = user_client(c);
		c.ret_object(cl ? cl->obj : nullptr);
	});
	reg("CSession::int NumClients()", [S](const NativeContext &c) { c.ret_u32(S(c) ? uint32_t(S(c)->clients.size()) : 0); });
	reg("CSession::int NumPlayersAfterPopulation()", [S](const NativeContext &c) { c.ret_u32(S(c) ? uint32_t(S(c)->clients.size()) : 0); });
	// DE_CStandardSession::SetCamera (0x4500f400): stored, and the renderer's camera.
	reg("CSession::SetCamera(class CActor)", [S](const NativeContext &c) {
		if (auto *s = S(c)) s->camera = c.arg_object(0);
		set_renderer_camera(c.world, c.arg_object(0));
	});
	reg("CSession::class CActor Camera()", [S](const NativeContext &c) { c.ret_object(S(c) ? S(c)->camera : nullptr); });
	for (const char *k : {"CStandardSession::class CStandardSessionModifier Master()", "CSession::class CSessionModifier Master()"}) {
		reg(k, [S](const NativeContext &c) {
			auto *s = S(c);
			c.ret_object(s && !s->modifiers.empty() ? s->modifiers[0] : nullptr);
		});
	}
	reg("CStandardSession::int NumSlot()", [S](const NativeContext &c) { c.ret_u32(S(c) ? uint32_t(S(c)->slots.size()) : 0); });
	reg("CStandardSession::class CStandardSessionSlot Slot(int)", [slot_at](const NativeContext &c) { c.ret_object(slot_at(c, c.arg<int32_t>(0))); });
	// SlotInFocus: the slot the local user plays in (the camera follows its creature).
	reg("CStandardSession::class CStandardSessionSlot SlotInFocus()", [S](const NativeContext &c) {
		auto *s = S(c);
		GameObject *f = nullptr;
		if (s) {
			for (auto &sl : s->slots) {
				if (sl->client && sl->client->user) f = sl->obj;
			}
		}
		c.ret_object(f);
	});
	reg("CStandardSession::int FindIndexOfSlotUsingCreature(class CCreature)", [S](const NativeContext &c) {
		auto *s = S(c);
		int idx = -1;
		if (s) {
			for (auto &sl : s->slots) {
				if (sl->creature && sl->creature == c.arg_object(0)) idx = sl->index;
			}
		}
		c.ret_u32(uint32_t(idx));
	});
	auto send_all = [S](const NativeContext &c, bool creatures) {
		auto *s = S(c);
		uint32_t e = c.arg<uint32_t>(0);
		if (!s) return;
		c.world.send_event(s->obj, e);
		auto slots = std::vector<SessionSlot *>();
		for (auto &sl : s->slots) slots.push_back(sl.get());
		for (auto *sl : slots) {
			c.world.send_event(sl->obj, e);
			if (creatures && sl->creature) c.world.send_event(sl->creature, e);
		}
	};
	reg("CStandardSession::SendToSessionAndSlots(class CEvent)", [send_all](const NativeContext &c) { send_all(c, false); });
	reg("CStandardSession::SendToSessionAndSlotsAndCreatures(class CEvent)", [send_all](const NativeContext &c) { send_all(c, true); });
	reg("CStandardSession::RegisterSlotConnectClientHook(class CClassExecutorInstance,Method(class CStandardSessionSlot))", [S](const NativeContext &c) {
		if (auto *s = S(c)) s->slot_connect_hooks.push_back({c.world.brain_of(c.ex), c.arg<uint32_t>(1)});
	});
	reg("CStandardSession::RegisterSlotDisconnectClientHook(class CClassExecutorInstance,Method(class CStandardSessionSlot))", [S](const NativeContext &c) {
		if (auto *s = S(c)) s->slot_disconnect_hooks.push_back({c.world.brain_of(c.ex), c.arg<uint32_t>(1)});
	});

	// --- Event relays: AddEventRelay(exThis(), Method(CEvent)) on any object --------------------
	for (const char *k : {"CSession::AddEventRelay(class CClassExecutorInstance,Method(class CEvent))",
	                      "CStandardSessionSlot::AddEventRelay(class CClassExecutorInstance,Method(class CEvent))",
	                      "CActor::AddEventRelay(class CClassExecutorInstance,Method(class CEvent))",
	                      "CScene::AddEventRelay(class CClassExecutorInstance,Method(class CEvent))"}) {
		reg(k, [](const NativeContext &c) {
			if (GameObject *o = c.self()) c.world.relays[o].push_back({c.world.brain_of(c.ex), c.arg<uint32_t>(1)});
		});
	}
	reg("CSession::RemoveEventRelays(class CClassExecutorInstance)", [](const NativeContext &c) {
		Brain *b = c.world.brain_of(c.ex);
		if (GameObject *o = c.self()) {
			auto &v = c.world.relays[o];
			v.erase(std::remove_if(v.begin(), v.end(), [b](auto &r) { return r.first == b; }), v.end());
		}
	});

	// --- Clients --------------------------------------------------------------------------------
	auto client = [S](const NativeContext &c) -> SessionClient * {
		auto *s = S(c);
		return s ? s->client_of(c.self()) : nullptr;
	};
	reg("CClient::bool IsUserControlled()", [client](const NativeContext &c) { auto *cl = client(c); c.ret_bool(cl && cl->user); });
	reg("CClient::bool IsLocal()", [client](const NativeContext &c) { auto *cl = client(c); c.ret_bool(cl && cl->local); });
	reg("CClient::string PlayerName()", [client](const NativeContext &c) {
		auto *cl = client(c);
		c.ret_u32(cl && !cl->player_name.empty() ? c.world.string(cl->player_name) : 0);
	});
	reg("CClient::SetPlayerName(string)", [client](const NativeContext &c) { if (auto *cl = client(c)) cl->player_name = c.arg_string(0); });
	reg("CClient::SetDescription(string)", [client](const NativeContext &c) { if (auto *cl = client(c)) cl->description = c.arg_string(0); });
	reg("CClient::SetAsLocal()", [client](const NativeContext &c) { if (auto *cl = client(c)) cl->local = true; });
	reg("CClient::SetAsUserControlled()", [client](const NativeContext &c) {
		if (auto *cl = client(c)) cl->user = cl->local = true;
	});
	reg("CClient::Activate()", [client](const NativeContext &c) { if (auto *cl = client(c)) cl->active = true; });
	// DestroyClient: the client leaves its slot and the session.
	reg("CClient::DestroyClient()", [S](const NativeContext &c) {
		SessionState *s = S(c);
		GameObject *o = c.self();
		if (!s || !o) return;
		for (size_t i = 0; i < s->clients.size(); ++i) {
			if (s->clients[i]->obj != o) continue;
			if (s->clients[i]->slot) s->clients[i]->slot->client = nullptr;
			s->clients.erase(s->clients.begin() + long(i));
			break;
		}
		c.world.clean_later(o);
	});
	// UI managers would show the creature/pilot/arena pickers; choices are made up front here, so
	// a launched UI never completes.
	reg("CClient::LaunchRemoteUIManager(class CUIManager)", [client, S](const NativeContext &c) {
		auto *cl = client(c);
		GameObject *ui = c.arg_object(0);
		if (!cl || !ui) return;
		cl->ui_manager = ui;
		auto &u = S(c)->ui[ui];
		auto it = S(c)->ui_choices.find(u.description);
		if (it != S(c)->ui_choices.end()) {
			memcpy(u.chosen.data(), it->second.bytes, 16);
			u.pending = true;
		} else if (c.world.launch_ui) {
			u.shown = true;
			c.world.launch_ui(ui);
		} else {
			c.world.log.push_back("session: no preset choice for UI \"" + u.description + "\"");
		}
	});
	auto shown = [S](const NativeContext &c, GameObject *ui) {
		if (auto it = c.world.rules_displays.find(ui); ui && it != c.world.rules_displays.end()) return it->second.shown;
		auto *s = S(c);
		auto it = s && ui ? s->ui.find(ui) : decltype(s->ui.end()){};
		return s && ui && it != s->ui.end() && it->second.shown;
	};
	reg("CClient::bool IsUIManagerActive()", [client, shown](const NativeContext &c) {
		auto *cl = client(c);
		c.ret_bool(cl && shown(c, cl->ui_manager));
	});
	stub("CUIManager::SetHaltLoading(bool)");
	reg("CUIManager::bool IsActiveLocally()", [shown](const NativeContext &c) {
		c.ret_bool(c.self() && c.self() == c.world.vcr_ui ? c.world.vcr_shown : shown(c, c.self()));
	});
	reg("CObject::class CVCRControls NewVCRControls()", [](const NativeContext &c) {
		c.world.vcr_ui = c.world.create_child(nullptr, "VCRControls", "UIManager\\VCRControls");
		c.world.vcr_handle = c.world.vcr_ui->handle;
		c.ret_object(c.world.vcr_ui);
	});
	stub("CUIManager::DisableAutodestroy()");
	// The panel finds the observer itself (StandardSessionAssureObserver), as this hands it over.
	stub("CVCRControls::SetObserverCamera(class CCreature)");
	reg("CObject::SetNextUIManager(class CUIManager)", [](const NativeContext &c) {
		GameObject *ui = c.arg_object(0);
		if (ui && ui == c.world.vcr_ui && c.world.show_vcr) {
			c.world.vcr_shown = true;
			c.world.show_vcr(ui);
		}
	});
	reg("CUIManager::bool IsDisplayed()", [shown](const NativeContext &c) { c.ret_bool(shown(c, c.self())); });
	reg("CObject::class CGenericDescriptionSelector NewGenericDescriptionSelector()", [](const NativeContext &c) {
		c.ret_object(c.world.create_child(nullptr, "GenericDescriptionSelector", "UIManager\\GenericDescriptionSelector"));
	});
	reg("CObject::class CChooseFighterUIManager NewChooseFighterUIManager()", [S](const NativeContext &c) {
		GameObject *ui = c.world.create_child(nullptr, "ChooseFighterUIManager", "UIManager\\GenericDescriptionSelector\\ChooseFighter");
		if (auto *s = S(c)) s->ui[ui].choose_fighter = true;
		c.ret_object(ui);
	});
	reg("CGenericDescriptionSelector::SetUIDescription(string)", [S](const NativeContext &c) {
		if (auto *s = S(c); s && c.self()) s->ui[c.self()].description = c.arg_string(0);
	});
	reg("CGenericDescriptionSelector::SetAutoselectDescription(string)", [S](const NativeContext &c) {
		if (auto *s = S(c); s && c.self()) s->ui[c.self()].autoselect = c.arg_string(0);
	});
	// AddAllDescriptionsDerivedFrom (0x701e4770): every master object of that class or a derived one.
	reg("CGenericDescriptionSelector::AddAllDescriptionsDerivedFrom(classtype )", [S](const NativeContext &c) {
		auto *s = S(c);
		if (!s || !c.self()) return;
		std::string path = c.world.class_path_of_handle(c.arg<uint32_t>(0));
		if (path.empty()) return;
		for (const auto &[stem, f] : c.world.data().files()) {
			for (auto &o : f->objects()) {
				if (o.class_path == path || o.class_path.rfind(path + "\\", 0) == 0) s->ui[c.self()].pool.push_back(o.guid);
			}
		}
	});
	reg("CGenericDescriptionSelector::AddDescriptionGUID(class CGUID)", [S](const NativeContext &c) {
		auto *s = S(c);
		uint32_t g = c.arg<uint32_t>(0);
		if (!s || !c.self() || !g) return;
		de::Guid guid;
		memcpy(guid.bytes, c.mem().ptr(g, 16), 16);
		s->ui[c.self()].pool.push_back(guid);
	});
	// GUID pools are {+0 data, +4 count} of 16-byte entries.
	reg("CGenericDescriptionSelector::GetAvailableDescriptionGUIDPool(class CGUIDPool)", [S](const NativeContext &c) {
		auto *s = S(c);
		uint32_t p = c.arg<uint32_t>(0);
		if (!s || !c.self() || !p) return;
		auto &v = s->ui[c.self()].pool;
		uint32_t data = c.mem().alloc(uint32_t(std::max<size_t>(1, v.size())) * 16);
		for (size_t i = 0; i < v.size(); ++i) memcpy(c.mem().ptr(data + uint32_t(i) * 16, 16), v[i].bytes, 16);
		c.mem().set<uint32_t>(p, data);
		c.mem().set<uint32_t>(p + 4, uint32_t(v.size()));
	});
	reg("CGenericDescriptionSelector::SetAvailableDescriptionGUIDPool(class CGUIDPool)", [S](const NativeContext &c) {
		auto *s = S(c);
		uint32_t p = c.arg<uint32_t>(0);
		if (!s || !c.self() || !p) return;
		auto &v = s->ui[c.self()].pool;
		v.clear();
		uint32_t data = c.mem().get<uint32_t>(p), n = c.mem().get<uint32_t>(p + 4);
		for (uint32_t i = 0; i < n; ++i) {
			de::Guid g;
			memcpy(g.bytes, c.mem().ptr(data + i * 16, 16), 16);
			v.push_back(g);
		}
	});
	reg("CObject::CloseUIManagerStack()", [](const NativeContext &c) {
		if (c.world.close_ui_stack) c.world.close_ui_stack();
	});

	// --- Descriptions (Core DE_CDescription::NewResource 0x701f2170, OMF
	// DE_CFighterDescription::NewResource 0x35033080) ------------------------------------------
	// Construct(GUID): an instance of the object with that GUID.
	reg("CObject::class CObject Construct(class CGUID)", [](const NativeContext &c) {
		uint32_t g = c.arg<uint32_t>(0);
		de::Guid guid;
		if (g) memcpy(guid.bytes, c.mem().ptr(g, 16), 16);
		de::ObjectRef r = g ? c.world.data().by_guid(guid) : de::ObjectRef{};
		GameObject *tpl = c.world.global_object(r);
		c.ret_object(tpl ? c.world.copy_tree(*tpl) : nullptr);
	});
	// A description's resource is its first passive reference; fighters are built by the app
	// (make_fighter), pilots and other resources are copies of the referenced object.
	reg("CDescription::class CObject NewResource()", [](const NativeContext &c) { c.ret_object(new_resource(c.world, c.self(), nullptr)); });
	reg("CFighterDescription::class CObject NewResource(class CGUID)", [](const NativeContext &c) {
		uint32_t g = c.arg<uint32_t>(0);
		de::Guid guid{};
		bool set = false;
		if (g) {
			memcpy(guid.bytes, c.mem().ptr(g, 16), 16);
			for (uint8_t b : guid.bytes) set |= b != 0;
		}
		c.ret_object(new_resource(c.world, c.self(), set ? &guid : nullptr));
	});
	// --- The scene's actor list (DE_CScene::NumActors/Actor/Add) ----------------------------------
	// The scene a script's Scene() names: its actor's (+0x10), else its session's.
	auto caller_scene = [S](const NativeContext &c) -> GameObject * {
		Brain *b = c.world.brain_of(c.ex);
		if (b && b->owner && !session_of(c.world, b->owner)) {
			const GameObject *r = b->owner;
			while (r->parent) r = r->parent;
			if (c.world.root_session.count(r)) return c.world.root_session[r];
		}
		SessionState *s = S(c);
		return s ? s->obj : nullptr;
	};
	// Is `o` an actor of the scene this call's Scene() names?
	auto of_scene = [caller_scene](const NativeContext &c) {
		int view = World::gui_scene_view(c.self());
		GameObject *scene = view < 0 ? caller_scene(c) : nullptr;
		return [&c, view, scene](const GameObject *o) {
			if (!o->is_actor) return false;
			if (view >= 0) return in_gui(*o) && gui_view(*o) == view;
			return !in_gui(*o) && c.world.scene_session_of(o) == scene;
		};
	};
	reg("CScene::int NumActors()", [of_scene](const NativeContext &c) {
		uint32_t n = 0;
		auto in = of_scene(c);
		for (auto *o : c.world.scene_objects()) n += in(o) ? 1 : 0;
		c.ret_u32(n);
	});
	reg("CScene::class CActor Actor(int)", [of_scene](const NativeContext &c) {
		int i = c.arg<int32_t>(0), k = 0;
		GameObject *found = nullptr;
		auto in = of_scene(c);
		for (auto *o : c.world.scene_objects()) {
			if (in(o) && k++ == i) {
				found = o;
				break;
			}
		}
		c.ret_object(found);
	});
	// RemoveActorCaller (Core 0x7006dee0): the actor leaves the scene (DE_CScene vtable +0x134, the
	// counterpart of Add's +0x130), staying in its tree.
	reg("CScene::Remove(class CActor)", [](const NativeContext &c) {
		if (GameObject *o = c.arg_object(0)) o->removed = true;
	});
	reg("CScene::Add(class CActor)", [caller_scene](const NativeContext &c) {
		GameObject *o = c.arg_object(0);
		if (o) o->removed = false;
		if (o && !o->parent) {
			int view = World::gui_scene_view(c.self());
			if (view >= 0) {
				o->gui = true;
				o->view = view;
				c.world.root_session.try_emplace(o, nullptr);
			} else {
				c.world.root_session.try_emplace(o, caller_scene(c));
			}
		}
		c.world.enter_scene(o);
	});
	// IsPausedCaller (Core 0x7006de90): the scene's +0xf0, set by ObjInitContents and Pause, cleared by
	// Genesis and Continue. A session's scene starts when a modifier owns the environment.
	reg("CScene::bool IsPaused()", [caller_scene](const NativeContext &c) {
		if (World::gui_scene_view(c.self()) >= 0) {
			c.ret_bool(false);
			return;
		}
		SessionState *s = session_of(c.world, caller_scene(c));
		c.ret_bool(!s || !s->scene_started);
	});
	// DE_CScene::OwnTree (Core): the scene keeps a reference to every object of the branch, so it
	// lives as long as the scene; ours live until deleted anyway.
	reg("CScene::OwnBranch(class CObject)", [](const NativeContext &) {});
	// IsMemberOfCaller (Core 0x701f1130): the cast is in the description's cast list.
	reg("CDescription::bool IsMemberOf(class CCast)", [](const NativeContext &c) {
		de::ObjectRef d = source_of(c.self()), cast = source_of(c.arg_object(0));
		bool member = false;
		if (d && cast) {
			for (auto &m : read_description(c.world.data(), d).casts) member |= m.info().guid.str() == cast.info().guid.str();
		}
		static const bool dbg = getenv("OPENBG_DEBUG_CASTS") != nullptr;
		if (dbg) fprintf(stderr, "[cast] %s in %s: %d\n", d ? d.info().name.c_str() : "-", cast ? cast.info().name.c_str() : (c.arg_object(0) ? c.arg_object(0)->name.c_str() : "null"), member);
		c.ret_bool(member);
	});
	reg("CChooseFighterUIManager::class CGUID ChosenModel()", [S](const NativeContext &c) {
		uint32_t g = c.world.make_instance("CObject::CGUID", 1);
		if (auto *s = S(c); s && c.self() && s->ui.count(c.self())) memcpy(c.mem().ptr(g, 16), s->ui[c.self()].model.data(), 16);
		c.ret_u32(g);
	});
	reg("CGenericDescriptionSelector::class CGUID ChosenDescription()", [S](const NativeContext &c) {
		uint32_t g = c.world.make_instance("CObject::CGUID", 1);
		if (auto *s = S(c); s && c.self() && s->ui.count(c.self())) memcpy(c.mem().ptr(g, 16), s->ui[c.self()].chosen.data(), 16);
		c.ret_u32(g);
	});
}

}  // namespace game
