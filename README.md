# openomf-battlegrounds

A native macOS reimplementation of the "DE" engine behind *One Must Fall: Battlegrounds* (2003), in the style of OpenOMF and ScummVM. It runs the original game's data: the robots, arenas, scripts, sound and music are all the game's own; the engine around them is new.

This repository holds only code and notes we wrote. **No game data is included**: you need your own copy of the game (the Windows CD or ISO).

Status: fights run end to end (Last Man Standing sessions, every robot and arena, the computer opponent's AI, HUD, sound and music). There is no menu front end yet; robots, pilots and arenas are picked on the command line. `PLACEHOLDERS.md` lists everything that is still a stand-in, approximation or missing.

## Setting up

You need macOS (Apple silicon or Intel), CMake 3.20+, a C++20 compiler (Xcode's), and from Homebrew:

```sh
brew install cmake sdl2 unshield
```

`zlib` and OpenGL come with macOS. The Indeo video decoder, [openindeo](https://github.com/kadima-tech/openindeo), is fetched by CMake (or used from a checkout next to this one, `../openindeo`).

**1. The game data.** Put your ISO in `source/` and unpack its InstallShield cabinets:

```sh
mkdir -p source/iso
cp /path/to/Battlegrounds.iso source/          # from your CD, or the ISO you own
bsdtar -xf source/Battlegrounds.iso -C source/iso Setup
unshield -d source/extracted x source/iso/Setup/data1.cab
```

You should now have `source/extracted/App_Executables/Data` (the `.DEObjects` game files) and `.../Engine` (the engine modules, read for their embedded core data). `source/` is ignored by git.

**2. Build.**

```sh
cmake -S engine -B engine/build
cmake --build engine/build -j
ctest --test-dir engine/build        # parses every data file; -LE full skips the 20-second full pass
```

**3. Play.**

```sh
engine/build/openbg-fight source/extracted/App_Executables/Data Jaguar --opponent Katana --arena Desert
```

Controls: arrows walk and turn, A/S left/right punch, Z/X left/right kick, Space jump, Left Shift or C duck/evade, drag to orbit. Robots: Jaguar, Katana, Gargoyle, Chronos, Mantis, Pyros, Warlord, Force. Arenas: Desert, BladePit, Canyon, City, FirePit, Iceberg, PowerPlant, SpaceStation, Volcano. `--watch` lets the computer fight itself; the options are described below.

## What is in this repository

- `engine/`: the native reimplementation (C++20, CMake), described below. `engine/third_party/dmusic` is GothicKit's DirectMusic library (MIT), vendored, with its synthesizer replaced by a C port of Microsoft's DirectMusic software synthesizer sample (MIT).
- `re/FINDINGS.md`: the reverse-engineering ledger. What each part of the original engine does, with the module addresses it was read from. `re/opcodes.md` documents the script VM's bytecode.
- `re/scripts/` (Ghidra scripts: export, symbol fixes, cross-references) and `re/proto/` (the first Python prototypes of the data formats). The Ghidra scripts expect `GHIDRA_INSTALL_DIR`. They regenerate a local `re/dump/` of decompiled modules from your own copy of the game; those dumps are never committed.
- `PLACEHOLDERS.md`: every stand-in, fixed answer and missing piece.

## The engine

- `engine/`: the native reimplementation (C++20, CMake). It currently contains:
- `src/de`: the data layer. All shipped `.DEObjects` files parse (65 files, 28,115 objects), including Behavior script source.
- `src/vm`: the script VM. It reads the compiled VMC2 bytecode (249 classes, 10,582 methods) and runs it natively; shipped fighter-stat methods execute correctly.
- `src/de/scene.cpp`: actor hierarchy, transforms and materials, with cross-file references resolved through a `World` of all files.
- `app/viewer.cpp` (`openbg-viewer`): native SDL2/OpenGL fighter viewer with pilot recolouring and skeletal animation:
  `engine/build/openbg-viewer --data source/extracted/App_Executables/Data --fighter Jaguar --anim LeftMediumPunch`
  (Left/Right cycles through the fighter's moves; `--list` prints them; Space pauses.)
- `app/arena.cpp` (`openbg-arena`): native arena fly-through:
  `engine/build/openbg-arena source/extracted/App_Executables/Data Desert` (WASD/QE, drag to look), drawn with
  the fight's renderer (materials, the arena's lights, reflections, glow).
- `app/fight.cpp` (`openbg-fight`): a match against a computer-controlled robot, run by the game's own
  Last Man Standing session scripts (countdown, READY/FIGHT, knockouts, rounds, winner), both robots
  driven by their own compiled script brains, in an arena:
  `engine/build/openbg-fight source/extracted/App_Executables/Data Jaguar --opponent Katana --arena Desert`
  (`--opponent none` for solo; `--pilot` / `--opponent-pilot` pick pilots from Pilots.DEObjects,
  `--difficulty rookie|amateur|veteran|champion|world-champion` sets the AI quality, `--wins 1|2|5|10|20|50`
  the score limit (default 2); `--direct` skips the session and spawns both robots straight away (in the real arena, at the session's spawn points, each with its pilot driving it as in a session; the same robot can be both fighters); `--watch` lets the computer drive your robot too, for watching AI against AI.)
  The arena's song plays and follows the fight's moods (DirectMusic segments rendered by GothicKit
  dmusic, vendored in `engine/third_party/dmusic`, MIT, through a port of Microsoft's software synthesizer).
  The game's own HUD and messages are drawn (fonts from Core.ModuleDLL) and also printed in the
  terminal, and the view is the game's fight camera (`--free-camera` for a mouse-orbit camera); `OPENBG_DEBUG_SESSION=1` lists the session's modifiers,
  `OPENBG_DEBUG_EVENTS=1` traces event handle checks. Attacks land:
  hits run each robot's own damage, reaction and knockback scripts, and standing still (or walking
  back) blocks attacks from the front. `OPENBG_DEBUG_HITS=1` logs hits and knockback,
  `OPENBG_DEBUG_DANGER=1` the danger each robot perceives. Robots stand, walk and land on the arena's
  own collision surfaces (walls and slippery slopes stop them, headless runs included), and forces
  reach the arena's materials: footsteps raise dust, jump pads launch, Desert's sand mines explode
  under a robot, falling out of the arena kills, and landings and explosions shake the camera.
  Grabs and throws hold the victim on the thrower (forward + both punches close in; Jaguar's
  overhead throw is both punches in the air), robots push against each other, projectiles fly
  (Jaguar's concussion bolt: back, forward + punch; hold the punch to charge), and the arena's
  pickups spawn: duck over one to pick it up, punch with that hand to use it (the fighter-jet
  pickup calls in five jets). Computer robots follow the arena's navigation network, run when
  they want something badly, and go for pickups their pilot likes.
  (arrows = joystick: up/down walk, left/right turn; A/S = left/right punch, Z/X = left/right kick,
  Space = jump (add a direction to jump that way), Left Shift or C = duck/evade (add left/right to
  sidestep); drag to orbit). The terminal prints each state/animation change. `--headless N` runs N frames without a window and lists missing natives (a frame passes 20 ms of game time, the original's 50 frames a second, within which the fighters update every 16 ms; the window passes the real time since the last frame, at least 20 ms; `--frames` and the `@frame` options count frames too);
  `HOLD="left_punch@300-306,up@380-384"` scripts input in headless and `--screenshot` runs.
  Script debugging: `OPENBG_CALLS="Katana@400-402"` logs the methods a brain enters,
  `OPENBG_MEMBERS="Katana:216,312f"` prints brain members when they change, `OPENBG_METHODS="Katana:825"`
  names class method indices, `OPENBG_DEBUG_MOMENTUM=1` prints fighter momentum each frame, `OPENBG_DEBUG_PARTICLES=1` traces spawner requests, attack trailers and live effects, `OPENBG_DEBUG_SOUND=1` lists the sounds played, `OPENBG_DEBUG_MUSIC=1` the music player's segments, moods and dmusic's log (`OPENBG_NO_MUSIC=1` turns music off, `OPENBG_NO_SOUND=1` sound effects, `OPENBG_RECORD_AUDIO=out.f32` writes everything heard as raw 32-bit float stereo at the device rate; in a `--headless` run it mixes offline at 44100 Hz, heard from the player's fighter), `OPENBG_DEBUG_FORCES=1` traces forces and the materials they reach (and lists the force-receiving surfaces; `=profile` adds a height profile), `OPENBG_DEBUG_SURFACES=1` prints each fighter's surface contacts per frame (`OPENBG_DEBUG_FTICK=1` says why a fighter has none), `OPENBG_PLACE="Jaguar:0,100,1035@900"` puts a fighter somewhere at a frame (a Desert jump pad here; several separated by `;`), and `OPENBG_FIND="ProjectileActor@870"` prints where every object of that name is at a frame (parent, active, visible, brain state).
  More tracing: `OPENBG_DEBUG_NAV=1` navigation paths (`=dump` lists the links), `OPENBG_DEBUG_MAP=x0,x1,z0,z1,step` prints a top-down map of the fighting surfaces and of the danger the AI sees, `OPENBG_DEBUG_NEEDS=1` what each pilot wants (needs, powerup likelihood, `FindPowerupToAquire`), `OPENBG_DEBUG_PUSH=1` fighter-vs-fighter pushes, `OPENBG_DEBUG_FOLLOW=1` a state line per fighter every 10 frames (`OPENBG_EVERY=N` to change; `OPENBG_CHANGES=1` only on changes), `OPENBG_DEBUG_TESTER=1` collision testers and what they touched, `OPENBG_DEBUG_REFS=1` actors created from passive references, `OPENBG_DEBUG_TEMPORALS=1` what joins each temporal set (creatures, executors, testers) and at which rate, `OPENBG_DEBUG_INTERFACES=1` failed `Interface()` lookups, `OPENBG_DEBUG_SLIDE=1` `SlideTo`, `OPENBG_DEBUG_PATHS=1` the arena actors paths move (every 120 frames), `OPENBG_DEBUG_AI=1` each computer fighter's command list every 30 frames, `OPENBG_IDLE_AI=<robot>` gives that fighter no pilot in `--direct` runs (a still target), `OPENBG_DEBUG_INCIDENTS=1` incident recorders, `OPENBG_DEBUG_INPUTS=1` / `OPENBG_DEBUG_CHANNELS=1` dump the move sequences and animation channels. Rendering: robots wear their pilot's paint (`ImageFilter\OMF2Fighter`), the arena's `Light` objects light vertex-lit meshes (robots, vehicles, pickups) and splash light over projector-lit ones, stages carry their per-corner colours, cube and bump reflections, animated `Image\AVI` textures play their videos (uncompressed, Cinepak, and Indeo 4/5 through our own [openindeo](https://github.com/kadima-tech/openindeo) library) as the scripts set their phase (`de-tests` decodes every one; `DE_TESTS_AVI_DUMP=<dir>` writes their first frames), and glowing and specular passes go through the game's glow layer (`OPENBG_NO_GLOW=1` leaves it out, `OPENBG_GLOW_ONLY=1` shows it alone, `OPENBG_DEBUG_GLOW=1` lists its passes, `OPENBG_DEBUG_LIGHTS=1` the lights every 60 frames, `OPENBG_NO_PROJECTORS=1` leaves out the light projector passes, `OPENBG_CULL=none|flip` turns face culling off or around). Lights reach what their affector touches (each robot, the arena), cones with their own image project it (scorch decals, the current-target marker), and faces are culled as the original's `D3DCULL_CW`. Hits throw sparks and a short light flash (`OPENBG_DEBUG_PARTICLES=1` logs each burst; session mode only, as the spark template comes from the fighter's passive reference), and Gargoyle's wings are drawn and animated by their own sequence manager. `OPENBG_LIST_NATIVES=<file>` writes every registered native key. `OPENBG_DEBUG_MATERIALS=1` lists each material's passes and mesh bounds, `OPENBG_SKIP_MATERIAL=name` / `OPENBG_ONLY_MATERIAL=name` hide materials, `OPENBG_SKIP_ACTOR=name`, `OPENBG_NO_ACTORS=1` and `OPENBG_NO_PARTICLES=1` hide run-time actors and effects, `OPENBG_DEBUG_ACTOR_MESHES=1` lists the run-time actors drawn; `deobj-dump stages <Data> [file]` surveys material stage layouts (`DEOBJ_STAGES_ALL=1` lists every stage), `deobj-dump meshflags <file>` lists each mesh's lighting flags, and `deobj-dump image <Data> COREImages:LightMap out.png` exports images from the engine modules too.
- `tools/deobj_dump.cpp`: the `deobj-dump` inspector (`header`, `toc[+]`, `scripts`, `disasm`, `ex`, `block`, `blocks`, `refs <Data> <file> <object>` resolved reference lists, `particles <Data> [file]` a survey of every particle effect).

`deobj-dump scripts <file.DEObjects> <dir>` extracts a data file's script source (into your local copy only; it is the game's).
