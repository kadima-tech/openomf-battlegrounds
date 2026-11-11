#include "pie.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "audio.h"
#include "de/platform.h"
#include "editor.h"
#include "game/observer.h"
#include "game/runtime.h"
#include "game/session.h"
#include "game_view.h"
#include "icons.h"
#include "pilot.h"
#include "render.h"
#include "theme.h"
#include "widgets.h"
#include "world_state.h"

using namespace theme;
using Vec3 = std::array<float, 3>;
namespace fs = std::filesystem;

namespace {

void set_env(const char *name, const std::string &value) {
#ifdef _WIN32
	_putenv_s(name, value.c_str());
#else
	if (value.empty()) unsetenv(name);
	else setenv(name, value.c_str(), 1);
#endif
}

de::ObjectRef find_in(const de::World &data, const std::string &file, const std::string &cls, const std::string &name) {
	if (auto *ff = data.file(file)) {
		for (uint32_t i = 0; i < ff->objects().size(); ++i) {
			if (ff->objects()[i].class_path == cls && ff->objects()[i].name == name) return {ff, i};
		}
	}
	return data.find(cls, name);
}

float call_float(game::World &w, game::Brain *b, const char *sig) {
	if (!b) return -1;
	std::vector<uint8_t> args(4);
	if (!b->call(w, sig, args)) return -1;
	float f;
	memcpy(&f, args.data(), 4);
	return f;
}

game::Brain *stats_brain(const game::GameObject *f) {
	for (auto *ch : f->children) {
		if (ch->brain && ch->brain->cls.name == "PFighterStats") return ch->brain;
	}
	return nullptr;
}

std::array<float, 4> clip_of(const float *m, const Vec3 &p) {
	return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
	        m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14], m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15]};
}

bool to_screen(const Pane &p, const float *vp, const Vec3 &w, ImVec2 *out) {
	auto c = clip_of(vp, w);
	if (c[3] <= 1e-3f) return false;
	*out = ImVec2(p.p0.x + (c[0] / c[3] * 0.5f + 0.5f) * p.size.x, p.p0.y + (0.5f - c[1] / c[3] * 0.5f) * p.size.y);
	return true;
}

const char *const kDifficulties[] = {"Rookie", "Amateur", "Veteran", "Champion", "World Champion", "Master"};

}  // namespace

PlaySession::PlaySession(Setup setup) : setup_(std::move(setup)) {
	started_ = ImGui::GetTime();
	const char *old = getenv("OPENBG_ADDONS");
	had_addons_ = old != nullptr;
	saved_addons_ = old ? old : "";
	set_env("OPENBG_ADDONS", setup_.addon.empty() ? std::string() : setup_.addon.string());
	loader_ = std::thread([this] {
		try {
			setup_world();
		} catch (const std::exception &ex) {
			error_ = ex.what();
		}
		loaded_ = true;
	});
	if (setup_.fixed_step) loader_.join();
}

PlaySession::~PlaySession() {
	if (loader_.joinable()) loader_.join();
	mixer_.reset();
	view_.reset();
	world_.reset();
	render::g_live_values = nullptr;
	render::g_bump_enabled = nullptr;
	render::g_texture_detail = nullptr;
	if (!loaded_ || had_addons_) set_env("OPENBG_ADDONS", saved_addons_);
}

void PlaySession::setup_world() {
	const PlaySettings &s = setup_.settings;
	const std::set<std::string> keep = setup_.keep;
	if (keep.empty()) world_ = std::make_unique<game::World>(setup_.data);
	else world_ = std::make_unique<game::World>(setup_.data, [&keep](const std::string &stem) { return keep.count(stem) > 0; });
	game::World &world = *world_;
	world.show_rules = [&world](game::GameObject *rules) {
		auto &r = world.rules_displays[rules];
		r.shown = r.displayed = r.authorized = true;
	};
	world.all_ai = setup_.simulate;
	world.ai_difficulty = s.difficulty;
	world.render_ticks_per_frame = true;
	view_ = std::make_unique<app::GameView>(world);
	app::GameView &view = *view_;
	direct_ = !s.opponent_ai && !setup_.simulate;
	if (!direct_) {
		// The game's own session, as openbg-fight runs it: the robots, pilots and arena picked up
		// front as BSinglePlayerSubSession does, the scripts creating and refereeing the fighters.
		view.wire_session();
		de::ObjectRef master = find_in(world.data(), "Session", "SessionModifier\\StandardSessionMaster", s.master);
		if (!master) throw std::runtime_error("no session master " + s.master);
		auto &ss = game::start_session(world, {master});
		std::string wins_name = s.wins == 1 ? "One Win" : std::to_string(s.wins) + " Wins";
		de::ObjectRef d = find_in(world.data(), "Session", "Description\\SessionModifier", wins_name);
		game::GameObject *tpl = d ? world.global_object(d) : nullptr;
		if (game::GameObject *m = tpl ? game::new_resource(world, world.copy_tree(*tpl), nullptr) : nullptr) game::add_session_modifier(world, m);
		de::Guid g;
		memcpy(g.bytes, setup_.environment_description.b, 16);
		ss.ui_choices["Choose Environment"] = g;
		game::begin_session(world);
		auto pick = [&](size_t slot, const char *handle, const de::ObjectRef &desc) {
			if (!desc || slot >= ss.slots.size()) return;
			uint32_t gm = world.make_instance("CObject::CGUID", 1);
			memcpy(world.memory().ptr(gm, 16), desc.info().guid.bytes, 16);
			uint32_t e = game::new_event(world, handle);
			world.memory().set<uint32_t>(e + 8, gm);
			world.send_event(ss.slots[slot]->obj, e);
		};
		std::vector<std::pair<std::string, std::string>> picks = {{s.robot, s.pilot}, {s.opponent, s.opponent_pilot}};
		for (size_t i = 0; i < picks.size(); ++i) {
			pick(i, "SessionSlotSelectCreature", find_in(world.data(), picks[i].first, "Description\\Creature\\Fighter", picks[i].first));
			pick(i, "SessionSlotSelectPilot", find_in(world.data(), "Pilots", "Description\\Pilot", picks[i].second));
			game::add_client(world, i == 0, picks[i].second);
		}
		return;
	}
	// An idle dummy: no session. The robots are made as BCreatureAndPilotPicker would, the
	// opponent without a pilot (OPENBG_IDLE_AI's still target), and spawned by the editor.
	auto make_fighter = [&](const std::string &who, bool user) -> app::Fighter & {
		app::Fighter *made = nullptr;
		world.make_fighter = [&](const de::ObjectRef &actor) -> game::GameObject * {
			made = &app::setup_fighter(world, actor, user);
			return made->actor;
		};
		de::ObjectRef fd = find_in(world.data(), who, "Description\\Creature\\Fighter", who);
		if (game::GameObject *desc = fd ? world.global_object(fd) : nullptr) game::new_resource(world, desc, nullptr);
		world.make_fighter = nullptr;
		if (!made) made = &app::setup_fighter(world, who, user);
		return *made;
	};
	auto make_pilot = [&](app::Fighter &fi, const std::string &who, bool idle) {
		de::ObjectRef dref = find_in(world.data(), "Pilots", "Description\\Pilot", who);
		if (idle) {
			if (!dref) return;
			if (auto res = world.data().find("Actor\\Creature\\Pilot", who)) {
				static std::deque<game::Pilot> pilots;
				if (auto p = game::read_pilot(*res.file, res.info(), s.difficulty)) fi.actor->pilot = &pilots.emplace_back(*p);
			}
			return;
		}
		game::GameObject *tpl = dref ? world.global_object(dref) : nullptr;
		game::GameObject *desc = tpl ? world.copy_tree(*tpl) : nullptr;
		game::GameObject *p = desc ? game::new_resource(world, desc, nullptr) : nullptr;
		if (!p) return;
		desc->parent = p;
		p->children.push_back(desc);
		p->parent = fi.actor;
		fi.actor->children.push_back(p);
	};
	app::Fighter &f = make_fighter(s.robot, true);
	make_pilot(f, s.pilot, false);
	view.player = &f;
	view.fighters.push_back(&f);
	view.fighters.push_back(&make_fighter(s.opponent, false));
	make_pilot(*view.fighters.back(), s.opponent_pilot, true);
	de::Guid g;
	memcpy(g.bytes, setup_.environment_description.b, 16);
	game::GameObject *desc = world.global_object(world.data().by_guid(g));
	if (game::GameObject *env = desc ? game::new_resource(world, desc, nullptr) : nullptr) world.enter_scene(env);
	for (auto *fi : view.fighters) {
		bool user = fi->actor->user_controlled;
		world.enter_scene(fi->actor);
		world.set_user_controlled(*fi->actor, user ? 0 : -1);
		if (fi->stats) world.start_brain(fi->stats);
	}
}

void PlaySession::spawn_direct() {
	game::World &world = *world_;
	std::vector<game::GameObject *> gathered;
	for (game::GameObject *o : world.scene_objects()) {
		if (world.has_interface_named(o, "BPlayerSpawnPoint")) gathered.push_back(o);
	}
	std::stable_sort(gathered.begin(), gathered.end(), [](auto *a, auto *b) { return a->name < b->name; });
	std::vector<de::Transform> points;
	for (auto *o : gathered) points.push_back(o->world());
	if (points.size() < 2) {
		de::Transform a, b;
		a.t = {0, 40, -60};
		b.t = {0, 40, 60};
		b.r = {-1, 0, 0, 0, 1, 0, 0, 0, -1};
		points = {a, b};
	}
	for (size_t i = 0; i < view_->fighters.size(); ++i) {
		app::Fighter &f = *view_->fighters[i];
		const de::Transform &at = points[i % points.size()];
		auto &mem = world.memory();
		uint32_t pos = mem.alloc(12), orient = mem.alloc(36), mom = mem.alloc(12);
		for (int k = 0; k < 3; ++k) mem.set<float>(pos + uint32_t(k) * 4, at.t[size_t(k)]);
		memcpy(mem.ptr(orient, 36), at.r.data(), 36);
		std::vector<uint8_t> args(16);
		memcpy(&args[4], &pos, 4);
		memcpy(&args[8], &orient, 4);
		memcpy(&args[12], &mom, 4);
		f.actor->spawned = true;
		f.brain->call(world, "bool ObjectSpawn(class CVector,class CCoordinateSystem,class CVector)", args);
	}
	if (view_->player) game::set_renderer_camera(world, game::creature_camera(world, view_->player->actor, 0));
}

bool PlaySession::ready() {
	if (gl_ready_) return true;
	if (!loaded_) return false;
	if (loader_.joinable()) loader_.join();
	set_env("OPENBG_ADDONS", saved_addons_);
	if (!error_.empty() || !world_) return false;
	view_->init_gl();
	std::string stem = setup_.arena_stem;
	int environment = -1;
	if (!setup_.addon.empty() && world_->data().file(setup_.addon.stem().string())) {
		de::Guid g;
		memcpy(g.bytes, setup_.environment_description.b, 16);
		de::ObjectRef d = world_->data().by_guid(g);
		const de::ObjectsFile *addon = world_->data().file(setup_.addon.stem().string());
		if (d && d.file->master().name == addon->master().name) {
			stem = setup_.addon.stem().string();
			auto refs = game::passive_references(world_->data(), d);
			if (!refs.empty() && refs[0] && refs[0].file == addon) environment = int(refs[0].index);
		}
	}
	view_->load_arena(stem, environment);
	if (!setup_.settings.mute && !setup_.fixed_step) {
		mixer_ = std::make_unique<audio::Mixer>();
		if (!mixer_->open()) mixer_.reset();
	}
	gl_ready_ = true;
	return true;
}

void PlaySession::run_ticks(int n) {
	game::World &world = *world_;
	for (int k = 0; k < n; ++k) {
		if (direct_ && frame_ == 30) spawn_direct();
		if (debug.full_energy) {
			for (auto *fi : view_->fighters) {
				if (game::Brain *b = stats_brain(fi->actor)) {
					std::vector<uint8_t> args(4, 0);
					float full = 1e6f;
					memcpy(&args[0], &full, 4);
					b->call(world, "AdjustEnergy(float)", args);
				}
			}
		}
		world.tick(game::World::kFrameMs);
		++frame_;
	}
	if (mixer_) {
		mixer_->play_requests(world, view_->ear());
		mixer_->update_music(world);
	}
	world.sound_requests.clear();
}

void PlaySession::step() {
	if (gl_ready_) run_ticks(1);
}

void PlaySession::frame(double ms, bool input) {
	if (!gl_ready_) return;
	game::World &world = *world_;
	world.input = {};
	if (input && !ejected_) {
		auto k = [](ImGuiKey key) { return ImGui::IsKeyDown(key); };
		world.input.up = k(ImGuiKey_UpArrow);
		world.input.down = k(ImGuiKey_DownArrow);
		world.input.left = k(ImGuiKey_LeftArrow);
		world.input.right = k(ImGuiKey_RightArrow);
		world.input.left_punch = k(ImGuiKey_A);
		world.input.right_punch = k(ImGuiKey_S);
		world.input.left_kick = k(ImGuiKey_Z);
		world.input.right_kick = k(ImGuiKey_X);
		world.input.jump = k(ImGuiKey_Space);
		world.input.evade = k(ImGuiKey_LeftShift);
		world.input.duck = k(ImGuiKey_C);
	}
	if (paused) return;
	if (setup_.fixed_step) {
		run_ticks(1);
		return;
	}
	accum_ += std::min(ms, 200.0) * speed;
	int n = 0;
	while (accum_ >= game::World::kFrameMs && n < 6) accum_ -= game::World::kFrameMs, ++n;
	if (n) run_ticks(n);
}

void PlaySession::render(Pane &p) {
	if (!gl_ready_) return;
	const int w = std::max(16, int(p.size.x * ImGui::GetIO().DisplayFramebufferScale.x));
	const int h = std::max(16, int(p.size.y * ImGui::GetIO().DisplayFramebufferScale.y));
	GLint prev_fbo = 0, prev_vp[4];
	bind_pane(p, w, h, &prev_fbo, prev_vp);
	if (ejected_) {
		view_->camera_override = [this](int dw, int dh, float *proj, float *view, float *eye) { eject_cam_.matrices(dw, dh, proj, view, eye); };
	} else {
		view_->camera_override = nullptr;
	}
	view_->draw(w, h, frame_, setup_.fixed_step);
	memcpy(p.vp, view_->drawn_vp.data(), sizeof p.vp);
	if (ejected_) {
		float proj[16];
		eject_cam_.matrices(w, h, proj, p.view, p.eye);
	}
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev_fbo));
	glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
}

void PlaySession::set_ejected(bool on) {
	if (on == ejected_) return;
	ejected_ = on;
	if (!on || !gl_ready_) return;
	// Start the free camera where the game's was: the eye and direction from its view-projection.
	float inv[16];
	const float *m = view_->drawn_vp.data();
	auto unproject = [&](float x, float y, float z) {
		float v[4] = {x, y, z, 1}, o[4] = {0, 0, 0, 0};
		for (int r = 0; r < 4; ++r) {
			for (int c = 0; c < 4; ++c) o[r] += inv[c * 4 + r] * v[c];
		}
		return Vec3{o[0] / o[3], o[1] / o[3], o[2] / o[3]};
	};
	if (invert4(m, inv)) {
		Vec3 a = unproject(0, 0, -1), b = unproject(0, 0, 0.5f);
		Vec3 f = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
		float l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
		if (l > 0) {
			eject_cam_.mode = Camera::Perspective;
			eject_cam_.pos = a;
			eject_cam_.yaw = std::atan2(f[0] / l, -f[2] / l);
			eject_cam_.pitch = -std::asin(std::clamp(f[1] / l, -1.0f, 1.0f));
		}
	}
}

bool PlaySession::pick_fighter(Pane &p, ImVec2 mouse) {
	if (!gl_ready_) return false;
	for (auto *fi : view_->fighters) {
		ImVec2 s;
		auto t = fi->actor->world().t;
		t[1] += 60;
		if (to_screen(p, p.vp, t, &s) && std::hypot(s.x - mouse.x, s.y - mouse.y) < ui::px(60)) {
			inspect_ = fi->actor;
			return true;
		}
	}
	return false;
}

app::Fighter *PlaySession::inspected() const {
	if (!view_) return nullptr;
	for (auto *fi : view_->fighters) {
		if (fi->actor == inspect_) return fi;
	}
	return view_->player ? view_->player : (view_->fighters.empty() ? nullptr : view_->fighters[0]);
}

void PlaySession::apply_pilot_stats(const std::string &pilot, const std::array<float, 4> &stats) {
	if (!view_) return;
	for (auto *fi : view_->fighters) {
		const game::Pilot *pl = game::pilot_of(fi->actor);
		if (!pl || pl->name != pilot) continue;
		auto *mp = const_cast<game::Pilot *>(pl);
		mp->stats = {stats[0], stats[1], stats[2], stats[3]};
	}
}

std::string PlaySession::summary() const {
	std::string out = "frame " + std::to_string(frame_);
	if (!view_) return out;
	for (auto *fi : view_->fighters) {
		auto *m = fi->brain && fi->brain->exec ? fi->brain->exec->current_method() : nullptr;
		out += "; " + fi->name + " " + (m ? m->cm->signature : "-");
	}
	return out;
}

void PlaySession::draw_overlays(Pane &p) {
	if (!gl_ready_) return;
	game::World &world = *world_;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	auto line = [&](const Vec3 &a, const Vec3 &b, unsigned c, float th) {
		ImVec2 sa, sb;
		if (to_screen(p, p.vp, a, &sa) && to_screen(p, p.vp, b, &sb)) dl->AddLine(sa, sb, rgb(c, 220), th);
	};
	auto sphere = [&](const Vec3 &c, float r, unsigned col) {
		for (int ax = 0; ax < 3; ++ax) {
			Vec3 prev{};
			for (int k = 0; k <= 24; ++k) {
				float a = float(k) / 24 * 6.2831853f;
				Vec3 l = ax == 0 ? Vec3{0, std::cos(a) * r, std::sin(a) * r} : ax == 1 ? Vec3{std::cos(a) * r, 0, std::sin(a) * r} : Vec3{std::cos(a) * r, std::sin(a) * r, 0};
				Vec3 w = {c[0] + l[0], c[1] + l[1], c[2] + l[2]};
				if (k) line(prev, w, col, 1.2f);
				prev = w;
			}
		}
	};
	if (debug.hit_spheres) {
		for (game::GameObject *o : world.affectors()) {
			if (o->shape != game::GameObject::Shape::Sphere || !o->active || !game::in_scene(*o)) continue;
			bool hit = o->groups & 0x100000, attack = o->groups & 0x1000;
			if (!hit && !attack) continue;
			float r = o->falloff > 0 ? o->falloff : o->hotspot;
			if (r <= 0) continue;
			sphere(o->world().t, r, attack ? 0xff4d4d : 0x4fc3f7);
		}
	}
	if (debug.nav) {
		for (game::GameObject *o : world.scene_objects()) {
			if (!o->is("UniqueActorProperty\\NavigationLink") || !o->file || !o->parent) continue;
			auto lists = game::reference_lists(world.data(), {o->file, o->index});
			if (lists.size() > 2 && !lists[2].empty() && !lists[2][0].empty() && lists[2][0][0]) {
				if (game::GameObject *to = world.referent(o, lists[2][0][0])) line(o->parent->world().t, to->world().t, 0xf5d36b, 1.5f);
			}
		}
		for (auto *fi : view_->fighters) {
			auto from = fi->actor->world().t;
			from[1] += 60;
			if (game::GameObject *t = world.object(fi->actor->current_target)) {
				auto to = t->world().t;
				to[1] += 60;
				line(from, to, 0xff6b6b, 2.0f);
			}
			for (auto &c : fi->actor->commands.list) {
				if (c.type & 2) {
					line(from, c.step_distance > 0 ? c.step : c.pos, 0x8fe3a8, 2.0f);
					ImVec2 s;
					if (to_screen(p, p.vp, c.pos, &s)) dl->AddCircleFilled(s, ui::px(5), rgb(0x8fe3a8));
				}
			}
		}
	}
	if (ejected_) {
		for (auto *fi : view_->fighters) {
			ImVec2 s;
			auto t = fi->actor->world().t;
			t[1] += 150;
			if (!to_screen(p, p.vp, t, &s)) continue;
			bool sel = fi == inspected();
			std::string label = fi->name + (fi->actor->user_controlled ? " (player)" : " (AI)");
			ImVec2 ts = ImGui::CalcTextSize(label.c_str());
			dl->AddRectFilled(s - ImVec2(ts.x / 2 + 6, 3), s + ImVec2(ts.x / 2 + 6, ts.y + 3), rgb(sel ? 0x3a3566 : 0x1d1e26, 220), 4);
			dl->AddText(s - ImVec2(ts.x / 2, 0), rgb(sel ? 0xffffff : 0xd3d5e0), label.c_str());
		}
	}
	// Status, top centre.
	std::string st = paused ? "PAUSED" : (speed != 1 ? (speed == 0.25f ? "0.25x" : speed == 0.5f ? "0.5x" : "1x") : "");
	if (ejected_) st = std::string("EJECTED (F8)") + (st.empty() ? "" : "  \xc2\xb7  " + st);
	if (!st.empty()) {
		ImVec2 ts = ImGui::CalcTextSize(st.c_str());
		ImVec2 at(p.p0.x + (p.size.x - ts.x) / 2, p.p0.y + ui::px(46));
		dl->AddRectFilled(at - ImVec2(10, 4), at + ts + ImVec2(10, 4), rgb(0x141519, 200), 4);
		dl->AddText(at, rgb(ejected_ ? 0xffc98a : 0x8fe3a8), st.c_str());
	}
}

void PlaySession::draw_inspector() {
	if (!gl_ready_) {
		ui::centered_note(ICON_CIRCLE_PLAY, "Starting the match", "Reading the files the match needs...");
		return;
	}
	app::Fighter *fi = inspected();
	if (!fi) {
		ui::centered_note(ICON_USER, "No creature", "The match has not made its fighters yet.");
		return;
	}
	game::World &world = *world_;
	game::GameObject &a = *fi->actor;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(8), ui::px(5)));
	// Creature picker.
	ui::form_label("Creature", ui::px(110));
	ImGui::SetNextItemWidth(-1);
	if (ui::begin_combo("##creature", (fi->name + (a.user_controlled ? " (player)" : " (AI)")).c_str())) {
		for (auto *f : view_->fighters) {
			if (ImGui::Selectable((f->name + (f->actor->user_controlled ? " (player)" : " (AI)")).c_str(), f == fi)) inspect_ = f->actor;
		}
		ImGui::EndCombo();
	}
	auto row = [&](const char *label, const std::string &value, unsigned colour = kText) {
		ui::form_label(label, ui::px(110));
		ImGui::AlignTextToFramePadding();
		ImGui::PushStyleColor(ImGuiCol_Text, vec(colour));
		ImGui::TextUnformatted(value.c_str());
		ImGui::PopStyleColor();
	};
	auto bar = [&](const char *label, float v, unsigned colour) {
		ui::form_label(label, ui::px(110));
		float h = ui::px(14);
		ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0, (ImGui::GetFrameHeight() - h) / 2));
		ImVec2 p0 = ImGui::GetCursorScreenPos();
		float w = ImGui::GetContentRegionAvail().x;
		ImDrawList *dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(p0, p0 + ImVec2(w, h), rgb(kInput), 3);
		if (v >= 0) dl->AddRectFilled(p0, p0 + ImVec2(w * std::clamp(v, 0.0f, 1.0f), h), rgb(colour), 3);
		char buf[16];
		snprintf(buf, sizeof buf, v >= 0 ? "%.0f%%" : "-", double(v * 100));
		dl->AddText(p0 + ImVec2(6, (h - ImGui::GetFontSize()) / 2), rgb(0xffffff), buf);
		ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight() - (ImGui::GetFrameHeight() - h) / 2));
	};
	ImGui::SeparatorText("State");
	auto *m = fi->brain && fi->brain->exec ? fi->brain->exec->current_method() : nullptr;
	auto *st = fi->brain && fi->brain->exec ? fi->brain->exec->current_state() : nullptr;
	row("State", st ? st->cm->signature : "-", kAccentText);
	row("Running", m ? m->cm->signature : "-", kTextDim);
	std::string anim = a.sequence >= 0 && fi->bank.sequences.size() > size_t(a.sequence) ? fi->bank.sequences[size_t(a.sequence)].name : "-";
	char buf[128];
	snprintf(buf, sizeof buf, "%s  t=%.2f%s", anim.c_str(), double(a.anim_time), a.prelude ? "  (prelude)" : "");
	row("Animation", buf);
	ImGui::SeparatorText("Stats");
	game::Brain *sb = stats_brain(&a);
	bar("Health", call_float(world, sb, "float NormalizedHealth()"), 0x2f8f4e);
	bar("Energy", call_float(world, sb, "float NormalizedCurrentEnergy()"), 0x4f9cff);
	bar("Stun", call_float(world, sb, "float NormalizedStun()"), 0xe8a33d);
	if (const game::Pilot *pl = game::pilot_of(&a)) {
		snprintf(buf, sizeof buf, "%s  P%.0f A%.0f E%.0f F%.0f", pl->name.c_str(), double(pl->stats.power), double(pl->stats.agility), double(pl->stats.endurance), double(pl->stats.focus));
		row("Pilot", buf);
	} else {
		row("Pilot", "none (idle)", kTextMuted);
	}
	ImGui::SeparatorText("Physics");
	auto t = a.world().t;
	snprintf(buf, sizeof buf, "%.1f, %.1f, %.1f", double(t[0]), double(t[1]), double(t[2]));
	row("Position", buf);
	snprintf(buf, sizeof buf, "%.2f, %.2f, %.2f", double(a.momentum[0]), double(a.momentum[1]), double(a.momentum[2]));
	row("Momentum", buf);
	row("Floor", a.on_floor ? "on the floor" : "in the air", a.on_floor ? kOk : kWarning);
	snprintf(buf, sizeof buf, "%.2f", double(a.gravity));
	row("Gravity", buf);
	ImGui::SeparatorText("Input buffer");
	if (a.user_controlled) {
		int shown = 0;
		ImGui::PushFont(fonts.mono);
		for (auto it = world.input_history.rbegin(); it != world.input_history.rend() && shown < 8; ++it, ++shown) {
			static const char *const dirs = "UDLR";
			std::string d, b;
			for (int k = 0; k < 4; ++k) {
				if (it->directions & (1 << k)) d += dirs[k];
			}
			for (int k = 0; k < 8; ++k) {
				if (it->buttons & (1 << k)) b += char('0' + k);
			}
			snprintf(buf, sizeof buf, "@%-6u dir %-4s btn %s", it->start, d.empty() ? "-" : d.c_str(), b.empty() ? "-" : b.c_str());
			ImGui::TextUnformatted(buf);
		}
		if (!shown) ui::dim_text("(no input yet)", kTextMuted);
		ImGui::PopFont();
	} else {
		snprintf(buf, sizeof buf, "%zu command(s), target %s", a.commands.list.size(), world.object(a.current_target) ? world.object(a.current_target)->name.c_str() : "-");
		row("AI", buf, kTextDim);
	}
	ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------------------------
// The level editor's play controls

std::vector<std::string> WorldEditor::play_files() {
	std::vector<std::string> stems = {settings_.robot, settings_.robot + "Model1", settings_.opponent, settings_.opponent + "Model1", "Pilots", "Session",
	                                  "ArenaDescriptions", "Fighter", "SharedObjects", "Pickups", "Effects", "Forces", "Controllers", "Creature", "Materials",
	                                  "Flyers", "Music"};
	if (arena_) stems.push_back(arena_->entry.stem);
	return stems;
}

void WorldEditor::start_play(bool simulate) {
	if (play_) return;
	if (!e_.base()) return;
	if (!arena_) open_arena_named("Desert");
	if (!arena_) return;
	PlaySession::Setup st;
	st.settings = settings_;
	st.data = e_.base()->dir();
	st.environment_description = arena_->entry.description;
	st.arena_stem = arena_->entry.stem;
	st.simulate = simulate;
	st.fixed_step = e_.sync_loads();
	ed::Project *p = e_.project();
	if (p) {
		fs::path dir = de::user_data_dir() / "editor" / "pie";
		std::error_code ec;
		fs::create_directories(dir, ec);
		st.addon = dir / p->path().filename();
		try {
			auto bytes = p->bytes();
			std::ofstream f(st.addon, std::ios::binary | std::ios::trunc);
			f.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
		} catch (const std::exception &ex) {
			e_.log(LogLine::Error, std::string("Cannot write the add-on for Play: ") + ex.what());
			return;
		}
	}
	if (!settings_.all_data) {
		const ed::BaseData *base = e_.base();
		std::set<std::string> keep;
		for (const std::string &s : play_files()) {
			if (!base->file(s)) continue;
			for (auto &f : ed::arena_files(*base, s, p ? &p->file() : nullptr)) keep.insert(f);
		}
		st.keep = keep;
	}
	simulate_ = simulate;
	play_ = std::make_unique<PlaySession>(std::move(st));
	play_->debug = debug_;
	if (simulate) play_->set_ejected(true);
	else focus_viewport_ = true;
	if (layout_ > 1 && active_pane_ != 0 && panes_[size_t(active_pane_)].cam.mode != Camera::Perspective) active_pane_ = 0;
	play_->eject_camera() = panes_[size_t(active_pane_)].cam;
	play_->eject_camera().mode = Camera::Perspective;
	e_.log(LogLine::Info, std::string(simulate ? "Simulate: " : "Play: ") + settings_.robot + " (" + settings_.pilot + ") vs " + settings_.opponent +
	                          (settings_.opponent_ai && !simulate ? " (" + settings_.opponent_pilot + ", AI)" : simulate ? " (AI)" : " (idle dummy)") + " in " +
	                          arena_->entry.name);
	play_menu_ = false;
}

void WorldEditor::stop_play() {
	if (!play_) return;
	play_.reset();
	e_.log(LogLine::Info, "Stopped play");
	e_.status("Ready");
}

void WorldEditor::eject(bool on) {
	if (play_) play_->set_ejected(on);
}

void WorldEditor::play_frame() {
	ImGuiIO &io = ImGui::GetIO();
	if (!play_) {
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		if (ImGui::IsKeyPressed(ImGuiKey_P, false) && io.KeyAlt) start_play(false);
		return;
	}
	if (!play_->ready()) {
		if (!play_->error().empty()) {
			e_.log(LogLine::Error, "Play failed: " + play_->error());
			play_.reset();
		}
		return;
	}
	if (play_->frame_number() == 0) e_.log(LogLine::Ok, std::string(simulate_ ? "Simulating" : "Playing") + " (" + std::to_string(int((ImGui::GetTime() - play_->started()) * 1000)) + " ms to start)");
	const bool focus = viewport_focused_ && !play_->ejected();
	viewport_focused_ = false;
	if (focus) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
	else io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) play_->set_ejected(!play_->ejected());
	if (focus && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		stop_play();
		return;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Pause, false)) play_->paused = !play_->paused;
	play_->debug = debug_;
	// Live data edits: an add-on pilot's attributes reach its fighter at once.
	if (ed::Project *p = e_.project(); p && p->revision() != live_revision_) {
		live_revision_ = p->revision();
		for (uint32_t pi : ed::addon_pilots(*p)) {
			try {
				ed::PilotData d = ed::read_pilot(*p, ed::pilot_objects(*p, pi));
				play_->apply_pilot_stats(p->file().objects[pi].info.name_text(), d.stats);
			} catch (const std::exception &) {
			}
		}
	}
	play_->frame(double(io.DeltaTime) * 1000.0, focus);
}

void WorldEditor::draw_toolbar_play() {
	const bool can = e_.base() != nullptr;
	const float y = ImGui::GetCursorPosY();
	if (!play_) {
		if (ui::toolbar_button(ICON_PLAY, "Play", ui::Tone::Play, can, "Play a match in the selected viewport (Alt+P)")) start_play(false);
		ImGui::SameLine();
		if (ui::toolbar_button(ICON_CIRCLE_PLAY, "Simulate", ui::Tone::Normal, can, "Run the arena with AI against AI, the camera free")) start_play(true);
	} else {
		const bool ready = play_->ready();
		if (ui::toolbar_button(play_->paused ? ICON_PLAY : ICON_PAUSE, play_->paused ? "Resume" : "Pause", ui::Tone::Normal, ready, "Pause / resume (Pause)")) play_->paused = !play_->paused;
		ImGui::SameLine();
		if (ui::toolbar_button(ICON_CHEVRON_RIGHT, "Step", ui::Tone::Normal, ready && play_->paused, "Step one game frame (20 ms)")) play_->step();
		ImGui::SameLine();
		const char *speeds[] = {"0.25x", "0.5x", "1x"};
		const float values[] = {0.25f, 0.5f, 1.0f};
		int cur = play_->speed == 0.25f ? 0 : play_->speed == 0.5f ? 1 : 2;
		if (ui::toolbar_button(nullptr, speeds[cur], ui::Tone::Normal, ready, "Game speed")) play_->speed = values[(cur + 1) % 3];
		ImGui::SameLine();
		if (ui::toolbar_button(ICON_EXTERNAL_LINK, play_->ejected() ? "Possess" : "Eject", ui::Tone::Normal, ready, "Free the camera and select while the match runs (F8)")) play_->set_ejected(!play_->ejected());
		ImGui::SameLine();
		if (ui::toolbar_button(ICON_SQUARE, "Stop", ui::Tone::Play, true, "End the match (Esc in the viewport)")) stop_play();
	}
	(void)y;
}

void WorldEditor::draw_play_menu() {
	if (robots_.empty() && e_.base()) {
		for (const ed::BaseObject *o : e_.base()->of_class("Description\\Creature\\Fighter")) {
			if (std::find(robots_.begin(), robots_.end(), o->name) == robots_.end() && e_.base()->file(o->name) && e_.base()->file(o->name + "Model1")) robots_.push_back(o->name);
		}
		std::sort(robots_.begin(), robots_.end());
		pilots_ = ed::base_pilot_names(*e_.base());
		for (const ed::BaseObject *o : e_.base()->of_class("SessionModifier\\StandardSessionMaster")) {
			if (e_.base()->files()[o->file].stem == "Session") masters_.push_back(o->name);
		}
	}
	std::vector<std::string> pilots = pilots_;
	if (e_.project()) {
		for (uint32_t pi : ed::addon_pilots(*e_.project())) pilots.insert(pilots.begin(), e_.project()->file().objects[pi].info.name_text());
	}
	auto combo = [](const char *id, std::string &v, const std::vector<std::string> &items) {
		ImGui::SetNextItemWidth(-1);
		if (ui::begin_combo(id, v.c_str())) {
			for (auto &it : items) {
				if (ImGui::Selectable(it.c_str(), it == v)) v = it;
			}
			ImGui::EndCombo();
		}
	};
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(8), ui::px(6)));
	ImGui::SeparatorText("Player");
	ui::form_label("Robot", ui::px(110));
	combo("##robot", settings_.robot, robots_);
	ui::form_label("Pilot", ui::px(110));
	combo("##pilot", settings_.pilot, pilots);
	ImGui::SeparatorText("Opponent");
	ui::form_label("Robot", ui::px(110));
	combo("##orobot", settings_.opponent, robots_);
	ui::form_label("Driven by", ui::px(110));
	int ai = settings_.opponent_ai ? 0 : 1;
	const char *drivers[] = {"AI pilot", "Idle dummy"};
	ImGui::SetNextItemWidth(-1);
	if (ui::combo("##driver", &ai, drivers, 2)) settings_.opponent_ai = ai == 0;
	if (settings_.opponent_ai) {
		ui::form_label("Pilot", ui::px(110));
		combo("##opilot", settings_.opponent_pilot, pilots);
		ui::form_label("Difficulty", ui::px(110));
		ImGui::SetNextItemWidth(-1);
		ui::combo("##diff", &settings_.difficulty, kDifficulties, 6);
	}
	ImGui::SeparatorText("Match");
	ui::form_label("Arena", ui::px(110));
	ui::dim_text(arena_ ? arena_->entry.name.c_str() : "The Desert (open an arena to choose)", kText);
	ui::form_label("Game type", ui::px(110));
	combo("##master", settings_.master, masters_);
	ui::form_label("Wins", ui::px(110));
	ImGui::SetNextItemWidth(ui::px(80));
	ImGui::InputInt("##wins", &settings_.wins, 0);
	settings_.wins = std::clamp(settings_.wins, 1, 50);
	ImGui::SeparatorText("Debug");
	ImGui::Checkbox("Hit and attack spheres", &debug_.hit_spheres);
	ImGui::Checkbox("Nav net and AI targets", &debug_.nav);
	ImGui::Checkbox("Full energy bar", &debug_.full_energy);
	ImGui::Checkbox("Mute", &settings_.mute);
	ImGui::Checkbox("Load all game data", &settings_.all_data);
	ImGui::PopStyleVar();
}

void WorldEditor::draw_inspector() {
	if (!ImGui::Begin(ICON_EYE "  Live Inspector###LiveInspector")) {
		ImGui::End();
		return;
	}
	if (!play_) {
		ui::centered_note(ICON_CIRCLE_PLAY, "Not playing", "Press Play to run a match in the viewport; the selected creature's state shows here.");
		ImGui::Dummy(ImVec2(0, ui::px(8)));
		draw_play_menu();
	} else {
		play_->draw_inspector();
		ImGui::Dummy(ImVec2(0, ui::px(6)));
		ImGui::SeparatorText("Debug");
		ImGui::Checkbox("Hit and attack spheres", &debug_.hit_spheres);
		ImGui::Checkbox("Nav net and AI targets", &debug_.nav);
		ImGui::Checkbox("Full energy bar", &debug_.full_energy);
	}
	ImGui::End();
}
