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
  These still run on the CPU every frame:
  - fighter skinning: `skin_vertices` in `app/render.cpp` blends every vertex, then re-uploads it.
    It moves to a vertex shader with a bone matrix palette.
  - particle quads: `app/particles_gl.cpp` builds every billboard and stream on the CPU and streams it
    up. It moves to instanced or shader-expanded quads.
  - batch rebuilds for animated texture and colour values.
- **Performance:**
  - profile a whole tournament;
  - free destroyed objects instead of keeping them allocated (FINDINGS 460);
  - spatial indexing for affector and collision walks (`walk_affectors` rebuilds its maps on every
    test);
  - cache the static arena batches;
  - limit the script memory pool's growth.
