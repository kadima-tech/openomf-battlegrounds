# Content editor: spec (draft for iteration)

A visual, multiplatform editor for the game's `.DEObjects` content, so that people can make **new
pilots, tournaments, arenas and HARs**. The existing `deobjects-edit` CLI stays and is used for
scripting and batch work. Mockups: https://claude.ai/artifact/Bzjd4q5YcLaSkPPWA511LL (private until
shared). Status: draft, 2026-11-06.

## 1. What the data allows (research summary)

- **Add-on files work.** Both the original and our engine load every `*.DEObjects` in `Data`
  (FINDINGS 192; ours in name order, and a later file wins a GUID clash). Content is found by class and
  cast, not by a list: Choose Pilot / Robot / Arena / Match gather every description of their class
  that is listed, unlocked and in the right cast. So **all edits go into one new add-on file**. The
  shipped files are never changed, which also keeps the launcher's 2.123 checksum check happy.
- **Pilot:** an `Actor\Creature\Pilot` with:
  - power / agility / endurance / focus, each 0-100;
  - three paint colours with weights;
  - 31 AI traits, each a level 0-8, variation 0-5 and wave 0-3;
  - sex, used for quote pronouns;
  - a photo, plus a `Description\Pilot` with the bio, the listed / locked flags and its casts.

  This is all data. Quotes are not stored with the pilot; they live in tournament scripts.
- **Tournament:** a `Description\SessionMaster` plus a session master, made of description data and a
  script. The matches, quotes, commentary, branching and unlocks are the `BSubSession` script: one
  method per match. Data-only edits can retarget the behaviour globals, such as an opponent pilot or
  the ending arena. Anything else needs the script compiler.
- **Arena:** an `ArenaDescriptions` entry (name, image, 3-10 players, casts, help text, components),
  plus one large environment tree:
  - meshes and materials, lights and a camera;
  - fighting surfaces and force affectors (collision groups);
  - spawn points and pickup spawners (references into SharedObjects);
  - an 86-link navigation net;
  - hazard scripts and a music reference.
- **HAR:** a fighter file and one paint file (`<Name>Model1`).
  - **Data:** movement and jump stats, 102 animation sequences, input sequences, hit and attack spheres,
    sounds, effects, paint images, and the 3 LOD meshes with skin weights.
  - **Script:** moves, damage, stun and supers (`BJaguar` alone is 13.7k lines).
- **Scripts ship as source,** and our compiler matches the shipped bytecode (FINDINGS 132). There is no
  bytecode (VMC2) writer yet. Both engines compile a class at load when its bytecode is missing, but
  the original runs nothing if that compile fails (FINDINGS 21, 158).
- **Assets:**
  - images and WAVs: decode and encode are easy;
  - meshes and skeletal animation: decode yes, import moderate to hard (per-face UVs, 3 LODs, a named
    15-17 bone rig the scripts refer to);
  - AVI and DirectMusic: reuse what's there.
- **Existing writer:** `tools/deobjects-editor` unpacks to JSON and packs back, but **the round trip is
  lossless for only 16 of 65 files**; the rest come out 2-32 bytes short. This must be fixed first.

## 2. Technology (options)

| Option | For | Against |
|---|---|---|
| **C++ + Dear ImGui (docking) on our engine** (recommended) | Reuses `libde` (format, cross-file resolution), `libvm` (compiler) and the real OpenGL renderer for previews, so they look exactly like the game. One codebase, one CMake build already working on macOS (arm64 / x86_64), Windows (MinGW) and Linux. Docking panels suit a tool. The original's own editor was "another build of the same engine codebase" (2002 developer chat). | ImGui looks like a tool, not a consumer app (themeable, as in the mockups). Text-heavy forms are more work than in a web UI. |
| Qt 6 (Widgets/QML) + our libraries | Polished native widgets, strong forms and tree views | Large dependency (LGPL dynamic linking or commercial licence). A GL viewport has to share a context with our renderer. Packaging is heavier. |
| Tauri/Electron web UI + engine as a sidecar (or wasm) | Fastest forms and UI iteration | Previews need the renderer over IPC or in WebGL (a second renderer). Two languages and two builds. |

**Recommendation: Dear ImGui.** It's the only option that puts the game's exact renderer and data model
in the same process with no glue. Forms are the cheaper part to build.

## 3. Product shape

- **Unreal-Editor-style shell.** Every panel is a dockable tab (Dear ImGui docking): drag it to split,
  stack, float, or move to a second monitor. Layouts are saved per user, with a "reset layout" option.
  - **Main window** (the arena / level view):
    - a toolbar (Save all, Content, Validate, Place, **Play**, play mode, **Simulate**, **Launch game**);
    - a Place panel (spawn points, pickup spawners, hazards, surfaces, nav links, lights, effects,
      sounds, prefabs);
    - 1, 2 or 4 **viewports** (perspective / top / front / side; lit, unlit and collision views; Show
      menu for surfaces, nav and spawns; move / rotate / scale gizmo with snapping);
    - **Outliner** and **Details** on the right;
    - a bottom dock with **Content Browser** (the add-on's objects marked NEW / OVERRIDE, and the
      read-only base game, with thumbnails and type filters), Output Log, Validation and Script
      Console.
  - **Asset editors** (pilot, HAR, tournament, script) open as document tabs along the top, or float
    as their own windows. Each has its own docked panels and a live preview drawn by the game's
    renderer (the screens in the mockup's "asset editors" row).
- **Play in editor.** The editor is a build of the engine, as the original's was.
  - **Play** runs a match inside the selected viewport, in the same process. The play mode menu sets
    the robot, opponent, arena and game type, or starts a tournament at a chosen match.
  - A green border shows the game is running.
  - **Pause**, **step one tick**, slow motion, **Stop**.
  - **Eject (F8)** frees the camera and selection while the match keeps running, as in Unreal.
  - A **live inspector** for the selected creature: state, animation and time, health / energy / stun,
    momentum, floor, input buffer.
  - **Debug toggles** cover today's `OPENBG_*` switches: hit spheres, the nav net and AI target, a full
    energy bar, an idle dummy opponent, AI difficulty.
  - **Edits while playing:** data edits (stats, colours, sphere sizes) apply live. Script edits
    recompile when you press Stop.
  - **Simulate** runs the arena (hazards, AI against AI) without taking control of a fighter.
  - **Launch game** starts a separate `openbg` on the base data plus the project, for full-flow tests
    (menus, Choose Pilot, tournaments).
  - **Network test:** play as a client of a server running in the editor, with set latency, jitter and
    loss, using the network plan's step-1 link.
- A **validation** bar and unsaved changes sit at the bottom.
- **Projects** are one add-on `.DEObjects` file, plus a sidecar folder for source assets (PNGs, WAVs,
  later glTF) so imports can be redone.
- **New = copy.** Every "new" object starts as a deep copy of a base object with fresh GUIDs, such as a
  pilot from Lucas or a HAR from Jaguar. The editor keeps the references and GUID pool right.
- **Validation** runs on save:
  - GUIDs are unique and every reference resolves;
  - scripts compile;
  - every input sequence a script names exists;
  - spawn points are on fighting surfaces and the nav net is connected;
  - every match reaches the end;
  - images are in a size and format the game accepts.

## 4. Editors: v1 scope

| Editor | v1 (data and script, no importers) | Later |
|---|---|---|
| Pilot | Everything in §1: name, bio, photo import, stats, paint colours (with a live robot recolour), 31 AI traits with presets from any pilot, listed / locked / visible, casts | Per-pilot story or endings (needs tournament scripting, below) |
| HAR | Variant of an existing robot: movement and jump stats, input sequences, hit and attack sphere sizes, sound and effect swaps, new paint jobs (image import), the script's constants (damage, stun) through a generated edit, an animation preview and timeline (view only) | glTF model and skin import; glTF animation import and retargeting; new moves |
| Arena | Description (name, image, players, game types, help and hazard text, music); a variant of an existing scene (lights, spawn points, pickup spawners, prefabs from other arenas); overlays for surfaces, nav and hazards | glTF geometry, collision and fighting-surface import; nav-net generation; new hazards |
| Tournament | A visual ladder: matches with opponent pilot, robot, arena (fixed or random), quotes, commentary, drop-ons, win / lose branches, unlocks, ending cinematic. **Generated as the sub-session script** and compiled; hand edits are kept in marked blocks | The economy (§5) |

The script editor is a shared panel: syntax highlighting, compile errors from our compiler, jump to
definition, and a diff against the base class.

## 5. Stretch goals

### 5.1 Proper tournament mode (money, upgrades, unique modifiers)
- 2.123 has **no** money, shop or upgrade code anywhere. The developers confirmed this in 2002:
  "simple ladder-based tournaments for single-player, and no robot upgrades" (Saul, developer chat,
  2002-09-23).
- They described the next version as having deeper "interaction between pilot and fighter through
  stats" (Kevin, same chat). So this mode would be new design, in the spirit of OMF 2097's tournament.
- **Parts:**
  - a save game per career (registry-backed, like unlocks);
  - prize money and entry fees per match;
  - an upgrade shop that scales robot stats and the script constants within limits;
  - per-tournament session modifiers.
- **Needs:** a few new natives, or a script-side store for career state; new UI pages; balancing.
- **Editor side:** the Economy panel in the tournament editor (drawn in the mockup, tagged STRETCH).

### 5.2 Features announced but never shipped
No design document is public. These come from the official site's FAQ (2002-2003) and the
2002 developer chat (both on archive.org), plus what is in the data:
- **Mod tools:**
  - "Our in-house editor will allow you to alter all scripts and game variables, as well as import
    content such as models, animations, and music."
  - Never released, along with the "DE Unraveller" 3DS Max exporter.
  - This editor is that promise.
- **Pharaoh's Fire arena:** announced in the FAQ (12 arenas in total, 2 of them unannounced). Its
  `Description\Environment` and icon are in `ArenaDescriptions` and a script declares
  `g_EnvironmentPharaohsFire`, but it isn't one of the selectable arenas. Whether its scene is in
  `DesertRings.DEObjects` (which also names it) still needs checking.
- **Three paint jobs per robot:** the FAQ says "you can pick from three paint jobs"; every robot ships
  only `Model1`.
- **Logos:** team, clan or personal logos stamped on robots ("currently considering").
- **Pilot stories:** a unique story and ending for every principal pilot ("if time permits, … for
  secondary characters as well").
- **Online stats and ranking:** an online ranking / stats system ("NG stats"-like). This ties into
  the network plan.
- **Replays:** saving a recording as an AVI video, slow-motion replay playback.
- **More game types** beyond Demolition, Last Man Standing and their team versions ("some of them may
  not get implemented").
- **Leftovers in the data:**
  - HAR Master trophies exist for Force, Chronos, Katana and Pyros, but tournaments only for Jaguar,
    Gargoyle, Mantis and Warlord.
  - Canyon's boulder pickup is never spawned.
  - A "Basic Training" session master is named by the 2004 build but absent from retail.

## 6. Prerequisites and risks
1. **Lossless writing.** Fix the 49 of 65 files whose JSON round trip loses 2-32 bytes, or have the
   editor write at the object level through the engine's writer. Writing must be byte-exact for
   untouched objects.
2. **Schemas.** Add the pilot, fighter-stats, input-sequence, behaviour-globals and
   arena-description chunk layouts (their readers already exist in the engine) to the writer, with
   round-trip tests.
3. **Script output.** "Drop VMC2 and compile at load" works in both engines; a VMC2 writer is needed
   so a bad compile can't silently break the original game. Decide whether add-ons must run on the
   original exe at all (§7).
4. **Override semantics.** "A later file wins a GUID clash" is inferred for the original, not tested.
   Unknown fields (`value84`, the EX flags, description +0xcc…+0xd4) must be preserved.
5. **Arena consistency.** Navigation, spawns and collision groups must stay consistent, or the AI
   breaks.
6. **Importer scope.** glTF model, skin and animation import against the fixed rig is the largest
   single item.

## 7. Decisions and open questions
Decided (2026-11-06):
- **Both engines.** Add-ons must load and work in OpenBG **and** the original 2.123 game, so the editor
  writes VMC2 bytecode for every script it touches and enforces the original's loader rules
  (§6 items 3-4; the compatibility rules are being established).
- **Lossless writing** is a hard requirement (§6 item 1).
- **Platforms:** macOS (arm64 and x86_64), Linux and Windows, from the same CMake build as the engine.
- **Visual direction:** like Unreal Engine 5. Dark charcoal panels (#191a21 ground, #1d1e26 panels,
  #33343f borders), a **violet accent** (#3a3566 selected rows, #2e2a52 active document tab, #5a4fc0
  tab underline, #6b5ce7 checkboxes), green Play, amber for warnings and STRETCH, Inter for UI and
  JetBrains Mono for code. Title "DEObjects Editor — <project>". Asset editors are document tabs; a
  status bar shows the add-on and "DE v2.123". In the pilot editor, a large in-engine preview sits on
  the left and tabbed forms (General / Attributes / AI Traits / Paint / Casts & Flags) on the right. The
  tournament editor is a tree of matches, a match form, and the generated `BSubSession` script with
  Quotes as a tab.

Open:
1. Mod distribution: just the `.DEObjects` plus an optional manifest (name, author, version,
   dependencies), or a mod browser later, tied to the lobby server?
2. Which stretch goal comes first after v1: the tournament economy, or the announced-but-cut features
   (paint jobs, logos, the Pharaoh's Fire arena)?
3. Testing in the original game needs a real Windows install of 2.123 (we don't run the protected
   exe). Who runs those checks: the user, or a tester?

## 8. Milestones
1. **Writer foundations:** a lossless round trip on all 65 files, the new schemas and an add-on file
   builder. Tests in ctest.
2. **Shell:** the ImGui docking app (layouts, document tabs), Content Browser, Outliner, Details,
   validation bar, viewports (reusing `openbg-viewer` / `openbg-arena` code), Play in editor with
   pause / step / eject and the live inspector, and Launch game.
3. **Pilot editor** (first end-to-end path: new pilot → save → shows in Choose Pilot).
4. **Tournament editor** with the generated script.
5. **HAR variant editor** (stats, inputs, hitboxes, paint import).
6. **Arena variant editor** (description, spawns, pickups, lights, prefabs).
7. **Importers** (glTF), then the stretch goals.
