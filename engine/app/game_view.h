// The game scene as the player sees it: the fighters (set up as a session's descriptions make
// them), the arena, run-time actors, particles, the glow layer and the HUD, through the fight
// camera. Shared by openbg-fight and the front end.
#pragma once

#include <functional>
#include <set>

#include <array>
#include <optional>
#include <map>
#include <string>
#include <vector>

#include "de/anim.h"
#include "game/runtime.h"
#include "glow.h"
#include "overlay.h"
#include "particles_gl.h"
#include "render.h"
#include "shadows.h"

namespace app {

struct Fighter {
	std::string name;
	game::GameObject *actor = nullptr;
	game::Brain *brain = nullptr;
	de::AnimBank bank;
	std::map<std::string, game::InputSequence> inputs;  // the fighter's own move sequences
	render::FighterRenderer renderer;
	bool renderer_loaded = false;
	std::string last_state;
	game::Brain *stats = nullptr;  // PFighterStats
};

// A robot from its file: a copy of the file's OMF2Fighter with its brain (and, with `with_stats`,
// PFighterStats under it). Fighters live as long as the program.
Fighter &setup_fighter(game::World &world, const std::string &name, bool user, bool with_stats = true);

// A Camera property's lens (DE_CCamera contents v3/v4: field of view in degrees, near, far), with
// the field of view set at run time (the observer's) when there is one.
struct CameraLens {
	float fov = 80, near = 10, far = 100000;
};
CameraLens camera_lens(const game::GameObject *actor);
// The value objects an actor's modifiers read, as set on that actor or one above it.
de::LiveFloat actor_values(const game::World &world, const game::GameObject *o);

class GameView {
public:
	explicit GameView(game::World &world);
	// Loads the renderers' GL resources (needs the GL context).
	void init_gl();
	// The session builds the fighters (World::make_fighter) and hands slot 0's to the player.
	void wire_session();
	// The arena whose static scene is drawn (its file stem); found from the scene when not set.
	// `environment`: the environment object in that file (-1: every one), for add-ons with several arenas.
	void load_arena(const std::string &stem, int environment = -1);
	int reflections_built_ = -1;  // the Reflections the arena and fighters were built with
	bool has_arena() const { return !arena.empty(); }
	// Draws into the bound viewport (dw x dh pixels). `fixed_step` (screenshot runs) snaps the
	// camera instead of trailing.
	void draw(int dw, int dh, int fr, bool fixed_step);
	// The player's Listener, facing as the fight camera does.
	de::Transform ear() const;

	game::World &world;
	std::vector<Fighter *> fighters;
	Fighter *player = nullptr;
	bool free_camera = false;
	// Follow the scene's environment as sessions come and go (the front end), rather than keep the
	// arena given once.
	bool follow_scene = false;
	const float fighter_radius = 60;  // camera framing
	float yaw = 0.6f, pitch = 0.18f, distance = fighter_radius * 5.0f;
	bool dragging = false;
	// Draw from this camera actor with no HUD, leaving the HUD's projection alone (a capture).
	game::GameObject *capture_camera = nullptr;
	// Replaces the game's camera (an editor's free view): fills the projection, view and eye for dw x dh.
	std::function<void(int dw, int dh, float *proj, float *view, float *eye)> camera_override;
	std::array<float, 16> drawn_vp{};  // the last draw's view-projection

	render::ArenaRenderer arena_r;
	render::OverlayRenderer overlay;
	render::ParticleRenderer particles_r;
	render::ShadowRenderer shadows_r;
	render::GlowRenderer glow_r;
	render::ActorMeshRenderer actors_r;

private:
	game::GameObject *scene_camera() const;
	Fighter *viewed_fighter() const;
	void load_renderers();
	std::string find_arena(int *environment = nullptr) const;
	bool shown(const game::GameObject *o) const;
	std::function<std::optional<render::ActorMeshRenderer::LayerImage>(const de::ObjectRef &, const de::ObjectRef &, const de::ObjectRef &)>
	images_of(const game::GameObject *o);

	std::string arena;
	int arena_environment = -1;
	std::array<float, 16> screen_vp{};
	float screen_wide_k = 1;  // 4:3 width over the screen width, below 1 on wide screens
	float cam_base = 0;  // camera heading, trailing the fighter's facing
	bool cam_init = false;
	bool owners_set = false;
	// Each drawn actor's inherited lighting owner (AssureLightable).
	std::map<const game::GameObject *, const game::GameObject *> inherited_owner_;
	const de::ObjectsFile *environment_tree_file_ = nullptr;
	std::vector<bool> environment_tree_;  // the arena file's objects under its environment
	// Where the bake places the arena file's objects, and its actors that are elsewhere (drawn live).
	std::vector<de::Transform> data_world_;
	std::vector<bool> data_world_done_;
	std::vector<int> data_parents_;
	std::set<const game::GameObject *> turned_;
	bool gl_ready_ = false;
};

}  // namespace app
