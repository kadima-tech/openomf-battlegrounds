// The front end's UI managers and page flow (FINDINGS 125, 126): Core's DE_CAppManager page
// state machine and DE_CUIManager pages, OMF's screens. A page is the GUI file's F.<folder>,
// whose VE.* actors are its buttons; each button is a hit rectangle placed over its actor's
// DisplayArea, and hover and clicks reach the actor's script as GUIEnter / GUILeave / GUISelect.
// What a click does is the screen's C++ (ProcessButtonClickEvent), here `UIManager::click`.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "de/scene.h"

namespace game {

class World;
struct GameObject;

namespace ui {

class AppManager;

// A DE_CButtonControl3d of type 0 (an undecorated hit rectangle with a label) placed by
// DE_CUIManager::PositionControl, with its DE_CUIManagerActorButtonRobot.
struct Control {
	std::string text;
	float left = 0, top = 0, right = 0, bottom = 0;  // fractions of the screen, y down
	GameObject *ve = nullptr;                         // the VE actor its events go to
	bool hovered = false;
	bool enabled = true;          // robot +0x74
	std::vector<int> keys;        // DE_CButtonClickOnKey: SDL keycodes that click it
	bool contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
};

// What a page draws besides its button labels, in screen fractions (y down). A text item is
// anchored at (x0, y0): its left edge, or its centre when centred, and its vertical middle.
struct DrawItem {
	enum Kind { Rect, Text, Image } kind = Rect;
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	float rgba[4] = {1, 1, 1, 1};
	std::string text;
	float size = 0;  // text height, screen heights
	bool centered = false;
	de::ObjectRef image;
};

class UIManager {
public:
	UIManager(std::string name, std::string folder) : name(std::move(name)), folder(std::move(folder)) {}
	virtual ~UIManager() = default;

	std::string name;
	std::string folder;  // +0xcc: the page is built from F.<folder>
	// DE_CUIManager::ObjInitContents (Core 0x701ea9a0) defaults.
	bool begun = false;            // +0x9c
	bool activated = false;        // +0xa0
	bool autodestroy = true;       // +0xa4
	bool uses_transition = true;   // +0xa8
	bool show_main_frame = true;   // +0xac
	bool show_backdrop = true;     // +0xb0
	bool autopause = true;         // +0xb4
	bool halt_loading = false;     // +0xb8
	bool input_off = true;         // +0xbc
	bool show_overlay = true;      // +0xc8
	bool auto_select = false;      // +0x94
	bool backed_out = false;       // +0x60
	bool persistent = false;       // the factory's persistent instance: never deleted

	std::vector<Control> controls;

	// Begin (vtable +0x148): false vetoes showing the page (or redirects it).
	virtual bool begin(AppManager &app);
	virtual void setup_controls(AppManager &) {}
	// The screen robot's ProcessButtonClickEvent.
	virtual void click(AppManager &, Control &) {}
	virtual void draw(AppManager &, std::vector<DrawItem> &) {}
	// Input the page takes before its buttons: `what` 0 move, 1 press, 2 release, 3 double click.
	virtual bool mouse(AppManager &, float, float, int) { return false; }
	virtual bool key(AppManager &, int) { return false; }
	virtual void wheel(AppManager &, int) {}
	virtual void text(AppManager &, const std::string &) {}
	virtual void deactivated(AppManager &) {}

	void activate(AppManager &app);
	void deactivate(AppManager &app);
	// UIManagerComplete (Core 0x701eb320).
	void complete(AppManager &app);

protected:
	// DE_CUIManagerRobot::NewButtonControl + PositionControl(marker).
	Control *button(AppManager &app, const std::string &text, const std::string &marker);

private:
	GameObject *root_ = nullptr;          // the custom actor under the UI parent
	std::vector<GameObject *> ve_actors_;  // SetupFolderControls' VE list
};
using UIPtr = std::shared_ptr<UIManager>;

// DE_CGenericDescriptionSelector (Core 0x701e46c0..0x701e5370; FINDINGS 127): a name-sorted list
// of the pool's listed descriptions, the highlighted one's large representation beside it, Next
// (Enter, double click) to choose and Back (Esc).
class DescriptionSelector : public UIManager {
public:
	explicit DescriptionSelector(std::string name) : UIManager(std::move(name), "ChooserListNextBack") {}

	std::string title = "Choose";  // +0x64
	std::string autoselect_key;     // +0x138
	std::string next_label = "Next";  // +0x6c; empty: no Next button (Help)
	std::vector<de::ObjectRef> pool;
	de::ObjectRef chosen;           // +0x128 (ChosenDescription)
	std::function<void(AppManager &, DescriptionSelector &)> on_complete;

	bool begin(AppManager &app) override;
	void setup_controls(AppManager &app) override;
	void click(AppManager &app, Control &c) override;
	void draw(AppManager &app, std::vector<DrawItem> &out) override;
	bool mouse(AppManager &app, float x, float y, int what) override;
	bool key(AppManager &app, int sym) override;
	void wheel(AppManager &app, int dy) override;

private:
	void choose(AppManager &app, const de::ObjectRef &d);
	void select(AppManager &app, int i);
	int visible_rows() const;

	std::vector<de::ObjectRef> entries_;
	int selected_ = -1;
	int scroll_ = 0;  // the first entry shown
	Control *list_ = nullptr, *window_ = nullptr, *next_ = nullptr, *back_ = nullptr;
};

class AppManager {
public:
	explicit AppManager(World &world);

	World &world;
	std::string transfer_agent = "OMFBGGUI";
	// The launcher's InitGUI choices (not readable; FINDINGS 125 inference).
	GameObject *main_frame = nullptr, *transition = nullptr, *backdrop = nullptr, *session_overlay = nullptr;

	UIPtr current, next, last;
	std::vector<UIPtr> stack;
	int transition_state = 0;  // +0x74
	bool page_finished = false;  // +0x78
	bool auto_select_mode = false;  // +0x7c
	bool quit_requested = false;  // g_QuiteOnNextUpdate

	// Sessions, as the app manager sees them.
	std::function<bool()> has_local_session;
	std::function<bool()> session_fully_sequenced;
	std::function<bool()> session_scene_rendering;

	// DE_CAppManager::IsUIManagerAutopaused (Core 0x7019b260): a page that autopauses is up or
	// stacked; the scene then passes no time (DE_CScene::PassTime).
	bool autopaused() const;
	// DE_CRulesDisplay: the screen for a session script's rules display.
	UIPtr rules_display(GameObject *rules);

	// The instances the launcher and screens use.
	UIPtr main_menu();
	UIPtr quit_menu();
	UIPtr help_menu();
	UIPtr options_menu();
	// The in-game menu's Esc impulse; true when it acted.
	bool in_game_menu_impulse();

	// <transfer agent>\<name>: a new copy (GetObjectByValueString / ConstructObject).
	GameObject *construct(const std::string &name);
	// DE_CActor::SetActive / SetInnactive for a scene root.
	void set_active(GameObject *actor, bool on);
	static bool is_active(const GameObject *actor);

	void set_next(const UIPtr &mgr, bool push);
	void set_last();
	void delete_stack(bool close_current);
	void close_stack();
	void begin_ui();
	void quit() { quit_requested = true; }
	// DE_COMF2Manager::MenuSelect (vtable +0x130).
	void menu_select(int id);
	// DE_CNotification::NewUserInformation: shown modally, Enter / Esc / click to dismiss.
	void notify(const std::string &title, const std::string &text);
	struct Notification {
		std::string title, text;
	};
	std::vector<Notification> notifications;
	// A screen this port does not have yet: says so instead of guessing.
	void not_ported(const std::string &what);

	// The game's registry (World::registry).
	std::string registry_string(const std::string &name) const;
	bool registry_bool(const std::string &name) const { return registry_string(name) == "1"; }
	void set_registry_string(const std::string &name, const std::string &v);
	void set_registry_bool(const std::string &name, bool v) { set_registry_string(name, v ? "1" : "0"); }

	// DE_CDescription::IsLocked(0): locked by default and not unlocked since (the registry's
	// DLock records; here the run's registry).
	bool is_locked(const de::ObjectRef &description) const;
	// The app's text measure (screen widths for a text `size` screen heights high).
	std::function<float(const std::string &, float)> text_width;
	// DE_COMF2Manager::ProcessEvent on the session chooser's Complete: start the chosen session.
	std::function<void(const de::ObjectRef &master_description, bool single_player)> launch_session;

	// One app update (DE_CAppManager::ObjUpdate before the sessions): UpdateUI.
	void update();

	// Input, in screen fractions (y down).
	void mouse_move(float x, float y);
	void mouse_button(bool down, int clicks = 1);
	void key(int sym, int scancode = 0);
	void wheel(int dy);
	void text(const std::string &t);
	int last_scancode = 0;  // the key event being handled
	// The keyboard actions (DE_COMF2Manager::ObjInitContents's impulses) and their bindings, kept as
	// registry values "Controls\<action>" (SDL scancodes); `key_name` names a scancode.
	struct Action {
		const char *name;
		int default_scancode;
	};
	static const std::vector<Action> &actions();
	int binding(size_t action) const;
	void set_binding(size_t action, int scancode);
	std::function<std::string(int)> key_name;
	// Apply the registry's settings to the game (volumes, renderer switches, AI difficulty).
	void apply_settings();

private:
	void transition_ui();
	bool transition_busy();
	void signal_transition(const char *handle);
	bool on_stack(const UIManager *m) const;
	void drop(const UIPtr &m);

	Control *pressed_ = nullptr;
	float mouse_x_ = -1, mouse_y_ = -1;
	bool first_ui_ = true;
	UIPtr main_menu_, quit_menu_, in_game_menu_, help_menu_, options_menu_;
};

}  // namespace ui
}  // namespace game
