// The open arena's state, shared by the world editor's files.
#pragma once

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "de/scene.h"
#include "world.h"

// Coloured vertices (position, RGBA) drawn as lines or triangles.
struct FlatMesh {
	GLuint vao = 0, vbo = 0;
	GLsizei count = 0;
	std::vector<float> data;
	~FlatMesh();
	void clear();
	void vertex(const std::array<float, 3> &p, unsigned rgb, float alpha = 1, float shade = 1);
	void upload();
	void draw(const float *vp, GLenum mode, bool depth_test, bool blend) const;
};

// An arena actor's drawn triangles in world space, for clicks in the viewport.
struct PickMesh {
	uint32_t actor = 0;
	std::vector<std::array<float, 3>> tris;
	std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
};

bool invert4(const float *m, float *out);
// Binds the pane's render target (made or resized to w x h), returning what was bound.
void bind_pane(Pane &p, int w, int h, GLint *prev_fbo, GLint *prev_vp);

struct MarkerHit {
	int node;
	ImVec2 at;
	int pane;
};

struct WorldEditor::ArenaState {
	~ArenaState();
	ed::ArenaEntry entry;
	ed::ArenaDescriptionData description;
	double started = 0;
	std::thread loader;
	std::atomic<bool> loaded{false};
	bool built = false;
	std::string error;
	std::unique_ptr<de::World> world;
	// What the viewport draws: the arena's base file, or for an add-on arena the add-on itself
	// (in `world` under its stem) and that arena's environment in it.
	std::string render_stem;
	int render_env = -1;
	std::vector<PickMesh> picks;
	std::unique_ptr<render::ArenaRenderer> renderer;
	std::unique_ptr<render::GlowRenderer> glow;
	std::array<float, 3> ambient{0.3f, 0.3f, 0.3f};
	uint32_t light_serial = 0;
	std::unique_ptr<de::ObjectsFile> addon_file;
	uint64_t addon_revision = 0;
	ed::ArenaScene scene;
	std::vector<bool> position_static;
	std::vector<ed::Issue> issues;
	FlatMesh collision, collision_lines, outline;
	std::array<FlatMesh, 3> kind_lines;  // the lit view's wire: surfaces, forces and death volumes, hazards
	int outline_node = -2;
	int selected = -1;
	std::vector<MarkerHit> marker_hits;
	// A gizmo drag: the node's world transform while the mouse is down, written on release.
	bool dragging = false;
	int drag_node = -1, drag_axis = 0;
	de::Transform drag_world, drag_start;
	ImVec2 drag_mouse;
	std::string place_filter, outliner_filter;
	int max_players() const;
};
