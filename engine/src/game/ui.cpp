#include "game/ui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "de/image.h"
#include "de/mesh.h"
#include "de/scene.h"
#include "game/descriptions.h"
#include "game/runtime.h"
#include "game/session.h"

namespace game::ui {

namespace {

constexpr int kKeyReturn = 13, kKeyEscape = 27, kKeyUp = (1 << 30) | 82, kKeyDown = (1 << 30) | 81;  // SDL keycodes

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

// A ControlCreator's layout (FINDINGS 127) flowed into a panel `width` wide from (x, y): text
// word-wrapped in its height, images at their size in 800 x 600 virtual pixels (scaled down to
// fit), groups stacked downwards or across. Returns the height used.
float flow(AppManager &app, const ControlLayout &l, float x, float y, float width, float bottom, std::vector<DrawItem> &out) {
	const float white[4] = {1, 1, 1, 1};
	if (l.kind == ControlLayout::Text) {
		float size = float(l.text_height) / 600.0f, line_h = size * 1.15f;
		std::vector<std::string> lines;
		size_t p = 0;
		const std::string &t = l.text;
		while (p <= t.size()) {
			size_t nl = t.find_first_of("\r\n", p);
			std::string para = t.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
			std::string line;
			size_t q = 0;
			while (q < para.size()) {
				size_t sp = para.find(' ', q);
				std::string word = para.substr(q, sp == std::string::npos ? std::string::npos : sp - q);
				std::string tryline = line.empty() ? word : line + " " + word;
				if (l.wrap && !line.empty() && app.text_width && app.text_width(tryline, size) > width) {
					lines.push_back(line);
					line = word;
				} else {
					line = tryline;
				}
				if (sp == std::string::npos) break;
				q = sp + 1;
			}
			lines.push_back(line);
			if (nl == std::string::npos) break;
			p = nl + (t.compare(nl, 2, "\r\n") == 0 ? 2 : 1);
		}
		float h = 0;
		for (auto &ln : lines) {
			if (y + h + line_h <= bottom && !ln.empty()) {
				DrawItem d;
				d.kind = DrawItem::Text;
				d.x0 = x, d.y0 = y + h + line_h / 2, d.size = size, d.text = ln;
				std::copy(white, white + 4, d.rgba);
				out.push_back(d);
			}
			h += line_h;
		}
		return h;
	}
	if (l.kind == ControlLayout::Image) {
		de::Image im;
		try {
			im = de::read_image_header(l.image.file->object_data(l.image.info()).contents);
		} catch (const std::exception &) {
			return 0;
		}
		float w = float(im.width) / 800.0f, h = float(im.height) / 600.0f;
		if (w > width && w > 0) h *= width / w, w = width;
		if (y + h <= bottom) {
			DrawItem d;
			d.kind = DrawItem::Image;
			d.x0 = x, d.y0 = y, d.x1 = x + w, d.y1 = y + h, d.image = l.image;
			out.push_back(d);
		}
		return h;
	}
	float used = 0, cx = x;
	for (auto &it : l.items) {
		if (l.across) {
			float h = flow(app, it, cx, y, width - (cx - x), bottom, out);
			used = std::max(used, h);
			if (it.kind == ControlLayout::Image) {
				try {
					cx += float(de::read_image_header(it.image.file->object_data(it.image.info()).contents).width) / 800.0f;
				} catch (const std::exception &) {
				}
			}
		} else {
			used += flow(app, it, x, y + used, width, bottom, out);
		}
	}
	return used;
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

bool DescriptionSelector::begin(AppManager &app) {
	UIManager::begin(app);
	chosen = {};
	// GetAvailableDescriptions (0x701e4790): listed, and not hidden by a lock; sorted by name.
	entries_.clear();
	for (const de::ObjectRef &d : pool) {
		DescriptionInfo info = read_description(app.world.data(), d);
		if (!info.listed || (app.is_locked(d) && !info.visible_when_locked)) continue;
		entries_.push_back(d);
	}
	std::stable_sort(entries_.begin(), entries_.end(), [](auto &a, auto &b) { return a.info().name < b.info().name; });
	selected_ = entries_.empty() ? -1 : 0;
	// Begin (0x701e4d40): a pool of one is chosen without showing; backing out passes through.
	if (pool.size() == 1) {
		choose(app, pool[0]);
		return false;
	}
	if (app.last && app.last->backed_out) {
		backed_out = true;
		app.set_last();
		return false;
	}
	return true;
}

void DescriptionSelector::setup_controls(AppManager &app) {
	list_ = button(app, "", "ListWindow");
	window_ = button(app, "", "Window");
	next_ = button(app, "Next", "NEXT");
	next_->keys = {kKeyReturn};
	back_ = button(app, "Back", "BACK");
	back_->keys = {kKeyEscape};
	select(app, selected_);
}

// The robot's ProcessSelectedObjects (0x701e5370): Next reads "Locked" for a locked one.
void DescriptionSelector::select(AppManager &app, int i) {
	if (i < 0 || i >= int(entries_.size())) return;
	selected_ = i;
	if (next_) next_->text = app.is_locked(entries_[size_t(i)]) ? "Locked" : "Next";
}

void DescriptionSelector::choose(AppManager &app, const de::ObjectRef &d) {
	chosen = d;
	complete(app);
	if (on_complete) on_complete(app, *this);
}

// ProcessButtonClickEvent (0x701e5130).
void DescriptionSelector::click(AppManager &app, Control &c) {
	if (&c == back_) {
		backed_out = true;
		app.set_last();
	} else if (&c == next_ && selected_ >= 0) {
		const de::ObjectRef &d = entries_[size_t(selected_)];
		if (app.is_locked(d)) app.notify("Selection Locked", d.info().name + " is currently locked and cannot be selected.");
		else choose(app, d);
	}
}

namespace {
constexpr float kRow = 20.0f / 600.0f, kListText = 16.0f / 600.0f;  // DE_CListControl3dEx entry height 0x14
}

void DescriptionSelector::draw(AppManager &app, std::vector<DrawItem> &out) {
	if (!list_ || !window_) return;
	const float white[4] = {1, 1, 1, 1}, hilite[4] = {0.55f, 0.75f, 0.9f, 0.35f};
	float pad = 0.01f;
	DrawItem t;
	t.kind = DrawItem::Text;
	t.x0 = (list_->left + list_->right) / 2, t.y0 = list_->top + pad + kRow / 2, t.size = kListText * 1.15f, t.centered = true, t.text = title;
	std::copy(white, white + 4, t.rgba);
	out.push_back(t);
	for (size_t i = 0; i < entries_.size(); ++i) {
		float y = list_->top + pad + kRow * float(i + 1.5f);
		if (y + kRow > list_->bottom) break;
		if (int(i) == selected_) {
			DrawItem r;
			r.x0 = list_->left + pad, r.x1 = list_->right - pad, r.y0 = y, r.y1 = y + kRow;
			std::copy(hilite, hilite + 4, r.rgba);
			out.push_back(r);
		}
		DrawItem e;
		e.kind = DrawItem::Text;
		e.x0 = list_->left + 2 * pad, e.y0 = y + kRow / 2, e.size = kListText, e.text = entries_[i].info().name;
		std::copy(white, white + 4, e.rgba);
		if (app.is_locked(entries_[i])) e.rgba[0] = e.rgba[1] = e.rgba[2] = 0.6f;
		out.push_back(e);
	}
	// NewLargeRepresentation (description vtable +0x120): the ControlCreator in the Window.
	if (selected_ >= 0) {
		DescriptionInfo info = read_description(app.world.data(), entries_[size_t(selected_)]);
		if (info.large && info.large.info().class_path == "ControlCreator") {
			flow(app, read_control_creator(app.world.data(), info.large), window_->left + 2 * pad, window_->top + 2 * pad,
			     window_->right - window_->left - 4 * pad, window_->bottom - pad, out);
		}
	}
}

bool DescriptionSelector::mouse(AppManager &app, float x, float y, int what) {
	if (!list_ || !list_->contains(x, y) || (what != 1 && what != 3)) return false;
	float pad = 0.01f;
	int row = int(std::floor((y - list_->top - pad) / kRow - 1.5f));
	if (row < 0 || row >= int(entries_.size())) return true;
	select(app, row);
	if (what == 3 && next_) click(app, *next_);
	return true;
}

bool DescriptionSelector::key(AppManager &app, int sym) {
	if (sym == kKeyUp) select(app, selected_ - 1);
	else if (sym == kKeyDown) select(app, selected_ + 1);
	else return false;
	return true;
}

bool AppManager::is_locked(const de::ObjectRef &d) const {
	return read_description(world.data(), d).locked_by_default && !registry_bool("Unlocked " + d.info().guid.str());
}

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

// DE_COMF2Manager::MenuSelect (OMF 0x350151d0).
void AppManager::menu_select(int id) {
	if (id >= 2 && id <= 5) {
		auto sel = std::make_shared<DescriptionSelector>("ChooseSesssionType");
		sel->title = "Choose Match";
		sel->autoselect_key = "Session Master";
		auto_select_mode = id == 4;
		de::ObjectRef cast;
		if (id == 2 || id == 3) cast = world.data().find("Description\\Cast", id == 3 ? "SinglePlayerCast" : "MultiPlayerCast");
		for (const auto &[stem, f] : world.data().files()) {
			for (uint32_t i = 0; i < f->objects().size(); ++i) {
				if (f->objects()[i].class_path != "Description\\SessionMaster") continue;
				de::ObjectRef d{f.get(), i};
				if (cast) {
					DescriptionInfo info = read_description(world.data(), d);
					bool member = std::any_of(info.casts.begin(), info.casts.end(), [&](auto &c) { return c.info().guid.str() == cast.info().guid.str(); });
					if (!member || !(info.visible_when_locked || !is_locked(d))) continue;
				}
				sel->pool.push_back(d);
			}
		}
		bool single = id == 3;
		sel->on_complete = [single](AppManager &app, DescriptionSelector &s) {
			if (app.launch_session) app.launch_session(s.chosen, single);
			else app.not_ported("Sessions");
		};
		set_next(sel, true);
		return;
	}
	if (id == 10) not_ported("Join Game");
}

void AppManager::notify(const std::string &title, const std::string &text) { notifications.push_back({title, text}); }

void AppManager::not_ported(const std::string &what) { notify(what, "This screen has not been ported yet."); }

// DE_CTopControl3d's control under the mouse (control events 0x25 / 0x26) and
// DE_CUIManagerActorButtonRobot::ProcessMouseEnter / Leave (Core 0x701ed540 / 0x701ed480).
void AppManager::mouse_move(float x, float y) {
	mouse_x_ = x, mouse_y_ = y;
	if (!current || !current->activated) return;
	if (notifications.empty()) current->mouse(*this, x, y, 0);
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
void AppManager::mouse_button(bool down, int clicks) {
	if (!notifications.empty()) {
		if (!down) notifications.erase(notifications.begin());
		return;
	}
	if (!current || !current->activated) return;
	{
		UIPtr page = current;
		if (page->mouse(*this, mouse_x_, mouse_y_, down ? (clicks >= 2 ? 3 : 1) : 2)) {
			pressed_ = nullptr;
			return;
		}
	}
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
	if (UIPtr page = current; page->key(*this, sym)) return;
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
