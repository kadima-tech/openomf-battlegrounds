# Developing openomf-battlegrounds

Notes for working on the engine: the repository layout, the tools, and the test and debugging switches. Player-facing setup is in [README.md](README.md).

The port follows the final official patch, 2.123. `re/scripts/rtpatch_chain.sh` rebuilds `source/patched/v2123` (and the earlier versions) from the disc install and the official patch files ([PATCHES.md](PATCHES.md)); the build and tests use that tree when it is there, and the examples below name it. The disc's `source/extracted/App_Executables/Data` still loads. [DESIGN.md](DESIGN.md) describes the look and feel the port reproduces.

## What is in this repository

- `engine/`: the native reimplementation (C++20, CMake), described below. `engine/third_party/dmusic` is GothicKit's DirectMusic library (MIT), vendored, with its synthesizer replaced by a C port of Microsoft's DirectMusic software synthesizer sample (MIT).
- `re/FINDINGS.md`: the reverse-engineering ledger. What each part of the original engine does, with the module addresses it was read from. `re/opcodes.md` documents the script VM's bytecode.
- `re/scripts/` (Ghidra scripts: export, symbol fixes, cross-references) and `re/proto/` (the first Python prototypes of the data formats). The Ghidra scripts expect `GHIDRA_INSTALL_DIR`. They regenerate a local `re/dump/` of decompiled modules from your own copy of the game; those dumps are never committed.
- `PLACEHOLDERS.md`: every stand-in, fixed answer and missing piece.
- `tools/`: standalone modding tools. `tools/deobjects-editor` (`deobjects-edit`) unpacks a `.DEObjects` file to editable JSON and packs it back byte for byte; [docs/DEOBJECTS_FORMAT.md](docs/DEOBJECTS_FORMAT.md) documents the format.

## The engine

- `engine/`: the native reimplementation (C++20, CMake). It currently contains:
- `src/de`: the data layer. All shipped `.DEObjects` files parse (65 files, 28,115 objects), including Behavior script source.
- `src/vm`: the script VM. It reads the compiled VMC2 bytecode (249 classes, 10,582 methods) and runs it natively; shipped fighter-stat methods execute correctly.
- `src/de/scene.cpp`: actor hierarchy, transforms and materials, with cross-file references resolved through a `World` of all files.
- `src/de/platform.cpp` and `app/gl_api.h`: the operating-system layer. Reserved, lazily committed memory for the script and object arenas (mmap or VirtualAlloc), the user's settings folder (`~/Library/Application Support/openbg`, `%APPDATA%\openbg`, or `$XDG_DATA_HOME/openbg` on Linux) and Pictures folder, and the OpenGL 4.1 entry points (from the OpenGL framework on macOS, loaded through SDL elsewhere). Everything else is portable C++20 and SDL2; the engine builds on macOS, on Windows 10/11 with MinGW-w64 ([README](README.md#building-on-windows)) and on Linux.
- `app/viewer.cpp` (`openbg-viewer`): native SDL2/OpenGL fighter viewer with pilot recolouring and skeletal animation:
  `engine/build/openbg-viewer --data source/patched/v2123/Data --fighter Jaguar --anim LeftMediumPunch`
  (Left/Right cycles through the fighter's moves; `--list` prints them; Space pauses.)
- `tools/pcc.cpp` (`pcc`): compiles a Class from its behaviors' sources (`vm/compiler.cpp`, for the classes the game ships without bytecode) and prints the code next to the shipped VMC2 where there is one:
  `engine/build/pcc source/patched/v2123/Data SinglePlayer PSubSession 1`.
- `app/arena.cpp` (`openbg-arena`): native arena fly-through:
  `engine/build/openbg-arena source/patched/v2123/Data Desert` (WASD/QE, drag to look), drawn with
  the fight's renderer (materials, the arena's lights, reflections, glow).
- `app/frontend.cpp` (`openbg`): the front end, the game's own 3D menus from OMFBGGUI with their scripts
  (buttons light up and click, the doors close and open between pages), driven by a port of the UI
  managers' page flow (`src/game/ui.cpp`; FINDINGS 125-127):
  `engine/build/openbg source/patched/v2123/Data`. The main menu, the Quit page and Single
  Player's Choose Match work, and a chosen match's session runs its Choose Pilot / Choose Creature
  screens, then its matches: a tournament plays through to its cinematic and back to the main menu
  (FINDINGS 256-261, 272-274), and the Trophy Room and Training run. Other screens say they are not ported yet.
  `--launch Training` starts a match at once (`--host "Last Man Standing"` as a joinable session, whose Choose Cast lists the
  Available Arenas), `--choose Elizabeth,Jaguar` has its screens pick those, and
  `--headless N` runs without a window and lists the natives still missing. `--screenshot out.png --frames N` with
  `MOUSE="0.5,0.93@50;down@52;up@54"` (fractions of the 4:3 menu area) scripts the mouse, and `HOLD` (as in `openbg-fight`) the user's buttons.
  Screenshot and headless runs depend only on their inputs (data, `MOUSE`, `HOLD`, flags and switches):
  they pass 20 ms a frame, keep `rand()`'s start value (`OPENBG_SEED=n` picks one; normal play seeds it
  with the time, as the original does) and read `SystemTime()` as 20 ms a frame (normal play: the wall
  clock). `SHOTS` and the `OPENBG_DEBUG_*` switches change nothing (`OPENBG_RECORD_AUDIO` does: its music
  player reports segment ends to the music scripts, as a sound device does); `OPENBG_DEBUG_STATE=N` prints the
  `rand()` state, a hash of the scene and the robots' positions every N frames, to compare runs.
- `app/fight.cpp` (`openbg-fight`): a match against a computer-controlled robot, run by the game's own
  Last Man Standing session scripts (countdown, READY/FIGHT, knockouts, rounds, winner), both robots
  driven by their own compiled script brains, in an arena:
  `engine/build/openbg-fight source/patched/v2123/Data Jaguar --opponent Katana --arena Desert`
  (`--opponent none` for solo; `--arena PracticeArena` is Training's room; `--pilot` / `--opponent-pilot` pick pilots from Pilots.DEObjects,
  `--difficulty rookie|amateur|veteran|champion|world-champion|master` sets the AI difficulty (default rookie, as the registry's), `--wins 1|2|5|10|20|50`
  the score limit (default 2); `--direct` skips the session and spawns both robots straight away (in the real arena, at the session's spawn points, each with its pilot driving it as in a session; the same robot can be both fighters); `--watch` lets the computer drive your robot too, for watching AI against AI.)
  The arena's song plays and follows the fight's moods (DirectMusic segments rendered by GothicKit
  dmusic, vendored in `engine/third_party/dmusic`, MIT, through a port of Microsoft's software synthesizer).
  The game's own HUD and messages are drawn (fonts from Core.ModuleDLL) and also printed in the
  terminal, and the view is the game's fight camera (`--free-camera` for a mouse-orbit camera); `OPENBG_DEBUG_SESSION=1` lists the session's modifiers,
  `OPENBG_DEBUG_EVENTS=1` traces event handle checks. In `openbg` screenshot and headless runs, `OPENBG_AUTOPLAY=<frame>` hands the player's fighter to the AI from that frame (the scripts still see it as the user's) and `OPENBG_KO=<frame>,...` drains every computer fighter's health then, the player named as the attacker and credited as a hit would credit it (BFighter's `m_LastDamageInflictor`, FINDINGS 280: the death event names the player and the destruction-hit event reaches it; `OPENBG_DEBUG_SENDS=Dead` prints the events whose handle contains that text, with INT3 and the first two objects); `OPENBG_KO=auto[:N]` does it N frames (default 150) after each start of the action, which plays a whole tournament (`--launch "North American Open" --choose Elizabeth,Jaguar` with `MOUSE` presses every 250 frames for the rules displays, notifications and the cinematic; the drain is no hit, so a fighter it catches in a state only a hit ends, such as an AI Pyros's duck, stays up: another N gets past it). `OPENBG_HELP_CYCLE=<frame>,<every>` shows the open description selector's entries in turn from that frame (every help page, with `SHOTS`), `OPENBG_PLACE` works in `openbg` too (Training's pads), `OPENBG_DEBUG_REGISTRY=1` prints registry values as they change (unlocks), `OPENBG_DEBUG_UI=1` the page flow (each page queued, activated and completed, rules displays, closed stacks), `OPENBG_DEBUG_TREE=<name>@<frame>` prints the scene branches under objects of that name, `OPENBG_DEBUG_MEMORY=N` the script arena's size and what the world holds (objects, brains, temporals, collision testers, affectors, tick hooks, particle instances; destroyed ones counted apart) every N frames (`openbg-fight` adds the frames' tick time, the draw time in windowed and screenshot runs, and the sound mixer's voices and speaker elements), and `OPENBG_DEBUG_VM_ERRORS=1` the instruction a script error left from; screenshot runs end by listing the log and the natives still missing. `OPENBG_QUIT_SESSION=<frame>` leaves the session then as the in-game menu's Main Menu does (closing a recording), `OPENBG_DEBUG_REPLAY=1` traces a recording's or replay's state managers, fighter positions and creatures given their first tick by `SetTime`, and `OPENBG_DEBUG_REWIND=<frame>:<ms>` sets a replay's clock back then. Attacks land:
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
  names class method indices, `OPENBG_DEBUG_MOMENTUM=1` prints fighter momentum each frame, `OPENBG_DEBUG_PARTICLES=1` traces spawner requests, attack trailers and live effects, `OPENBG_DEBUG_SOUND=1` lists the sounds played (and when each script speaker's voice starts and ends, with its volume, frequency, progress and gains every 50 frames), `OPENBG_DEBUG_MUSIC=1` the music player's segments, moods and dmusic's log (`OPENBG_NO_MUSIC=1` turns music off, `OPENBG_NO_SOUND=1` sound effects, `OPENBG_RECORD_AUDIO=out.f32` writes everything heard as raw 32-bit float stereo at the device rate; in a `--headless` run it mixes offline at 44100 Hz, heard as in a window), `OPENBG_DEBUG_FORCES=1` traces forces and the materials they reach (and lists the force-receiving surfaces; `=profile` adds a height profile), `OPENBG_DEBUG_SURFACES=1` prints each fighter's surface contacts per frame (`OPENBG_DEBUG_FLOOR=1` the floor under it, its turn, the fighter's place in its frame and the momentum it carries) (`OPENBG_DEBUG_FTICK=1` says why a fighter has none), `OPENBG_PLACE="Jaguar:0,100,1035@900"` puts a fighter somewhere at a frame (a Desert jump pad here; several separated by `;`), `OPENBG_ENERGY="Force@250"` fills a fighter's energy bar at a frame (super moves: hold both punches through a special move's prelude; several separated by `;`), and `OPENBG_FIND="ProjectileActor@870"` prints where every object of that name is at a frame (its Z axis, parent, active, visible, brain state).
  More tracing: `OPENBG_DEBUG_NAV=1` navigation paths (`=dump` lists the links), `OPENBG_DEBUG_MAP=x0,x1,z0,z1,step` prints a top-down map of the fighting surfaces and of the danger the AI sees, `OPENBG_DEBUG_NEEDS=1` what each pilot wants (needs, powerup likelihood, `FindPowerupToAquire`), `OPENBG_DEBUG_PUSH=1` fighter-vs-fighter pushes, `OPENBG_DEBUG_FOLLOW=1` a state line per fighter every 10 frames (`OPENBG_EVERY=N` to change; `OPENBG_CHANGES=1` only on changes), `OPENBG_DEBUG_TESTER=1` collision testers and what they touched, `OPENBG_DEBUG_REFS=1` actors created from passive references, `OPENBG_DEBUG_TEMPORALS=1` what joins each temporal set (creatures, executors, testers) and at which rate, `OPENBG_DEBUG_INTERFACES=1` failed `Interface()` lookups, `OPENBG_DEBUG_SLIDE=1` `SlideTo`, `OPENBG_DEBUG_PATHS=1` the arena actors drawn live because paths or scripts move them (every 120 frames), `OPENBG_EYE=x,y,z,tx,ty,tz` in `openbg-fight` and `openbg` looks from a point at another (to watch a moving part), `OPENBG_DEBUG_AI=1` each computer fighter's command list every 30 frames, `OPENBG_IDLE_AI=<robot>` gives that fighter no pilot in `--direct` runs (a still target), `OPENBG_DEBUG_INCIDENTS=1` incident recorders, `OPENBG_DEBUG_INPUTS=1` / `OPENBG_DEBUG_CHANNELS=1` dump the move sequences and animation channels. Rendering: robots wear their pilot's paint (`ImageFilter\OMF2Fighter`), the arena's `Light` objects light vertex-lit meshes (robots, vehicles, pickups) and splash light over projector-lit ones, stages carry their per-corner colours, cube and bump reflections, animated `Image\AVI` textures play their videos (uncompressed, Cinepak, and Indeo 4/5 through our own [openindeo](https://github.com/kadima-tech/openindeo) library) as the scripts set their phase (`de-tests` decodes every one; `DE_TESTS_AVI_DUMP=<dir>` writes their first frames; `OPENBG_DEBUG_AVI=1` logs each one's decoding and, every 250 updates, the phase its scripts set and the frame shown), and glowing and specular passes go through the game's glow layer (`OPENBG_NO_GLOW=1` leaves it out, `OPENBG_GLOW_ONLY=1` shows it alone, `OPENBG_DEBUG_GLOW=1` lists its passes, `OPENBG_DEBUG_LIGHTS=1` the lights every 60 frames, `OPENBG_NO_PROJECTORS=1` leaves out the light projector passes, `OPENBG_DEBUG_PROJECTORS=1` lists each pass's faces and each search for a light's touched polygons (FINDINGS 283), `OPENBG_CULL=none|flip` turns face culling off or around). Lights reach what their affector touches (each robot, the arena), cones with their own image project it (scorch decals, the current-target marker), and faces are culled as the original's `D3DCULL_CW`. Hits throw sparks and a short light flash (`OPENBG_DEBUG_PARTICLES=1` logs each burst; session mode only, as the spark template comes from the fighter's passive reference), and Gargoyle's wings are drawn and animated by their own sequence manager. `OPENBG_LIST_NATIVES=<file>` writes every registered native key. `OPENBG_DEBUG_MATERIALS=1` lists each material's passes and mesh bounds, `OPENBG_SKIP_MATERIAL=name` / `OPENBG_ONLY_MATERIAL=name` hide materials, `OPENBG_SKIP_ACTOR=name`, `OPENBG_NO_ACTORS=1` and `OPENBG_NO_PARTICLES=1` hide run-time actors and effects, `OPENBG_DEBUG_ACTOR_MESHES=1` lists the run-time actors drawn; `deobj-dump stages <Data> [file]` surveys material stage layouts (`DEOBJ_STAGES_ALL=1` lists every stage), `deobj-dump meshflags <file>` lists each mesh's lighting flags, and `deobj-dump image <Data> COREImages:LightMap out.png` exports images from the engine modules too.
- `app/net_harness.cpp` (`openbg-net`): network play, phase 1 (`docs/NETWORK_PLAN.md`). A server and a client
  World in one process, headless and in lockstep (20 ms a frame), joined by the loopback link (`src/net/link.h`:
  a reliable ordered and an unreliable channel, the interface the ENet transport will implement) with a simulated
  delay, jitter and loss. Both run the same Last Man Standing match; slot 0 is the server's, slot 1 the client's.
  States replicate as the original's do (`game/netplay.h`): clients send their own fighter, the server everything,
  receivers insert them (hit reactions in the past, re-simulating the creature), outcomes run only on the server:
  `engine/build/openbg-net source/patched/v2123/Data --ping 100 --jitter 10 --loss 0.02 --frames 4000 --client-ai`
  (`--server Katana:Elizabeth`, `--client Jaguar:Lucas`, `--arena`, `--wins`, `--difficulty`, `--budget` bytes/s,
  default the original's 16000; `--ping` is the round trip, half each way; the lockstep adds one frame, so 0 ms
  measures 20). The server's fighter is driven by its pilot's AI unless `--server-user` (then `HOLD` scripts it);
  the client's by `CLIENT_HOLD` (same syntax as `HOLD`), or its AI with `--client-ai`. Every `--every N` frames (default
  50) it prints both machines' view of each fighter (position, health, state) and the client copy's distance from
  the server's, and per machine the states received, inserted, rejected and held, corrections, rollbacks and the
  ticks they re-simulated, bytes sent, lag and ping; a summary ends the run (position error mean, median, 95th
  percentile and maximum once both copies have landed). `OPENBG_DEBUG_NET=<frame>` lists both machines' scene roots
  and state managers (server ID, replication flags, control, newest state) then; `OPENBG_DEBUG_NETSTATES=1` traces
  each received state's placement. `OPENBG_SEED` seeds both machines' `rand()`.
- Network play, phase 2 (`docs/NETWORK_PLAN.md`): `openbg --host "Last Man Standing"` serves the match (its Choose
  Cast and Choose Environment as usual) on `Network\Winsock\Listener Port` (default 2002; `--port N` overrides), and
  `openbg --join host[:port]` joins one; in the menus, Host Game does the same and Join Game is the session browser (LAN
  sessions by broadcast every 500 ms, or a typed address). The transport is ENet (`src/net/enet_link.h`, fetched pinned at
  1.3.18): a reliable ordered and an unreliable channel, the original's delay simulator (`Network\Winsock\Delay Packet
  Time` and `Delay Packet Time Added Variation`, ms, applied to sends on that machine), "interrupted" after 6 s of silence and
  dropped after `Network\Kick Time`. The handshake (`src/game/netgame.cpp`) refuses another protocol, engine version
  (`OPENBG_VERSION` in release builds, "dev" otherwise) or data hash (printed as `data xxxxxxxx` at start), and a wrong
  `Network\Server Password` (a joining client sends its own registry value with `--join`). Each machine prints its status
  changes (`net: status ...`, `net: Guest reports ...`); `OPENBG_DEBUG_NETSTATES=1` also traces ID requests and links.
  Test switches: `OPENBG_ALL_AI=1` gives every fighter, the user's too, to its pilot's AI; `OPENBG_NET_TRACE=<file>` writes
  each frame's scene time, wall clock and every slot's fighter (place, health, state, creature time); headless network runs
  are paced to the wall clock. `tools/net-lan/run.sh <Data> [--ping ms] [--frames N] [--port N] [--out dir] [--arena "The Desert"]
  [--host-fighter Elizabeth,Katana] [--client-fighter Lucas,Jaguar]` runs a host and a client as two headless processes
  with scratch HOMEs on localhost (started together; `OPENBG_JOIN_RETRY=1` makes `--join` try again until the server
  accepts it; `HOST_ENV` / `CLIENT_ENV` add settings to one side), both picking through the game's pages (the
  client's opened remotely by the host), plays Last Man Standing with the AI and prints `tools/net-lan/compare.py`'s
  position and health errors of the client's copies against the server's at the same wall-clock moment, each
  fighter's clock offset, and per object name (projectiles, pickups, hazards: the trace's " # " part lists every
  replicated creature and every projectile in the scene, linked or not) the frames each machine had one and how far
  the client's copies were from the server's. A `--join` screenshot run (`--screenshot`, `SHOTS`, `OPENBG_EYE`) keeps
  the wall clock's pace and writes the trace too. `OPENBG_CHANGE_FIGHTER=<frame>` presses the in-game menu's Change
  Fighter then. `net-tests` (ctest, no game data) checks the transport on localhost.
- `tools/deobj_dump.cpp`: the `deobj-dump` inspector (`header`, `toc[+]`, `scripts`, `disasm`, `ex`, `block`, `blocks`, `refs <Data> <file> <object>` resolved reference lists, each target with its object index, `particles <Data> [file]` a survey of every particle effect).

`deobj-dump scripts <file.DEObjects> <dir>` extracts a data file's script source (into your local copy only; it is the game's).

## The content editor

`openbg-editor` (`engine/editor`, spec in [docs/EDITOR_SPEC.md](docs/EDITOR_SPEC.md)) edits a
**project**: one add-on `.DEObjects` file that adds objects with fresh GUIDs. The base `Data`
folder is only read. It is built with the engine (`cmake --build engine/build --target openbg-editor`);
CMake fetches Dear ImGui (docking branch, pinned tag), and the fonts are built in from
`engine/third_party/fonts`.

```sh
engine/build/openbg-editor --data source/patched/v2123/Data              # browse; open or create an add-on from the UI
engine/build/openbg-editor --data source/patched/v2123/Data ~/Mods/MyMod.DEObjects   # straight into an add-on
```

Without `--data` it asks for the Data folder once (remembered). Settings, recent add-ons and the dock
layout live in `<user data>/openbg/editor` (`layout.ini`; Tools > Reset Layout restores the default).

**Step by step**
1. **Point it at the game.** Choose the 2.123 `Data` folder. Only the 65 files' headers are read at
   start (about 20 ms for the 28k objects); a file's objects are read on a background thread the
   first time something in it is shown, and kept.
2. **Browse the base game.** The main window opens straight away with **no add-on open** (nothing is
   created for you). The Content Browser's left side has the base game **By Type** (Pilots, HARs,
   Arenas, Tournaments, Images, Meshes, Sounds, Music, Scripts, Descriptions, Values, Particles,
   Materials, Animation, Scene, Other, with counts) or **By File** (every `.DEObjects`, expandable
   into its folders). The right side shows tiles (thumbnails for images, pilot photos and
   description icons, made a few per frame and cached; a class icon otherwise) or a list; the Type
   filter and the search box work across all objects. Every base tile has a **BASE** badge; add-on
   objects have **NEW**.
3. **Look at anything.** A click selects it and fills **Details**: class, GUID, file, index, flags,
   sizes, extended data, the fields the editor decodes (descriptions, pilots, images, values,
   sounds, behaviors), parent and children, **References** and **Referenced by** (clickable; the
   base game's reverse index is built on a background thread the first time it is needed), and the
   raw chunks as hex. A double-click opens a viewer tab: images (fit / 1:1, R G B A channels),
   Behavior scripts (read-only, syntax coloured), sounds (waveform, Play / Stop), HARs (the robot,
   animated, drag to turn), base pilots in the pilot editor **read-only**. Meshes and music show their
   data but no preview yet. In By File, a double-click on a folder goes into it.
4. **Open or make an add-on.** File > Open Add-on, File > Recent, or the welcome card in the viewport
   (Reopen <last>, Open, New, recent list). File > New Add-on asks where to save and writes the empty
   add-on there. File > Close Add-on returns to browsing.
5. **Make a pilot.** Base content is read-only: the original game cannot override a base object
   (FINDINGS 430-437), so you copy it. Right-click a base pilot > Copy to Add-on, the Details
   button, or the banner in the read-only pilot editor. Without an open add-on it first asks which
   one (recent add-ons, Open..., New...); nothing is written until you choose. The copy (fresh
   GUIDs, listed, unlocked, named "<Name> Copy") opens in the pilot editor. Content > New Pilot does
   the same from a pilot you pick.
6. **Edit, save, test.** Edit the pilot's tabs, Ctrl+S to save, F7 to validate, F5 to launch the
   game with the add-on (below).

- **This iteration:** the shell (menus, document tabs, toolbar, Place / Viewport / Outliner / Details,
  Content Browser, Output Log, Validation, Script Console, status bar) and the **pilot editor**:
  Content > New Pilot copies a base pilot (its description, bio and photo) with fresh GUIDs, listed and
  unlocked; the tabs edit name, sex, photo (PNG or BMP import, cropped and stored as an uncompressed
  256 x 256 `Image\Standard`), bio, listed / locked flags, casts, the four attributes, the 31 AI
  traits and the three paint colours. The preview shows the photo, or the robot posed and painted in
  the pilot's colours (the viewer's fighter code, `app/fighter_model.*`). Ctrl+S saves, Ctrl+Z undoes.
  Arena, HAR and tournament editing, Play and Simulate are shown but not active yet.
- **Validate** (F7) runs the §7 checks that apply; rules only the original 2.123 enforces are warnings.
- **Launch Game** (F5) writes the add-on to `<user data>/openbg/editor/launch/` and starts `openbg`
  (next to the editor, or `OPENBG_GAME`) with `<Data> --addon <file>`; its output goes to the Output Log.
  `openbg --addon file.DEObjects` (repeatable) and `OPENBG_ADDONS` (paths separated by `:`, `;` on
  Windows; read by every program's `de::World`) load add-on files as if they were in `Data`.
- **Scripted runs** for screenshots and checks: `--new-pilot Lucas:Jetta`, `--open-pilot Jetta`,
  `--tab 0-4`, `--preview photo|robot`, `--photo file.png`, `--content <type>`, `--base-content`,
  `--browse-file <stem>`, `--open-base Class:Name`, `--select-base Class:Name`, `--copy-base <pilot>`,
  `--select <object>`, `--level`, `--save`, `--launch-game`, and `--screenshot out.png [--frames N]
  [--size WxH] [--scale S]` (a hidden window, 1600x1000 points at 2 pixels per point by default).
- `editor-tests` (ctest `editor-pilot`) checks the base content index (object counts per type),
  reference decoding and behavior source, new pilot -> edit every field -> photo import -> save ->
  reopen (then the engine's own readers on the add-on), and copying a base pilot into a chosen,
  existing add-on with nothing written before the add-on is chosen or saved.
- Memory: files stay in memory once read; browsing every image in the game reads most of `Data`.

