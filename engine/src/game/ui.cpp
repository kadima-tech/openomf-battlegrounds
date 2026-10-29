#include "game/ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>

#include "de/image.h"
#include "de/mesh.h"
#include "de/scene.h"
#include "game/descriptions.h"
#include "game/music.h"
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

	// DE_CMainMenuUIManager::Begin (OMF 0x3505a3c0).
	bool begin(AppManager &app) override {
		UIManager::begin(app);
		app.delete_stack(false);
		if (app.world.music) app.world.music->play_default();
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
		send(app.world, app.world.music_player_actor(), "MusicPlayerAssureDefaultMusic");
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
		} else if (&c == help_) {
			app.set_next(app.help_menu(), true);
		} else if (&c == options_) {
			app.set_next(app.options_menu(), true);
		} else if (&c == replays_) {
			app.not_ported(c.text);
		}
	}

private:
	int nags_ = 0;  // robot +0x78
	Control *options_ = nullptr, *replays_ = nullptr, *single_ = nullptr, *join_ = nullptr, *host_ = nullptr, *help_ = nullptr,
	        *quit_ = nullptr;
};

// DE_CRulesDisplay + robot (OMF ObjInitContents 0x35062ef0, SetupControls 0x35063140, robot
// 0x35063520, ProcessPreDraw 0x350643d0): the single-player loading screen. Under a sub-session
// owner it shows the owner's MatchCommentary, NextMatchDescription and NextMatchName (each falling
// back to the owner's master description's value of that name, then its name); in single player
// it is not authorized until a key is pressed once "Press any key to continue" shows.
class RulesDisplayPage : public UIManager {
public:
	explicit RulesDisplayPage(GameObject *rules) : UIManager("Rules", "SinglePlayerLoading"), rules_(rules) {
		autopause = false;      // +0xb4
		show_backdrop = false;  // +0xb0
	}

	bool begin(AppManager &app) override {
		UIManager::begin(app);
		World &w = app.world;
		auto &r = w.rules_displays[rules_];
		SessionState *s = session_of(w, r.session);
		SessionState *owner = s ? s->owner : nullptr;
		auto value = [&](const char *name) -> std::string {
			if (!owner) return {};
			if (auto *v = w.object_value(owner->obj, name); v && !v->text.empty()) return v->text;
			if (owner->master_description) {
				if (auto *v = w.object_value(owner->master_description, name); v && !v->text.empty()) return v->text;
			}
			return {};
		};
		if (owner) {
			commentary_ = value("MatchCommentary");
			next_ = value("NextMatchDescription");
			title_ = value("NextMatchName");
			if (title_.empty() && owner->master_description) title_ = owner->master_description->name;
			if (owner->single_player) r.authorized = false;
		} else if (s && s->master_description) {
			if (auto *v = w.object_value(s->master_description, "Description")) commentary_ = v->text;
			if (commentary_.empty()) commentary_ = "Game Description";
			title_ = s->master_description->name;
		}
		return true;
	}
	void setup_controls(AppManager &app) override {
		commentary_box_ = button(app, "", "CommentaryText");
		next_box_ = button(app, "", "NextTextBox");
		name_box_ = button(app, "", "MatchName");
		progress_box_ = button(app, "", "Progress");
	}
	void draw(AppManager &app, std::vector<DrawItem> &out) override {
		auto &r = app.world.rules_displays[rules_];
		auto text = [&](Control *c, const std::string &t, int height) {
			if (!c || t.empty()) return;
			ControlLayout l;
			l.kind = ControlLayout::Text;
			l.text = t;
			l.text_height = height;
			flow(app, l, c->left + 0.005f, c->top + 0.005f, c->right - c->left - 0.01f, c->bottom, out);
		};
		text(name_box_, title_, 22);
		text(commentary_box_, commentary_, 15);
		text(next_box_, next_, 15);
		if (progress_box_) {
			DrawItem d;
			d.kind = DrawItem::Text;
			d.x0 = (progress_box_->left + progress_box_->right) / 2, d.y0 = (progress_box_->top + progress_box_->bottom) / 2;
			d.size = 16.0f / 600.0f, d.centered = true;
			if (r.prompted && !r.authorized) {
				d.text = "Press any key to continue";
				d.rgba[3] = 0.55f + 0.45f * std::sin(float(app.world.frame()) * 0.15f);
			} else {
				d.text = r.task;
			}
			out.push_back(d);
		}
	}
	bool key(AppManager &app, int) override {
		auto &r = app.world.rules_displays[rules_];
		if (r.prompted && !r.authorized) r.authorized = true;
		return true;
	}
	void deactivated(AppManager &app) override { app.world.rules_displays[rules_].shown = false; }

private:
	GameObject *rules_;
	std::string title_, commentary_, next_;
	Control *commentary_box_ = nullptr, *next_box_ = nullptr, *name_box_ = nullptr, *progress_box_ = nullptr;
};

// DE_CInGameMenuUIManager (OMF ObjInitContents 0x35059460, SetupControls 0x35059490, click
// 0x35059a00): over the match, no door transition and no main frame; its folder is chosen by the
// session (InGameMenuSinglePlayer / Server / Client / DedicatedServer).
class InGameMenu : public UIManager {
public:
	InGameMenu() : UIManager("InGameMenu", "InGameMenuSinglePlayer") {
		uses_transition = false;
		show_main_frame = false;
		persistent = true;
	}
	void setup_controls(AppManager &app) override {
		main_menu_ = button(app, "Main Menu", "MainMenu");
		change_ = folder != "InGameMenuSinglePlayer" ? button(app, "Change Fighter", "ChangeFighter") : nullptr;
		add_bot_ = folder == "InGameMenuServer" || folder == "InGameMenuDedicatedServer" ? button(app, "Add AI Bot", "AddBot") : nullptr;
		resume_ = button(app, "Resume", "Resume");
		resume_->keys = {kKeyEscape};
		quit_ = button(app, "Quit", "Quit");
		options_ = button(app, "Options", "Options");
		help_ = button(app, "Help", "Help");
	}
	void click(AppManager &app, Control &c) override {
		if (&c == add_bot_) {
			app.menu_select(7);
		} else if (&c == main_menu_) {
			destroy_all_sessions(app.world);
			app.set_next(app.main_menu(), true);
		} else if (&c == change_) {
			app.not_ported(c.text);
		} else if (&c == quit_) {
			app.set_next(app.quit_menu(), true);
		} else if (&c == resume_) {
			app.delete_stack(true);
		} else if (&c == help_) {
			app.set_next(app.help_menu(), true);
		} else if (&c == options_) {
			app.set_next(app.options_menu(), true);
		}
	}

private:
	Control *main_menu_ = nullptr, *change_ = nullptr, *add_bot_ = nullptr, *resume_ = nullptr, *quit_ = nullptr, *options_ = nullptr,
	        *help_ = nullptr;
};


// DE_COptionsUIManager (OMF ObjInitContents 0x3505b060, SetupControls 0x3505d0e0, click
// 0x3505e170) and its panels (DE_CAudioPanelRobot, DE_CVideoPanelRobot 0x3505eef0,
// DE_CGameplayPanelRobot 0x3505b170, DE_CNetworkPanelRobot 0x3505bc60, DE_CControlsPanelRobot
// 0x3505f6e0): the panel buttons, Back (Esc) and the panel in WINDOW; the previous panel's
// settings are applied when another opens or the page is left. Each setting keeps the registry
// value the original uses.
class OptionsMenu : public UIManager {
public:
	OptionsMenu() : UIManager("Options", "Options") { halt_loading = true; }

	void setup_controls(AppManager &app) override {
		controls_ = button(app, "Control", "Controls");
		video_ = button(app, "Video", "Video");
		audio_ = button(app, "Audio", "Audio");
		game_ = button(app, "Gameplay", "Game");
		network_ = button(app, "Network", "Network");
		back_ = button(app, "Back", "Back");
		back_->keys = {kKeyEscape};
		window_ = button(app, "", "WINDOW");
		open(app, audio_);
	}
	void click(AppManager &app, Control &c) override {
		apply(app);
		if (&c == back_) {
			app.set_last();
			return;
		}
		if (&c != window_) open(app, &c);
	}
	void deactivated(AppManager &app) override {
		apply(app);
		settings_.clear();
		panel_ = nullptr;
	}
	void draw(AppManager &app, std::vector<DrawItem> &out) override {
		if (!window_) return;
		const float pad = 0.02f;
		float x0 = window_->left + pad, x1 = window_->right - pad, y = window_->top + pad;
		auto text = [&](const std::string &t, float x, float yy, float size, bool centered = false, float a = 1) {
			DrawItem d;
			d.kind = DrawItem::Text;
			d.x0 = x, d.y0 = yy, d.size = size, d.text = t, d.centered = centered;
			d.rgba[3] = a;
			out.push_back(d);
		};
		text(title_, (x0 + x1) / 2, y + 0.02f, 0.042f, true);  // DE_CUISizes PanelTitle
		y += 0.06f;
		rows_.clear();
		row_h_ = std::min(kRowH, (window_->bottom - pad - y) / float(std::max<size_t>(1, settings_.size())));
		const float kRowH = row_h_;
		for (size_t i = 0; i < settings_.size(); ++i) {
			Setting &st = settings_[i];
			float mid = x0 + (x1 - x0) * 0.5f;
			if (y + kRowH > window_->bottom - pad) break;
			rows_.push_back({y, y + kRowH});
			if (int(i) == focus_) {
				DrawItem r;
				r.x0 = x0, r.x1 = x1, r.y0 = y, r.y1 = y + kRowH;
				const float hi[4] = {0.55f, 0.75f, 0.9f, 0.2f};
				std::copy(hi, hi + 4, r.rgba);
				out.push_back(r);
			}
			text(st.label, x0 + 0.01f, y + kRowH / 2, 0.033f);  // PanelItem
			std::string v;
			switch (st.kind) {
			case Setting::Slider: {
				DrawItem back, fill;
				back.x0 = fill.x0 = mid, back.x1 = x1 - 0.01f, back.y0 = fill.y0 = y + kRowH * 0.35f, back.y1 = fill.y1 = y + kRowH * 0.65f;
				const float bc[4] = {0.2f, 0.3f, 0.3f, 0.6f}, fc[4] = {0.5f, 1.0f, 1.0f, 1.0f};
				std::copy(bc, bc + 4, back.rgba);
				std::copy(fc, fc + 4, fill.rgba);
				fill.x1 = mid + (back.x1 - mid) * std::clamp(st.value, 0.0f, 1.0f);
				out.push_back(back);
				out.push_back(fill);
				break;
			}
			case Setting::Choice: v = st.choices.empty() ? std::string() : st.choices[size_t(st.index)]; break;
			case Setting::Check: v = st.value != 0 ? "On" : "Off"; break;
			case Setting::Text: v = st.text + (int(i) == focus_ && editing_ ? "_" : ""); break;
			case Setting::Key: v = (int(i) == focus_ && assigning_) ? "Press a key..." : (app.key_name ? app.key_name(app.binding(size_t(st.action))) : ""); break;
			case Setting::Button: v = "[ " + st.text + " ]"; break;
			}
			if (!v.empty()) text(v, (mid + x1) / 2, y + kRowH / 2, 0.033f, true);
			y += kRowH;
		}
		if (!note_.empty() && y + 0.05f < window_->bottom) {
			ControlLayout l;
			l.kind = ControlLayout::Text;
			l.text = note_;
			l.text_height = 15;
			flow(app, l, x0, y + 0.02f, x1 - x0, window_->bottom - pad, out);
		}
	}
	bool mouse(AppManager &app, float x, float y, int what) override {
		if (!window_ || !window_->contains(x, y) || (what != 1 && what != 3)) return false;
		float x0 = window_->left + 0.02f, x1 = window_->right - 0.02f, mid = x0 + (x1 - x0) * 0.5f;
		for (size_t i = 0; i < rows_.size() && i < settings_.size(); ++i) {
			if (y < rows_[i].first || y >= rows_[i].second) continue;
			Setting &st = settings_[i];
			focus_ = int(i);
			editing_ = st.kind == Setting::Text;
			assigning_ = false;
			switch (st.kind) {
			case Setting::Slider:
				if (x >= mid) st.value = std::clamp((x - mid) / (x1 - 0.01f - mid), 0.0f, 1.0f);
				break;
			case Setting::Choice:
				if (!st.choices.empty()) st.index = (st.index + (x < (mid + x1) / 2 ? int(st.choices.size()) - 1 : 1)) % int(st.choices.size());
				break;
			case Setting::Check: st.value = st.value != 0 ? 0 : 1; break;
			case Setting::Key: assigning_ = true; break;
			case Setting::Button:
				if (st.on_click) st.on_click(app);
				break;
			default: break;
			}
			changed(app);
			return true;
		}
		return true;
	}
	bool key(AppManager &app, int sym) override {
		if (focus_ < 0 || size_t(focus_) >= settings_.size()) return false;
		Setting &st = settings_[size_t(focus_)];
		if (assigning_ && st.kind == Setting::Key) {
			if (sym != kKeyEscape && app.last_scancode) app.set_binding(size_t(st.action), app.last_scancode);
			assigning_ = false;
			return true;
		}
		if (editing_ && st.kind == Setting::Text) {
			if (sym == 8 && !st.text.empty()) st.text.pop_back();  // backspace
			else if (sym == kKeyReturn || sym == kKeyEscape) editing_ = false;
			return true;
		}
		return false;
	}
	void text(AppManager &, const std::string &t) override {
		if (!editing_ || focus_ < 0 || size_t(focus_) >= settings_.size()) return;
		Setting &st = settings_[size_t(focus_)];
		if (st.kind == Setting::Text && st.text.size() < 32) st.text += t;
	}

private:
	static constexpr float kRowH = 0.055f;
	float row_h_ = kRowH;
	struct Setting {
		enum Kind { Slider, Choice, Check, Text, Key, Button } kind = Check;
		std::string label, reg;  // registry value name
		float value = 0;
		std::vector<std::string> choices;
		int index = 0;
		std::string text;
		int action = -1;
		std::function<void(AppManager &)> on_click;
	};

	static float reg_float(AppManager &app, const std::string &name, float def) {
		std::string v = app.registry_string(name);
		return v.empty() ? def : float(atof(v.c_str()));
	}
	void open(AppManager &app, Control *c) {
		panel_ = c;
		settings_.clear();
		focus_ = -1;
		editing_ = assigning_ = false;
		note_.clear();
		auto slider = [&](const char *label, const char *reg, float def) {
			Setting s;
			s.kind = Setting::Slider, s.label = label, s.reg = reg, s.value = reg_float(app, reg, def);
			settings_.push_back(s);
		};
		auto check = [&](const char *label, const char *reg, bool def) {
			Setting s;
			s.kind = Setting::Check, s.label = label, s.reg = reg;
			std::string v = app.registry_string(reg);
			s.value = v.empty() ? (def ? 1.0f : 0.0f) : (v == "1" ? 1.0f : 0.0f);
			settings_.push_back(s);
		};
		auto choice = [&](const char *label, const char *reg, std::vector<std::string> choices, int def) {
			Setting s;
			s.kind = Setting::Choice, s.label = label, s.reg = reg, s.choices = std::move(choices);
			std::string v = app.registry_string(reg);
			s.index = std::clamp(v.empty() ? def : atoi(v.c_str()), 0, int(s.choices.size()) - 1);
			settings_.push_back(s);
		};
		auto textf = [&](const char *label, const char *reg) {
			Setting s;
			s.kind = Setting::Text, s.label = label, s.reg = reg, s.text = app.registry_string(reg);
			settings_.push_back(s);
		};
		if (c == audio_) {
			title_ = "Audio Settings";
			slider("Music Volume", "Audio\\Music Volume", 0.6f);
			slider("Sound Volume", "Audio\\SFX Volume", 1.0f);
		} else if (c == game_) {
			title_ = "Game Settings";
			textf("Player Name", "Network\\Player Name");
			check("Record Session", "Gamepley\\Record Session", false);
			// DE_COMF2Manager::SetupAIDifficultyList.
			choice("AI Difficulty", "AI Difficulty", {"Rookie", "Amateur", "Veteran", "Champion", "World Champion", "Master"}, 0);
			choice("Auto Populate With AIs", "Auto Populate With AIs", {"Disabled", "2 Players", "3 Players", "4 Players", "5 Players", "6 Players", "7 Players", "8 Players"}, 0);
		} else if (c == video_) {
			title_ = "Video Options";
			check("Lights", "Preferences\\Renderer\\Lighting Enabled", true);
			check("Reflections", "Preferences\\Renderer\\Reflections Enabled", true);
			check("Glow", "Preferences\\Renderer\\Glow Enabled", true);
			check("Bump Mapping", "Preferences\\Renderer\\Bump Mapping Enabled", true);
			check("High Detail Geometry", "Preferences\\Renderer\\High Quality Geometry Enabled", true);
			choice("Shadows", "Preferences\\Renderer\\Shadow Type", {"None", "Low Quality", "Medium Quality", "High Quality"}, 3);
			choice("Resolutions", "Preferences\\Renderer\\Resolution", {"640 x 480", "800 x 600", "1024 x 768", "1280 x 960", "1600 x 1200"}, 2);
			check("Full Screen", "Preferences\\Renderer\\Full Screen", false);
			note_ = "Resolution and bit depth changes will take effect next time you start the game.";
		} else if (c == network_) {
			title_ = "Network Settings";
			check("LAN Only", "Network\\LAN Only", false);
			textf("Server Name", "Network\\Server Name");
			textf("Server Password", "Network\\Server Password");
			textf("Remote Admin Password", "Network\\Remote Admin Password");
			textf("Listener Port", "Winsock\\Listener Port");
			textf("Max Clients", "Network\\Max Clients");
		} else if (c == controls_) {
			title_ = "Control Settings";
			for (size_t i = 0; i < AppManager::actions().size(); ++i) {
				Setting s;
				s.kind = Setting::Key, s.label = AppManager::actions()[i].name, s.action = int(i);
				settings_.push_back(s);
			}
			Setting d;
			d.kind = Setting::Button, d.label = "", d.text = "Set Defaults";
			d.on_click = [](AppManager &a) {
				for (size_t i = 0; i < AppManager::actions().size(); ++i) a.set_binding(i, AppManager::actions()[i].default_scancode);
			};
			settings_.push_back(d);
		}
	}
	// Sliders take effect at once (DE_CAudioPanelRobot::OnUpdateOption).
	void changed(AppManager &app) {
		if (panel_ == audio_) apply(app);
	}
	// OnApplyOptions.
	void apply(AppManager &app) {
		for (Setting &st : settings_) {
			if (st.reg.empty()) continue;
			switch (st.kind) {
			case Setting::Slider: app.set_registry_string(st.reg, std::to_string(st.value)); break;
			case Setting::Check: app.set_registry_bool(st.reg, st.value != 0); break;
			case Setting::Choice: app.set_registry_string(st.reg, std::to_string(st.index)); break;
			case Setting::Text: app.set_registry_string(st.reg, st.text); break;
			default: break;
			}
		}
		app.apply_settings();
	}

	Control *controls_ = nullptr, *video_ = nullptr, *audio_ = nullptr, *game_ = nullptr, *network_ = nullptr, *back_ = nullptr, *window_ = nullptr;
	Control *panel_ = nullptr;
	std::string title_, note_;
	std::vector<Setting> settings_;
	std::vector<std::pair<float, float>> rows_;
	int focus_ = -1;
	bool editing_ = false, assigning_ = false;
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

namespace {
constexpr float kRow = 20.0f / 600.0f, kListText = 16.0f / 600.0f;  // DE_CListControl3dEx entry height 0x14
}

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
	scroll_ = 0;
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
	if (!next_label.empty()) {
		next_ = button(app, next_label, "NEXT");
		next_->keys = {kKeyReturn};
	}
	back_ = button(app, "Back", "BACK");
	back_->keys = {kKeyEscape};
	select(app, selected_);
}

// The robot's ProcessSelectedObjects (0x701e5370): Next reads "Locked" for a locked one.
int DescriptionSelector::visible_rows() const {
	if (!list_) return 0;
	return std::max(1, int((list_->bottom - list_->top - 0.01f) / kRow - 1.5f));
}

// The list control (DE_CListControl3dEx) scrolls to keep the selection in view.
void DescriptionSelector::select(AppManager &app, int i) {
	if (i < 0 || i >= int(entries_.size())) return;
	selected_ = i;
	int rows = visible_rows();
	if (rows > 0) {
		if (i < scroll_) scroll_ = i;
		if (i >= scroll_ + rows) scroll_ = i - rows + 1;
	}
	if (next_) next_->text = app.is_locked(entries_[size_t(i)]) ? "Locked" : next_label;
	// ProcessSelectedObjects (0x350574b0): the fighter's models, the flagged one by default.
	if (choose_fighter) {
		models_ = fighter_models(app.world.data(), entries_[size_t(i)]);
		model_ = 0;
		for (size_t k = 0; k < models_.size(); ++k) {
			if (read_description(app.world.data(), models_[k]).flag_d8) model_ = int(k);
		}
	}
}

void DescriptionSelector::choose(AppManager &app, const de::ObjectRef &d) {
	chosen = d;
	chosen_model = choose_fighter && model_ >= 0 && size_t(model_) < models_.size() ? models_[size_t(model_)] : de::ObjectRef{};
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


void DescriptionSelector::draw(AppManager &app, std::vector<DrawItem> &out) {
	if (!list_ || !window_) return;
	const float white[4] = {1, 1, 1, 1}, hilite[4] = {0.55f, 0.75f, 0.9f, 0.35f};
	float pad = 0.01f;
	DrawItem t;
	t.kind = DrawItem::Text;
	t.x0 = (list_->left + list_->right) / 2, t.y0 = list_->top + pad + kRow / 2, t.size = kListText * 1.15f, t.centered = true, t.text = title;
	std::copy(white, white + 4, t.rgba);
	out.push_back(t);
	for (size_t i = size_t(std::max(0, scroll_)); i < entries_.size(); ++i) {
		float y = list_->top + pad + kRow * (float(i) - float(scroll_) + 1.5f);
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
	// The Model strip along the Window's bottom (SetupControls 0x35056720).
	if (choose_fighter && !models_.empty()) {
		float y = window_->bottom - 0.05f;
		DrawItem l;
		l.kind = DrawItem::Text;
		l.x0 = window_->left + 0.03f, l.y0 = y, l.size = kListText, l.text = "Model";
		out.push_back(l);
		DrawItem m = l;
		m.x0 = (window_->left + window_->right) / 2, m.centered = true, m.text = models_[size_t(model_)].info().name;
		const float c[4] = {0.55f, 0.75f, 0.9f, 1};
		std::copy(c, c + 4, m.rgba);
		out.push_back(m);
	}
	// NewLargeRepresentation (description vtable +0x120): the ControlCreator in the Window, or a
	// pilot's panel (DE_CPilotDescription::NewLargeRepresentation, OMF 0x3504f930): its name, its
	// photo framed on the left, its stats as bars on the right.
	if (selected_ >= 0 && entries_[size_t(selected_)].info().class_path == "Description\\Pilot") {
		const de::ObjectRef &d = entries_[size_t(selected_)];
		float wx0 = window_->left + 2 * pad, wx1 = window_->right - 2 * pad, wy0 = window_->top + 2 * pad, wy1 = window_->bottom - 2 * pad;
		float w = wx1 - wx0, h = wy1 - wy0;
		DrawItem n;
		n.kind = DrawItem::Text;
		n.x0 = wx0 + 0.01f * w, n.y0 = wy0 + 0.04f * h, n.size = 20.0f / 600.0f, n.text = d.info().name;
		out.push_back(n);
		if (de::ObjectRef photo = pilot_photo(app.world.data(), d)) {
			const float frame[4] = {0.55f, 0.75f, 0.9f, 0.8f};
			DrawItem f;
			f.x0 = wx0 + 0.01f * w - 0.003f, f.x1 = wx0 + 0.58f * w + 0.003f, f.y0 = wy0 + 0.1f * h - 0.004f, f.y1 = wy0 + 0.65f * h + 0.004f;
			std::copy(frame, frame + 4, f.rgba);
			out.push_back(f);
			DrawItem im;
			im.kind = DrawItem::Image;
			im.x0 = wx0 + 0.01f * w, im.x1 = wx0 + 0.58f * w, im.y0 = wy0 + 0.1f * h, im.y1 = wy0 + 0.65f * h, im.image = photo;
			out.push_back(im);
		}
		auto res = passive_references(app.world.data(), d);
		std::optional<Pilot> pl = res.empty() ? std::nullopt : read_pilot(*res[0].file, res[0].info(), 0);
		if (pl) {
			// A vertical bar of four rows from 0.6 across, down to 0.73 (SetupVerticalBar), each a
			// DE_CPilotStatsRobot (OMF 0x350510b0): the stat's icon (OMFHelp's images) on the left
			// quarter, its name (10 virtual pixels) over the top half of the rest, and below it a
			// down frame (0.2, 0.3, 0.3) holding a (0.2, 0.5, 0.5) well filled (0.5, 1, 1) by
			// stat / 100.
			const char *names[4] = {"Power", "Agility", "Endurance", "Focus"};
			const char *icons[4] = {"Strength", "Agility", "Endurance", "Focus"};
			float vals[4] = {pl->stats.power, pl->stats.agility, pl->stats.endurance, pl->stats.focus};
			const de::ObjectsFile *help = app.world.data().file("OMFHelp");
			float x0 = wx0 + 0.6f * w, x1 = wx1, top = wy0 + 0.1f * h, row = (0.73f - 0.1f) * h / 4;
			for (int k = 0; k < 4; ++k) {
				float y = top + row * float(k), xi = x0 + 0.25f * (x1 - x0);
				if (help) {
					for (uint32_t i = 0; i < help->objects().size(); ++i) {
						const de::ObjectInfo &o = help->objects()[i];
						if (o.name != icons[k] || o.class_path != "Image\\Standard") continue;
						float side = std::min(xi - x0, row) * 0.9f;
						DrawItem im;
						im.kind = DrawItem::Image;
						im.x0 = x0 + (xi - x0 - side) / 2, im.x1 = im.x0 + side;
						im.y0 = y + (row - side) / 2, im.y1 = im.y0 + side, im.image = {help, i};
						out.push_back(im);
						break;
					}
				}
				DrawItem t;
				t.kind = DrawItem::Text;
				t.x0 = xi + 0.01f * (x1 - xi), t.y0 = y + row * 0.25f, t.size = 10.0f / 600.0f * 1.6f, t.text = names[k];
				out.push_back(t);
				DrawItem frame, well, fill;
				frame.x0 = xi, frame.x1 = x1, frame.y0 = y + row * 0.5f, frame.y1 = y + row * 0.95f;
				well = frame;
				well.x0 += 0.003f, well.x1 -= 0.003f, well.y0 += 0.004f, well.y1 -= 0.004f;
				fill = well;
				const float fc[4] = {0.2f, 0.3f, 0.3f, 1}, wc[4] = {0.2f, 0.5f, 0.5f, 0.5f}, bc[4] = {0.5f, 1.0f, 1.0f, 1.0f};
				std::copy(fc, fc + 4, frame.rgba);
				std::copy(wc, wc + 4, well.rgba);
				std::copy(bc, bc + 4, fill.rgba);
				fill.x1 = fill.x0 + (well.x1 - well.x0) * std::clamp(vals[k] / 100.0f, 0.0f, 1.0f);
				out.push_back(frame);
				out.push_back(well);
				out.push_back(fill);
			}
		}
	} else if (selected_ >= 0) {
		DescriptionInfo info = read_description(app.world.data(), entries_[size_t(selected_)]);
		if (info.large && info.large.info().class_path == "ControlCreator") {
			flow(app, read_control_creator(app.world.data(), info.large), window_->left + 2 * pad, window_->top + 2 * pad,
			     window_->right - window_->left - 4 * pad, window_->bottom - pad, out);
		}
	}
}

bool DescriptionSelector::mouse(AppManager &app, float x, float y, int what) {
	// The Model choice: a click steps to the next model.
	if (choose_fighter && window_ && what == 1 && !models_.empty() && window_->contains(x, y) && y > window_->bottom - 0.08f) {
		model_ = (model_ + 1) % int(models_.size());
		return true;
	}
	if (!list_ || !list_->contains(x, y) || (what != 1 && what != 3)) return false;
	float pad = 0.01f;
	int row = int(std::floor((y - list_->top - pad) / kRow - 1.5f)) + scroll_;
	if (row < 0 || row >= int(entries_.size())) return true;
	select(app, row);
	if (what == 3 && next_) click(app, *next_);
	return true;
}

void DescriptionSelector::wheel(AppManager &, int dy) {
	int rows = visible_rows();
	scroll_ = std::clamp(scroll_ - dy, 0, std::max(0, int(entries_.size()) - rows));
}

bool DescriptionSelector::key(AppManager &app, int sym) {
	if (sym == kKeyUp) select(app, selected_ - 1);
	else if (sym == kKeyDown) select(app, selected_ + 1);
	else return false;
	return true;
}

bool AppManager::is_locked(const de::ObjectRef &d) const {
	return description_locked(world, d);
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
	deactivated(app);
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

bool AppManager::autopaused() const {
	if (stack.empty() && !current) return false;
	if (current && current->autopause) return true;
	return std::any_of(stack.begin(), stack.end(), [](const UIPtr &m) { return m->autopause; });
}

UIPtr AppManager::rules_display(GameObject *rules) { return std::make_shared<RulesDisplayPage>(rules); }

// DE_CInGameMenuUIManagerFactory::ProcessEvent (OMF 0x35059150), the "In Game Menu" impulse (Esc):
// with any page up, an in-game menu that is showing closes; otherwise, over a sequenced local
// session, the menu opens in the session's folder.
bool AppManager::in_game_menu_impulse() {
	if (current || next) {
		if (in_game_menu_ && in_game_menu_->activated && current == in_game_menu_) {
			set_next(nullptr, false);
			return true;
		}
		return false;
	}
	if (world.sessions.empty()) return false;
	SessionState *s = world.sessions.back().get();
	while (s->owner) s = s->owner;
	if (!in_game_menu_) in_game_menu_ = std::make_shared<InGameMenu>();
	in_game_menu_->folder = s->single_player ? "InGameMenuSinglePlayer" : "InGameMenuServer";
	delete_stack(true);
	set_next(in_game_menu_, true);
	return true;
}

// DE_CHelpUIManager (OMF ObjInitContents 0x350587e0, GetAvailableDescriptions 0x350589b0): a
// description selector over the master descriptions in COREMeshes\HelpEntry, on ChooserListBack
// with no Next.
UIPtr AppManager::help_menu() {
	if (!help_menu_) {
		auto sel = std::make_shared<DescriptionSelector>("Help");
		sel->folder = "ChooserListBack";
		sel->next_label = "";
		de::ObjectRef cast = world.data().find("Description\\Cast", "HelpEntry");
		for (const auto &[stem, f] : world.data().files()) {
			for (uint32_t i = 0; i < f->objects().size(); ++i) {
				if (f->objects()[i].class_path.rfind("Description", 0) != 0) continue;
				de::ObjectRef d{f.get(), i};
				for (auto &c : read_description(world.data(), d).casts) {
					if (cast && c.info().guid.str() == cast.info().guid.str()) sel->pool.push_back(d);
				}
			}
		}
		help_menu_ = sel;
	}
	return help_menu_;
}

UIPtr AppManager::options_menu() {
	if (!options_menu_) options_menu_ = std::make_shared<OptionsMenu>();
	return options_menu_;
}

const std::vector<AppManager::Action> &AppManager::actions() {
	// SDL scancodes: arrows, A S Z X, Space, Left Shift, C.
	static const std::vector<Action> a = {
		{"Move Forward", 82}, {"Move Backward", 81}, {"Turn Left", 80}, {"Turn Right", 79}, {"Left Punch", 4}, {"Right Punch", 22},
		{"Left Kick", 29},    {"Right Kick", 27},    {"Jump", 44},      {"Evade", 225},     {"Duck", 6},
	};
	return a;
}

int AppManager::binding(size_t i) const {
	std::string v = registry_string(std::string("Controls\\") + actions()[i].name);
	return v.empty() ? actions()[i].default_scancode : atoi(v.c_str());
}

void AppManager::set_binding(size_t i, int scancode) { set_registry_string(std::string("Controls\\") + actions()[i].name, std::to_string(scancode)); }

void AppManager::apply_settings() {
	auto f = [&](const char *name, float def) {
		std::string v = registry_string(name);
		return v.empty() ? def : float(atof(v.c_str()));
	};
	auto b = [&](const char *name, bool def) {
		std::string v = registry_string(name);
		return v.empty() ? def : v == "1";
	};
	world.music_volume = f("Audio\\Music Volume", 0.6f);
	world.sfx_volume = f("Audio\\SFX Volume", 1.0f);
	world.glow_enabled = b("Preferences\\Renderer\\Glow Enabled", true);
	world.lighting_enabled = b("Preferences\\Renderer\\Lighting Enabled", true);
	// DE_COMF2Manager::Activate: registry "AI Difficulty" (default 0, Rookie).
	world.ai_difficulty = int(f("AI Difficulty", 0));
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
	// The standard session overlay (+0x9c) tints a rendering session's scene under a page that
	// wants it (+0xc8).
	bool under_session = session_scene_rendering && session_scene_rendering();
	set_active(session_overlay, current && under_session && current->show_overlay);
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

void AppManager::text(const std::string &t) {
	if (notifications.empty() && current && current->activated) current->text(*this, t);
}

void AppManager::key(int sym, int scancode) {
	last_scancode = scancode;
	if (!notifications.empty()) {
		if (sym == kKeyReturn || sym == kKeyEscape) notifications.erase(notifications.begin());
		return;
	}
	if (sym == kKeyEscape && in_game_menu_impulse()) return;
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

void AppManager::wheel(int dy) {
	if (notifications.empty() && current && current->activated) current->wheel(*this, dy);
}

std::string AppManager::registry_string(const std::string &name) const { return world.registry_string(name); }

void AppManager::set_registry_string(const std::string &name, const std::string &v) { world.registry[name] = v; }

}  // namespace game::ui
