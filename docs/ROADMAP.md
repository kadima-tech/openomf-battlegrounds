# Roadmap

Planned work beyond the faithful 2.123 port. The details are in the linked documents.

## Next
- Fix the bugs found in Windows playtesting, North American Open (tracked in issues and the session notes).
- Internet play: the lobby server, NAT traversal, relay and encryption ([NETWORK_PLAN.md](NETWORK_PLAN.md) phase 4).
- The content editor: HAR, arena and tournament editors, play in editor
  ([EDITOR_SPEC.md](EDITOR_SPEC.md)).

## Later
- **New game modes:** a story campaign with missions, choices, funds and upgrades, authored in the
  editor (EDITOR_SPEC §5.3), and MCP support (§5.4).
- **Renderer, optional improvements.** The faithful look stays the default, and every improvement has
  a switch.
  - **Lighting:** per-pixel lighting in place of the original's per-vertex D3D7-style lighting, better
    shadows, optional normal and specular use of the existing bump images, and HDR with tone mapping.
  - **Effects:** higher-resolution glow (the original's target is 256²), smoother particles (soft
    particles, sorting), and anti-aliasing options.
- **Moving CPU work to the GPU.** Drawing, lighting, glow and shadows already run in GLSL shaders.
  These still run on the CPU every frame, and none is a large cost yet (seeded North American Open,
  1024x768, Apple M-series; FINDINGS 580):
  - fighter skinning: `skin_vertices` in `app/render.cpp` blends every vertex, then re-uploads it:
    about 0.09 ms a fighter a frame, most of it the upload. It moves to a vertex shader with a bone
    matrix palette.
  - particle quads: `app/particles_gl.cpp` builds every billboard and stream on the CPU and streams it
    up: about 0.2 ms a frame. It moves to instanced or shader-expanded quads.
  - batch rebuilds for animated texture and colour values.
- **Performance** (FINDINGS 580-581; `OPENBG_DEBUG_MEMORY` prints `[perf]` and `[spike]` lines):
  - done: a whole tournament profiled; brains attach from a per-class template, references are read
    without decompressing contents, uniform locations are cached, and a class's code has one arena
    copy. A frame costs about 6 ms (scene tick 1.8, draw 1.8, GPU wait 2.0); frames over 20 ms in a
    40000-frame run went from 107 to 44.
  - the hitches left are loads: an arena takes 85-340 ms and each robot's renderer 50-160 ms when a
    match starts, and every texture override change (`texture_override_version`) rebuilds all
    run-time actor batches in one frame (20-65 ms, mostly `build_batches`);
  - free destroyed objects instead of keeping them allocated (FINDINGS 460): 29800 objects made,
    10300 live, after 40000 frames; the 3500 collision testers made are never freed;
  - spatial indexing for affector and collision walks (`walk_affectors` rebuilds its maps on every
    test): `affectors()` 0.23 ms and the scene walks 0.36 ms a frame;
  - cache the static arena batches (the arena's draw is 0.4 ms a frame);
  - limit the script memory pool's growth: 126 MB after the first match, 339 MB after 40000 frames;
    live brains (a 256 KB stack each) went from 457 to 640, the rest is not yet attributed.
