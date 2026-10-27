// Session, slots, modifiers and clients (see session.h), and descriptions (DE_CDescription:
// what the creature and pilot pickers build fighters and pilots from).
#include "game/session.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

// ClassExecutor (Core 0x701434b0): contents [name][entry state name]; references: its class.
std::pair<de::ObjectRef, std::string> executor_class(const de::World &data, const de::ObjectRef &exec) {
	de::ObjectRef cls;
	for (auto &lists : reference_lists(data, exec)) {
		for (auto &l : lists) {
			for (auto &t : l) {
				if (t && t.info().class_path == "Class") cls = t;
			}
		}
	}
	std::string entry = "Entry";
	try {
		de::Contents c(exec.file->object_data(exec.info()).contents);
		de::begin_chunk(c);
		uint32_t n = c.u32();
		c.seek(c.pos() + n);
		uint32_t m = c.u32();
		if (m > 1 && m < 64) {
			auto bytes = c.bytes(m);
			entry.assign(bytes.begin(), bytes.end());
			while (!entry.empty() && entry.back() == 0) entry.pop_back();
		}
	} catch (const std::exception &) {
	}
	return {cls, entry};
}

}  // namespace

std::vector<Brain *> World::activate_executors(GameObject *o, const std::vector<de::ObjectRef> &properties, bool run) {
	std::vector<Brain *> out;
	if (!o || !activated_.insert(o).second) return out;
	for (auto &p : properties) {
		if (!p || p.info().class_path != "ClassExecutor") continue;
		auto [cls, entry] = executor_class(data_, p);
		if (!cls) continue;
		try {
			Brain *b = attach_brain(o, cls, entry);
			b->started = true;
			if (run) start_brain(b);
			out.push_back(b);
		} catch (const std::exception &e) {
			log.push_back(o->name + ": " + e.what());
		}
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

void World::enter_scene(GameObject *root) {
	if (!root) return;
	if (std::find(scene_roots.begin(), scene_roots.end(), root) == scene_roots.end()) scene_roots.push_back(root);
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
		branch.insert(branch.end(), o->children.begin(), o->children.end());
	}
	// Every actor of the branch is prepared (its executors attached) before any entry runs, so
	// that a script's Interface() calls find the behaviors of the actors around it.
	std::vector<Brain *> started;
	for (GameObject *o : branch) {
		if (o && o->is_actor && o->file && !o->brain) {
			auto b = activate_executors(o, properties_of(*o), false);
			started.insert(started.end(), b.begin(), b.end());
		}
	}
	for (Brain *b : started) start_brain(b);
}

void set_branch_active(GameObject *o, bool on) {
	if (!o) return;
	o->active = on;
	for (GameObject *c : o->children) set_branch_active(c, on);
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

static GameObject *reference_target(World &w, GameObject *o, const ReferenceFlags &flags, const de::Guid &target, bool rename) {
	GameObject *tpl = w.global_object(w.data().by_guid(target));
	if (!tpl) return nullptr;
	GameObject *n = w.copy_tree(*tpl);
	if (rename) n->name = o->name;
	n->local = (flags.place == 0 ? o->local : o->world()).then(tpl->local);
	for (auto &p : w.actor_properties({o->file, o->index})) {
		if (p) n->extra_properties.push_back(p);
	}
	if (flags.deactivate) set_branch_active(n, false);
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
	if (!read_reference(o, flags, target, rename) || (flags.mode != 2 && flags.mode != 3)) return nullptr;
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
	if (flags.place != 0 && flags.add && !flags.deactivate) enter_scene(n);
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
		if (in_scene) enter_scene(n);
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
			// The original parents it to the master; it hangs off the session here so that
			// Session().Interface(...) finds its behaviors.
			m->parent = s.obj;
			s.obj->children.push_back(m);
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
	// The description's values (e.g. "2 Wins" -> ScoreLimit) go onto the resource.
	if (res) {
		for (GameObject *ch : desc->children) {
			if (ch->is("Value")) {
				GameObject *v = w.copy_tree(*ch);
				v->parent = res;
				res->children.push_back(v);
			}
		}
	}
	return res;
}

void add_session_modifier(World &w, GameObject *m) {
	auto &s = *w.session;
	m->parent = s.obj;
	s.obj->children.push_back(m);
	s.modifiers.push_back(m);
	make_node(w, s, m);
}

SessionState &start_session(World &w, const std::vector<de::ObjectRef> &modifiers) {
	auto s = std::make_shared<SessionState>();
	s->obj = w.create("Session", "Session\\Standard");
	w.session = s;
	for (auto &r : modifiers) {
		GameObject *tpl = w.global_object(r);
		if (!tpl) continue;
		GameObject *m = w.copy_tree(*tpl);
		m->parent = s->obj;
		s->obj->children.push_back(m);
		s->modifiers.push_back(m);
		make_node(w, *s, m);
	}
	return *s;
}

void begin_session(World &w) {
	auto &s = *w.session;
	// InitializeModifiers walks the growing list: attached modifiers are initialised too.
	for (size_t i = 0; i < s.modifiers.size(); ++i) {
		initialize_node(w, s, *s.node(s.modifiers[i]));
	}
	if (getenv("OPENBG_DEBUG_SESSION")) {
		for (auto *m : s.modifiers) {
			auto &n = *s.node(m);
			fprintf(stderr, "[session] modifier %s: %zu properties, brain %s, hooks%s%s%s%s\n", m->name.c_str(), n.properties.size(),
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
}

SessionClient &add_client(World &w, bool user, const std::string &name) {
	auto &s = *w.session;
	auto &c = *s.clients.emplace_back(std::make_unique<SessionClient>());
	c.obj = w.create_child(s.obj, "Client" + std::to_string(s.clients.size()), "Client\\Standard");
	c.user = user;
	c.local = true;
	c.active = true;
	c.player_name = name;
	return c;
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

void update_session(World &w) {
	{
		auto hooks = w.delta_tick_hooks;
		float ms = World::kTickMs;
		for (auto &[b, m] : hooks) {
			if (b->inst.shut_down) continue;
			std::vector<uint8_t> args(4);
			memcpy(args.data(), &ms, 4);
			b->call_handle(w, m, args);
		}
	}
	if (w.render_ticks_per_frame) render_tick(w, World::kTickMs);
	if (!w.session || !w.session->begun) return;
	auto &s = *w.session;
	// DE_CStandardSession::Update: connect each active client without a slot to the next
	// connectable one.
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
	// A preset UI choice completes: every node's UIManagerComplete hook hears about it (each
	// checks whether the manager is its own).
	for (auto &[ui, u] : s.ui) {
		if (!u.pending) continue;
		u.pending = false;
		std::vector<SessionNode *> nodes;
		for (auto &[o, n] : s.nodes) nodes.push_back(n.get());
		for (auto *n : nodes) call_hook(w, n->ui_complete, ui->handle, true);
	}
	// The loading screen (the engine shows it): while anything reports it needs loading
	// (SessionQuerryIsLoadingRequired counts them in INT1), every slot's choices are made and
	// nothing halts loading, SessionLoadElements goes to the session and its slots once per
	// element with INT1 = 1; the first loader still waiting takes it and decrements INT1.
	if (!s.slots.empty()) {
		auto ask = [&](const char *handle, int32_t initial) {
			uint32_t e = new_event(w, handle);
			w.memory().set<int32_t>(e + 4, initial);
			w.send_event(s.obj, e);
			for (size_t k = 0; k < s.slots.size(); ++k) w.send_event(s.slots[k]->obj, e);
			return w.memory().get<int32_t>(e + 4);
		};
		bool clients = true;
		for (auto &sl : s.slots) clients &= sl->client != nullptr;
		int32_t required = clients ? ask("SessionQuerryIsLoadingRequired", 0) : 0;
		if (required > 0 && ask("SlotQuerryUserChoicesComplete", 1) && ask("SessionQuerryIsReadyToLoad", 1)) {
			for (int32_t k = 0; k < required; ++k) ask("SessionLoadElements", 1);
			s.loaded = true;
		}
	}
	for (size_t i = 0; i < s.modifiers.size(); ++i) {
		call_hook(w, s.node(s.modifiers[i])->update);
	}
	for (size_t i = 0; i < s.slots.size(); ++i) {
		auto mods = s.slots[i]->modifiers;
		for (auto *m : mods) {
			if (auto *n = s.node(m)) call_hook(w, n->update);
		}
	}
}

void register_session_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };
	auto stub = [&](const char *key) { w.register_native(key, [](const NativeContext &) {}); };
	auto S = [](const NativeContext &c) -> SessionState * { return c.world.session.get(); };
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
		stub((std::string(cls) + "RegisterState()").c_str());
		stub((std::string(cls) + "RegisterUrgentState()").c_str());
		reg(std::string(cls) + "class CStandardSession Session()", [S](const NativeContext &c) {
			auto *s = S(c);
			c.ret_object(s ? s->obj : nullptr);
		});
		reg(std::string(cls) + "class CScene Scene()", [](const NativeContext &c) { c.ret_object(c.world.create("Scene", "Scene")); });
		reg(std::string(cls) + "bool IsServer()", [](const NativeContext &c) { c.ret_bool(true); });
		reg(std::string(cls) + "bool IsSimulating()", [](const NativeContext &c) { c.ret_bool(true); });
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
			c.world.enter_scene(s->environment);
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
		for (auto &t : n->components) {
			GameObject *tpl = c.world.global_object(t);
			if (!tpl) continue;
			bool present = false;
			for (auto *ch : creature->children) present |= ch->file == tpl->file && ch->index == tpl->index;
			if (present) continue;
			GameObject *comp = c.world.copy_tree(*tpl);
			comp->parent = creature;
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
	reg("CStandardSessionSlot::OwnCreature(class CCreature)", [slot](const NativeContext &c) {
		auto *sl = slot(c);
		if (!sl) return;
		sl->creature = c.arg_object(0);
		if (sl->creature) {
			sl->creature->user_controlled = sl->client && sl->client->user && !c.world.all_ai;
			// The creature is in the scene now: its scripts (and its pilot's, HUD's...) start,
			// the fighter's entry running at once as AssureActivated does.
			c.world.enter_scene(sl->creature);
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
	reg("CSession::int MaxPlayerClients()", [S](const NativeContext &c) { c.ret_u32(S(c) ? uint32_t(S(c)->max_player_clients) : 2); });
	reg("CSession::bool IsReplaying()", [](const NativeContext &c) { c.ret_bool(false); });
	reg("CSession::bool IsLocal()", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CSession::bool IsReadyToLoad()", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CSession::bool IsFullySequenced()", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CSession::bool AreMasterRequirementsSatisfied()", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CSession::string StatusDescription()", [](const NativeContext &c) { c.ret_u32(c.world.string("")); });
	stub("CSession::LoadingComplete()");
	stub("CSession::EndAndDie()");
	stub("CStandardSession::ForceClientsToLoad()");
	reg("CSession::class CClient ControllingClient()", [user_client](const NativeContext &c) {
		auto *cl = user_client(c);
		c.ret_object(cl ? cl->obj : nullptr);
	});
	reg("CSession::int NumClients()", [S](const NativeContext &c) { c.ret_u32(S(c) ? uint32_t(S(c)->clients.size()) : 0); });
	reg("CSession::int NumPlayersAfterPopulation()", [S](const NativeContext &c) { c.ret_u32(S(c) ? uint32_t(S(c)->clients.size()) : 0); });
	reg("CSession::SetCamera(class CActor)", [S](const NativeContext &c) { if (auto *s = S(c)) s->camera = c.arg_object(0); });
	reg("CSession::class CActor Camera()", [S](const NativeContext &c) { c.ret_object(S(c) ? S(c)->camera : nullptr); });
	reg("CStandardSession::class CStandardSessionModifier Master()", [S](const NativeContext &c) {
		auto *s = S(c);
		c.ret_object(s && !s->modifiers.empty() ? s->modifiers[0] : nullptr);
	});
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
		} else {
			c.world.log.push_back("session: no preset choice for UI \"" + u.description + "\"");
		}
	});
	reg("CClient::bool IsUIManagerActive()", [](const NativeContext &c) { c.ret_bool(false); });
	stub("CUIManager::SetHaltLoading(bool)");
	reg("CUIManager::bool IsActiveLocally()", [](const NativeContext &c) { c.ret_bool(false); });
	reg("CUIManager::bool IsDisplayed()", [](const NativeContext &c) { c.ret_bool(false); });
	reg("CObject::class CGenericDescriptionSelector NewGenericDescriptionSelector()", [](const NativeContext &c) {
		c.ret_object(c.world.create_child(nullptr, "GenericDescriptionSelector", "UIManager\\GenericDescriptionSelector"));
	});
	reg("CObject::class CChooseFighterUIManager NewChooseFighterUIManager()", [](const NativeContext &c) {
		c.ret_object(c.world.create_child(nullptr, "ChooseFighterUIManager", "UIManager\\GenericDescriptionSelector\\ChooseFighter"));
	});
	reg("CGenericDescriptionSelector::SetUIDescription(string)", [S](const NativeContext &c) {
		if (auto *s = S(c); s && c.self()) s->ui[c.self()].description = c.arg_string(0);
	});
	for (const char *k : {"CGenericDescriptionSelector::AddAllDescriptionsDerivedFrom(classtype )",
	                      "CGenericDescriptionSelector::SetAutoselectDescription(string)",
	                      "CGenericDescriptionSelector::AddDescriptionGUID(class CGUID)",
	                      "CGenericDescriptionSelector::GetAvailableDescriptionGUIDPool(class CGUIDPool)",
	                      "CGenericDescriptionSelector::SetAvailableDescriptionGUIDPool(class CGUIDPool)",
	                      "CObject::CloseUIManagerStack()"}) {
		stub(k);
	}

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
	reg("CScene::int NumActors()", [](const NativeContext &c) {
		uint32_t n = 0;
		for (auto *o : c.world.scene_objects()) n += o->is_actor ? 1 : 0;
		c.ret_u32(n);
	});
	reg("CScene::class CActor Actor(int)", [](const NativeContext &c) {
		int i = c.arg<int32_t>(0), k = 0;
		GameObject *found = nullptr;
		for (auto *o : c.world.scene_objects()) {
			if (o->is_actor && k++ == i) {
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
	reg("CScene::Add(class CActor)", [](const NativeContext &c) {
		GameObject *o = c.arg_object(0);
		if (o) o->removed = false;
		if (o && !o->active) set_branch_active(o, true);  // a copy of a deactivated template comes to life
		c.world.enter_scene(o);
	});
	// DE_CScene::OwnTree (Core): the scene keeps a reference to every object of the branch, so it
	// lives as long as the scene; ours live until deleted anyway.
	reg("CScene::OwnBranch(class CObject)", [](const NativeContext &) {});
	reg("CDescription::bool IsMemberOf(class CCast)", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CChooseFighterUIManager::class CGUID ChosenModel()", [](const NativeContext &c) { c.ret_u32(c.world.make_instance("CObject::CGUID", 1)); });
	reg("CGenericDescriptionSelector::class CGUID ChosenDescription()", [S](const NativeContext &c) {
		uint32_t g = c.world.make_instance("CObject::CGUID", 1);
		if (auto *s = S(c); s && c.self() && s->ui.count(c.self())) memcpy(c.mem().ptr(g, 16), s->ui[c.self()].chosen.data(), 16);
		c.ret_u32(g);
	});
}

}  // namespace game
