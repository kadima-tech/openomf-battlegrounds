#include "game_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <set>
#include <stdexcept>

#include "de/lighting.h"
#include "game/session.h"
#include "gl_util.h"
#include "scene_lights.h"

namespace app {

Fighter &setup_fighter(game::World &world, const std::string &name, bool user, bool with_stats) {
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

GameView::GameView(game::World &w) : world(w) {}

void GameView::init_gl() {
	overlay.load(world.data());
	particles_r.load();
	actors_r.image_of = [this](const de::ObjectRef &tm) -> std::optional<de::ObjectRef> {
		auto it = world.texture_overrides.find({tm.file, tm.index});
		if (it == world.texture_overrides.end()) return std::nullopt;
		return it->second;
	};
	// A Mantis bug's images carry the fighter's paint filter: the filter's fighter's pilot colours.
	actors_r.paint_of = [this](const de::ObjectRef &img) -> std::optional<std::array<std::array<float, 3>, 3>> {
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
	gl_ready_ = true;
}

void GameView::wire_session() {
	world.make_fighter = [this](const de::ObjectRef &actor) -> game::GameObject * {
		Fighter &nf = setup_fighter(world, actor.info().name, false, false);
		fighters.push_back(&nf);
		return nf.actor;
	};
	world.on_creature_owned = [this](game::GameObject *c, int slot) {
		for (auto *fi : fighters) {
			if (fi->actor == c && slot == 0) player = fi;
		}
		if (player) world.focus = player->actor;
		printf("slot %d: %s%s%s\n", slot, c->name.c_str(), c->user_controlled ? " (player)" : " (computer)", c->team >= 0 ? (", team " + std::to_string(c->team)).c_str() : "");
	};
}

void GameView::load_arena(const std::string &stem) {
	arena = stem;
	owners_set = false;
	arena_r = render::ArenaRenderer();
	arena_r.load(world.data(), stem);
	world.floor = [this](float x, float z, float from) { return arena_r.floor_height(x, z, from); };
	if (auto *f = world.data().file(stem)) render::g_lighting.ambient = de::read_ambient(world.data(), *f);
	printf("arena %s: %zu triangles\n", stem.c_str(), arena_r.triangles);
}

// The session's environment in the scene: its file is the arena.
std::string GameView::find_arena() const {
	for (game::GameObject *o : world.scene_objects()) {
		if (o->file && o->class_path == "Actor\\Creature\\Environment" && !game::in_gui(*o)) return o->file->master().name;
	}
	return {};
}

void GameView::load_renderers() {
	for (auto *fi : fighters) {
		if (!fi->renderer_loaded) {
			// The fighter's model (NewResource's chosen or default FighterModel child): its paint.
			const de::ObjectsFile *mf = world.data().file(fi->name + "Model1");
			size_t model = SIZE_MAX;
			for (game::GameObject *ch : fi->actor->children) {
				if (ch->class_path == "FighterModel" && ch->file == mf) model = ch->index;
			}
			fi->renderer.load(world.data(), *mf, world.data().file(fi->name), model);
			fi->renderer_loaded = true;
		}
	}
}

de::Transform GameView::ear() const {
	de::Transform e;
	if (!player) return e;
	std::function<game::GameObject *(game::GameObject *, const char *)> find = [&](game::GameObject *o, const char *n) -> game::GameObject * {
		for (auto *ch : o->children) {
			if (ch->name == n) return ch;
			if (auto *r = find(ch, n)) return r;
		}
		return nullptr;
	};
	auto *cam = find(player->actor, "CameraActor");
	auto *lis = find(player->actor, "Listener");
	e = cam ? cam->world() : player->actor->world();
	if (lis) e.t = lis->world().t;
	return e;
}

void GameView::draw(int dw, int dh, int fr, bool fixed_step) {
	if (!gl_ready_) return;
	if (arena.empty() || follow_scene) {
		// The session's environment came or went (a match began or ended).
		std::string a = find_arena();
		if (a != arena) {
			if (a.empty()) {
				arena.clear();
				fighters.clear();
				player = nullptr;
			} else {
				load_arena(a);
			}
		}
	}
	fighters.erase(std::remove_if(fighters.begin(), fighters.end(), [this](Fighter *f) { return !world.object(f->actor->handle); }), fighters.end());
	if (player && std::find(fighters.begin(), fighters.end(), player) == fighters.end()) player = nullptr;
	load_renderers();
	glViewport(0, 0, dw, dh);
	glEnable(GL_DEPTH_TEST);
	glClearColor(0.62f, 0.55f, 0.45f, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	de::Transform ft = player ? player->actor->world() : de::Transform{};
	float target[3] = {ft.t[0], ft.t[1] + fighter_radius * 0.8f, ft.t[2]};
	// Orbit relative to the fighter's facing so the camera stays behind it while it turns.
	auto facing = ft.rotate({0, 0, 1});
	float base_yaw = std::atan2(-facing[0], -facing[2]);
	if (!cam_init || fixed_step) {
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
		world.world_to_screen = [this](const std::array<float, 3> &p) {
			const float *m = screen_vp.data();
			float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
			float y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
			float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
			if (w <= 0) return std::array<float, 3>{0, 0, w};
			return std::array<float, 3>{(x / w + 1) / 2, (y / w + 1) / 2, w};
		};
	}
	// The frame's lights and their lighting owners (scene_lights.h).
	render::FrameLights frame_lights = render::collect_lights(world, fr, [](const game::GameObject *o) { return !game::in_gui(*o); });
	if (!world.lighting_enabled) render::g_lighting.lights.clear();  // Preferences\Renderer\Lighting Enabled
	const auto &light_owners = frame_lights.owners;
	auto light_owner_of = [&](const game::GameObject *o) { return frame_lights.owner_of(o); };
	const de::ObjectsFile *arena_file = world.data().file(arena);
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
			if (!o->copied || !o->is_actor || o->fighter || !o->file || o->meshes_disabled || game::in_gui(*o)) continue;
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
	{
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
	if (!no_glow && world.glow_enabled) {
		glow_r.begin();
		arena_r.draw_glow(vp, eye);
		{
			for (auto *fi : fighters) {
				if (fi->actor->spawned && fi->renderer_loaded) fi->renderer.draw_glow(vp, eye);
			}
		}
		if (!getenv("OPENBG_NO_PARTICLES")) particles_r.draw(world, vp, view, eye, true);
		glow_r.end(dw, dh);
	}
	overlay.draw(world, dw, dh, player ? player->actor : nullptr);
}

}  // namespace app
