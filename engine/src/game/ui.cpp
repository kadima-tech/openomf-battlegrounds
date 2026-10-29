#include "game/ui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "de/mesh.h"
#include "de/scene.h"
#include "game/runtime.h"
#include "game/session.h"

namespace game::ui {

namespace {

constexpr int kKeyReturn = 13, kKeyEscape = 27;  // as SDL's keycodes

std::string upper(std::string s) {
	for (char &c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
	return s;
}

void send(World &w, GameObject *to, const char *handle) {
	if (to) w.send_event(to, new_event(w, handle));
}

GameObject *child_named(GameObject *o, const std::string &name) {
	for (GameObject *ch : o->children) {
		if (ch->name == name) return ch;
	}
	return nullptr;
}

// DE_CUIManager::SetupControlArea (Core 0x701ec270): the first mesh's bounding box at the actor's
// position, in the UI parent's frame (the GUI root, at the origin): x 0..1 across the screen and
// y 0..0.75 up it.
bool display_rect(World &w, GameObject *area, Control &c) {
	if (!area->file) return false;
	auto meshes = de::actor_meshes(w.data(), {area->file, area->index});
	if (meshes.empty()) return false;
	de::Mesh m = de::read_mesh(*meshes[0].file, meshes[0].info());
	if (m.vertices.empty()) return false;
	std::array<float, 3> lo = m.vertices[0], hi = m.vertices[0];
	for (auto &v : m.vertices) {
		for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v[k]), hi[k] = std::max(hi[k], v[k]);
	}
	auto t = area->world().t;
	float px = t[0] + lo[0], py = t[1] + lo[1], sx = hi[0] - lo[0], sy = hi[1] - lo[1];
	c.left = px;
	c.right = px + sx;
	c.top = 1 - (py + sy) * 4 / 3;
	c.bottom = 1 - py * 4 / 3;
	return true;
}

class MainMenu : public UIManager {
public:
	MainMenu() : UIManager("MainMenu", "MainMenu") { show_main_frame = false; }

	bool begin(AppManager &app) override {
		UIManager::begin(app);
		app.delete_stack(false);
		if (auto_select) {
			// The remembered choice: "Singleplayer" reopens Single Player, anything else Host.
			app.menu_select(app.registry_string("MainMenu") == "Singleplayer" ? 3 : 2);
			return false;
		}
		return true;
	}
	void setup_controls(AppManager &app) override {
		nags_ = 0;
		options_ = button(app, "Options", "OPTIONS");
		replays_ = button(app, "Replays", "REPLAYS");
		single_ = button(app, "Single Player", "SINGLEPLAYER");
		join_ = button(app, "Join Game", "MULTIPLAYERJOIN");
		host_ = button(app, "Host Game", "MULTIPLAYERCREATE");
		help_ = button(app, "Help", "HELP");
		quit_ = button(app, "Quit", "QUIT");
	}
	// DE_CMainMenuUIManagerRobot::ProcessButtonClickEvent (OMF 0x3505a8f0).
	void click(AppManager &app, Control &c) override {
		app.delete_stack(false);
		if (&c == join_) {
			if (!app.registry_bool("SinglePlayerPlayed") && nags_ < 2) {
				if (++nags_ == 1) {
					app.notify("Potential Humiliation", "I strongly recommend Practice or Training from the Single Player section before joining a multiplayer game.");
				} else {
					app.notify("Masochist Detected",
					           "Just want to make sure I was clear, the average online player will pound you into a fine paste (people may laugh at you).");
				}
				return;
			}
			if (nags_ == 2) {
				app.notify("Legal Disclaimer",
				           "Diversions Entertainment and it's subsidiaries will not be held responsible for your entirely shattered ego.\n\nI'll stop nagging you now, enjoy your whomping.");
			}
			app.set_registry_bool("SinglePlayerPlayed", true);
			app.menu_select(10);
			app.set_registry_string("MainMenu", "MultiplayerJoin");
		} else if (&c == host_) {
			app.menu_select(2);
			app.set_registry_string("MainMenu", "MultiplayerJoin");
		} else if (&c == single_) {
			app.menu_select(3);
			app.set_registry_string("MainMenu", "Singleplayer");
		} else if (&c == quit_) {
			app.set_next(app.quit_menu(), true);
		} else if (&c == help_ || &c == options_ || &c == replays_) {
			app.not_ported(c.text);
		}
	}

private:
	int nags_ = 0;  // robot +0x78
	Control *options_ = nullptr, *replays_ = nullptr, *single_ = nullptr, *join_ = nullptr, *host_ = nullptr, *help_ = nullptr,
	        *quit_ = nullptr;
};

// DE_CQuitMenuUIManager (OMF 0x350611d0, 0x35061210, 0x35061610).
class QuitMenu : public UIManager {
public:
	QuitMenu() : UIManager("QuitMenu", "Quit") {}
	void setup_controls(AppManager &app) override {
		yes_ = button(app, "Yes", "YES");
		no_ = button(app, "No", "NO");
	}
	void click(AppManager &app, Control &c) override {
		if (&c == no_) app.set_last();
		else if (&c == yes_) app.quit();
	}

private:
	Control *yes_ = nullptr, *no_ = nullptr;
};

}  // namespace

bool UIManager::begin(AppManager &) {
	begun = true;
	return true;
}

// DE_CUIManager::Activate (Core 0x701eb8a0) and SetupFolderControls (0x701ebc70).
void UIManager::activate(AppManager &app) {
	World &w = app.world;
	root_ = w.create_child(nullptr, name + " custom actor", "Actor");
	root_->gui = true;
	if (GameObject *f = app.construct("F." + folder)) {
		auto children = f->children;
		for (GameObject *ch : children) {
			if (!ch->is_actor || upper(ch->name).rfind("VE.", 0) != 0) continue;
			auto &v = f->children;
			v.erase(std::remove(v.begin(), v.end(), ch), v.end());
			ch->parent = root_;
			root_->children.push_back(ch);
		}
		w.destroy(f);
	}
	w.enter_scene(root_);
	// A passive reference among them has been replaced by what it created.
	ve_actors_ = root_->children;
	controls.clear();
	controls.reserve(32);  // Control pointers stay valid while the page is up
	setup_controls(app);
	for (GameObject *ve : ve_actors_) send(w, ve, "GUILeave");
	activated = true;
}

// DE_CUIManager::Deactivate (Core 0x701eb6e0).
void UIManager::deactivate(AppManager &app) {
	if (root_) app.world.clean_later(root_);
	root_ = nullptr;
	ve_actors_.clear();
	controls.clear();
	activated = false;
}

void UIManager::complete(AppManager &app) {
	begun = false;
	app.page_finished = true;
}

Control *UIManager::button(AppManager &app, const std::string &text, const std::string &marker) {
	std::string key = upper(marker.rfind("VE.", 0) == 0 ? marker : "VE." + marker);
	Control &c = controls.emplace_back();
	c.text = text;
	for (GameObject *ve : ve_actors_) {
		if (upper(ve->name) != key) continue;
		// DE_CUIManager::SetupDisplayArea (Core 0x701ec3f0).
		GameObject *area = child_named(ve, "DisplayArea");
		if (!area) {
			if (auto *v = app.world.object_value(ve, "P.DisplayArea")) area = child_named(ve, v->text);
		}
		if (!area || !display_rect(app.world, area, c)) {
			fprintf(stderr, "SetupDisplayArea: no display area under %s\n", ve->name.c_str());
		}
		c.ve = ve;
		return &c;
	}
	fprintf(stderr, "PositionControl() was unnable to find %s so was unnable to correctly position the control\n", key.c_str());
	return &c;
}

AppManager::AppManager(World &w) : world(w) {}

UIPtr AppManager::main_menu() {
	if (!main_menu_) {
		main_menu_ = std::make_shared<MainMenu>();
		main_menu_->persistent = true;
	}
	return main_menu_;
}

UIPtr AppManager::quit_menu() {
	if (!quit_menu_) quit_menu_ = std::make_shared<QuitMenu>();
	return quit_menu_;
}

GameObject *AppManager::construct(const std::string &name) {
	const de::ObjectsFile *f = world.data().file(transfer_agent);
	if (!f) return nullptr;
	auto parents = f->parents();
	for (uint32_t i = 0; i < f->objects().size(); ++i) {
		if (parents[i] >= 0 || f->objects()[i].name != name) continue;
		if (f->objects()[i].class_path == "Value\\String") {
			if (auto *v = world.object_value(world.global_object({f, i}), name)) return construct(v->text);
		}
		GameObject *tpl = world.global_object({f, i});
		return tpl ? world.copy_tree(*tpl) : nullptr;
	}
	fprintf(stderr, "%s has no %s\n", transfer_agent.c_str(), name.c_str());
	return nullptr;
}

void AppManager::set_active(GameObject *a, bool on) {
	if (!a || is_active(a) == on) return;
	a->removed = !on;
	if (on) world.enter_scene(a);
}

bool AppManager::is_active(const GameObject *a) { return a && !a->removed; }

bool AppManager::on_stack(const UIManager *m) const {
	return std::any_of(stack.begin(), stack.end(), [m](const UIPtr &p) { return p.get() == m; });
}

// TransitionUIManager's teardown. Instances live as long as someone holds them, so the original's
// delete-unless-persistent-or-stacked needs nothing more.
void AppManager::drop(const UIPtr &m) {
	if (m && m->activated) m->deactivate(*this);
}

// DE_CAppManager::SetNextUIManager (Core 0x7019af60).
void AppManager::set_next(const UIPtr &mgr, bool push) {
	page_finished = false;
	if (mgr && auto_select_mode) mgr->auto_select = true;
	if (first_ui_) {
		first_ui_ = false;
		if (mgr && !mgr->auto_select) {
			next = mgr;
			mgr->begin(*this);
			return;
		}
	}
	if (mgr && (mgr == current || mgr == next)) return;
	last = current;
	if (current) {
		if (push && !on_stack(current.get())) stack.push_back(current);
		if (!mgr) page_finished = true;
	}
	next = mgr;
}

// SetLastUIManager (Core 0x7019b100): Back.
void AppManager::set_last() {
	if (stack.empty()) return;
	UIPtr top = stack.back();
	set_next(top, false);
	stack.pop_back();
}

// DeleteUIManagerStack (Core 0x7019ba00).
void AppManager::delete_stack(bool close_current) {
	stack.clear();
	if (close_current) set_next(nullptr, false);
}

// CloseUIManagerStack (Core 0x7019bd00).
void AppManager::close_stack() {
	if (current) {
		while (!stack.empty()) {
			next = stack.back();
			stack.pop_back();
		}
	}
	set_next(nullptr, false);
}

// BeginUI (Core 0x7019b9f0): the transition actor plays its end, the opening reveal.
void AppManager::begin_ui() {
	set_active(transition, true);
	signal_transition("GUIPageChangeTransitionCompleted");
}

void AppManager::signal_transition(const char *handle) {
	set_active(transition, true);
	send(world, transition, handle);
}

// PageTransitionBusy (Core 0x7019b8f0): the transition actor answers in INT1.
bool AppManager::transition_busy() {
	if (!transition) return false;
	uint32_t e = new_event(world, "GUIPageChangeTransitionBusy");
	world.memory().set<int32_t>(e + 4, 0);
	world.send_event(transition, e);
	return world.memory().get<int32_t>(e + 4) != 0;
}

// TransitionUIManager (Core 0x7019ab30).
void AppManager::transition_ui() {
	bool wanted = (current && current->uses_transition && page_finished) || (next && next->uses_transition);
	if (transition_state == 0) {
		transition_state = 1;
		if (wanted) signal_transition("GUIStartPageChangeTransition");
		return;
	}
	if (transition_state == 1) {
		if (transition_busy()) return;
		if (next) {
			if (!next->begun) return;
		} else if (!(session_fully_sequenced && session_fully_sequenced())) {
			return;
		}
		if (current) {
			drop(current);
			current = nullptr;
		}
		if (next && next->begun) {
			current = next;
			next = nullptr;
		}
		if (!current) {
			set_active(main_frame, false);
			set_active(backdrop, false);
		} else {
			if (!current->activated && current->begun && !page_finished) current->activate(*this);
			set_active(main_frame, current->show_main_frame);
			bool under_session = session_scene_rendering && session_scene_rendering();
			if (current->show_backdrop && !under_session) set_active(backdrop, true);
			if (!current->show_backdrop) set_active(backdrop, false);
		}
		transition_state = 2;
		signal_transition("GUIPageChangeTransitionCompleted");
		return;
	}
	if (transition_busy()) return;
	transition_state = 0;
	if (!current) page_finished = false;
	set_active(transition, false);
}

// UpdateUI (Core 0x7019b360), then DE_COMF2Manager::ObjUpdate's way back to the main menu.
void AppManager::update() {
	if (!current && !next && transition_state == 0) {
		set_active(transition, false);
	} else if ((current && current->uses_transition && page_finished) || (next && next->uses_transition)) {
		set_active(transition, true);
	}
	if (transition_state != 0 || (current && page_finished) || next || page_finished) transition_ui();
	if (!next || next->begun) {
		if (current) {
			if (!current->activated && current->begun && !page_finished) current->activate(*this);
		} else if (!next && !stack.empty()) {
			UIPtr top = stack.back();
			set_next(top, false);
			stack.erase(std::find(stack.begin(), stack.end(), top));
		}
	} else {
		UIPtr n = next;
		if (!n->begin(*this)) {
			n->begun = false;
			if (next == n) next = nullptr;
		}
	}
	if (!(has_local_session && has_local_session()) && !current && !next && transition_state == 0) set_next(main_menu(), true);
}

void AppManager::menu_select(int id) {
	switch (id) {
	case 3: not_ported("Single Player"); break;
	case 2: not_ported("Host Game"); break;
	case 10: not_ported("Join Game"); break;
	default: break;
	}
}

void AppManager::notify(const std::string &title, const std::string &text) { notifications.push_back({title, text}); }

void AppManager::not_ported(const std::string &what) { notify(what, "This screen has not been ported yet."); }

// DE_CTopControl3d's control under the mouse (control events 0x25 / 0x26) and
// DE_CUIManagerActorButtonRobot::ProcessMouseEnter / Leave (Core 0x701ed540 / 0x701ed480).
void AppManager::mouse_move(float x, float y) {
	mouse_x_ = x, mouse_y_ = y;
	if (!current || !current->activated) return;
	for (Control &c : current->controls) {
		bool over = notifications.empty() && c.contains(x, y);
		if (over && !c.hovered && c.enabled) {
			c.hovered = true;
			send(world, c.ve, "GUIEnter");
		} else if (!over && c.hovered) {
			c.hovered = false;
			send(world, c.ve, "GUILeave");
		}
	}
}

// DE_CButtonRobot: a button clicks when released over itself; the actor robot sends GUISelect
// (ProcessButtonClick 0x701ed640) and the page's robot acts on it.
void AppManager::mouse_button(bool down) {
	if (!notifications.empty()) {
		if (!down) notifications.erase(notifications.begin());
		return;
	}
	if (!current || !current->activated) return;
	Control *hit = nullptr;
	for (Control &c : current->controls) {
		if (c.contains(mouse_x_, mouse_y_)) hit = &c;
	}
	if (down) {
		pressed_ = hit;
		return;
	}
	if (hit && hit == pressed_ && hit->enabled) {
		UIPtr page = current;
		send(world, hit->ve, "GUISelect");
		page->click(*this, *hit);
	}
	pressed_ = nullptr;
}

void AppManager::key(int sym) {
	if (!notifications.empty()) {
		if (sym == kKeyReturn || sym == kKeyEscape) notifications.erase(notifications.begin());
		return;
	}
	if (!current || !current->activated) return;
	for (Control &c : current->controls) {
		if (std::find(c.keys.begin(), c.keys.end(), sym) == c.keys.end() || !c.enabled) continue;
		UIPtr page = current;
		send(world, c.ve, "GUISelect");
		page->click(*this, c);
		return;
	}
}

std::string AppManager::registry_string(const std::string &name) const {
	auto it = registry.find(name);
	return it == registry.end() ? std::string() : it->second;
}

}  // namespace game::ui
