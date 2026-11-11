// Play in editor: a match of the game in a viewport, in this process (docs/EDITOR_SPEC.md §3).
#pragma once

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include "world.h"

namespace game {
class World;
struct GameObject;
}
namespace app {
class GameView;
struct Fighter;
}
namespace audio {
class Mixer;
}

class PlaySession {
public:
	struct Setup {
		PlaySettings settings;
		std::filesystem::path data, addon;  // addon: empty for none
		std::set<std::string> keep;         // the Data files to load (empty: all)
		ed::Guid environment_description;   // the arena, by its Description\Environment
		std::string arena_stem;
		bool simulate = false;
		bool fixed_step = false;  // screenshot runs: one game frame per editor frame
	};
	explicit PlaySession(Setup setup);
	~PlaySession();

	// Finishes the start (GL, arena) once the world is built; false while it is still loading.
	bool ready();
	const std::string &error() const { return error_; }
	double started() const { return started_; }
	// One editor frame: `ms` of real time pass at the chosen speed unless paused; `input` routes
	// the keyboard to the player.
	void frame(double ms, bool input);
	void step();
	void run_ticks(int n);
	void render(Pane &p);
	void draw_overlays(Pane &p);
	bool pick_fighter(Pane &p, ImVec2 mouse);
	void draw_inspector();
	bool ejected() const { return ejected_; }
	void set_ejected(bool on);
	Camera &eject_camera() { return eject_cam_; }
	// Data edits the running match takes at once (a pilot's attributes).
	void apply_pilot_stats(const std::string &pilot, const std::array<float, 4> &stats);
	int frame_number() const { return frame_; }
	std::string summary() const;

	bool paused = false;
	float speed = 1;
	DebugFlags debug;

private:
	void setup_world();
	void spawn_direct();
	app::Fighter *inspected() const;

	Setup setup_;
	std::thread loader_;
	std::atomic<bool> loaded_{false};
	bool gl_ready_ = false;
	std::string error_;
	double started_ = 0;
	std::unique_ptr<game::World> world_;
	std::unique_ptr<app::GameView> view_;
	std::unique_ptr<audio::Mixer> mixer_;
	bool direct_ = false;  // no session (an idle dummy): spawned by the editor
	int frame_ = 0;
	double accum_ = 0;
	bool ejected_ = false;
	Camera eject_cam_;
	game::GameObject *inspect_ = nullptr;
	std::string saved_addons_;
	bool had_addons_ = false;
};
