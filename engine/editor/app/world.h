// The level editor's world side: the arena in the viewports (the game's renderer, cameras, overlays,
// selection, gizmo), its Outliner and Details, arena variants, and play in editor (pie.cpp).
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "arena.h"
#include "gl_util.h"
#include "imgui.h"

class Editor;
struct ObjectRef;
class PlaySession;
namespace render {
class ArenaRenderer;
class GlowRenderer;
}

struct Camera {
	enum Mode { Perspective, Top, Front, Side } mode = Perspective;
	std::array<float, 3> pos{0, 700, 1300};
	float yaw = 0, pitch = 0.45f;            // perspective: yaw about +Y, pitch down
	std::array<float, 3> centre{0, 0, 0};    // orthographic: what the view is centred on
	float half_height = 1200;                // orthographic: half the view's height in world units
	float speed = 900;                       // fly speed, units a second
	std::array<float, 3> forward() const;
	void matrices(int w, int h, float *proj, float *view, float *eye) const;
};

// One viewport pane: its camera, shading and render target.
struct Pane {
	Camera cam;
	int shading = 0;  // 0 lit, 1 unlit, 2 collision
	GLuint fbo = 0, tex = 0, depth = 0;
	int tw = 0, th = 0;
	float proj[16] = {}, view[16] = {}, vp[16] = {}, eye[3] = {};
	ImVec2 p0, size;
	bool hovered = false;
};

struct ShowFlags {
	bool surfaces = true, forces = true, death = true, spawns = true, pickups = true, nav = true, hazards = true, lights = true,
	     cameras = true, affectors = false, labels = true, grid = true;
};

struct PlaySettings {
	std::string robot = "Jaguar", pilot = "Lucas", opponent = "Katana", opponent_pilot = "Elizabeth";
	bool opponent_ai = true;   // false: an idle dummy (no pilot)
	int difficulty = 2;        // Rookie .. Master
	std::string master = "LastManStandingMaster";
	int wins = 2;
	bool all_data = false;     // load every Data file instead of what the match reaches
	bool mute = false;
};

struct DebugFlags {
	bool hit_spheres = false, nav = false, full_energy = false;
};

class WorldEditor {
public:
	explicit WorldEditor(Editor &e);
	~WorldEditor();

	// Each frame, before the panels: finishes loads, runs the match, keyboard shortcuts.
	void frame();
	// An arena object opens in the viewport (true when `r` is one).
	bool open(const ObjectRef &r);
	void open_arena(const ed::ArenaEntry &a);
	void close_arena();
	bool has_arena() const;
	const ed::ArenaEntry *arena() const;
	// Inside the Viewport window; false when there is nothing to show (the shell's empty state).
	bool draw_viewport();
	bool draw_outliner();
	bool draw_details_header();
	bool draw_place();
	void draw_toolbar_play();
	void draw_inspector();
	void draw_arena_menu();
	void draw_play_menu();
	std::string play_mode_label() const;
	// Scripted start-up actions (Options), applied once the arena and the match are ready.
	void run_script();
	std::vector<ed::Issue> arena_issues();
	void copy_to_addon();
	bool playing() const { return play_ != nullptr; }

	// Scripted runs (screenshots).
	void open_arena_named(const std::string &name);
	void select_named(const std::string &name);
	void set_layout(int panes) { layout_ = panes; }
	void set_shading(int s) { panes_[0].shading = s; }
	void start_play(bool simulate);
	void stop_play();
	void eject(bool on);
	void frame_selection(int pane);
	void set_camera(int pane, const std::array<float, 3> &pos, float yaw, float pitch);

	struct ArenaState;
	ArenaState *state() { return arena_.get(); }

private:
	friend class PlaySession;
	void finish_load();
	void refresh_scene();
	void draw_pane(int i, ImVec2 p0, ImVec2 size);
	void render_pane(Pane &p);
	void pane_header(int i);
	void pane_input(int i);
	void draw_overlays(int i);
	bool gizmo(int i);
	int pick(int i, ImVec2 mouse);
	void select_node(int node);
	void commit_drag();
	int selected_node() const;
	ObjectRef node_ref(int node) const;
	void place(const char *what);
	void delete_selected();
	void play_frame();
	std::vector<std::string> play_files();

	Editor &e_;
	std::unique_ptr<ArenaState> arena_;
	std::unique_ptr<PlaySession> play_;
	std::array<Pane, 4> panes_;
	int layout_ = 1;       // 1, 2 or 4 panes
	int active_pane_ = 0;  // the selected viewport (Play runs in it)
	ShowFlags show_;
	int gizmo_mode_ = 0;   // 0 move, 1 rotate
	bool snap_ = true;
	float grid_ = 10;
	PlaySettings settings_;
	DebugFlags debug_;
	bool simulate_ = false;
	bool play_menu_ = false;
	std::string play_error_;
	std::vector<std::string> robots_, pilots_, masters_;
	bool viewport_focused_ = false;
	bool focus_viewport_ = false;
	uint64_t live_revision_ = 0;
	int script_step_ = 0;
	bool replayed_ = false;
	friend struct PlayAccess;
};
