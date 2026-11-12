# Content editor: spec (draft for iteration)

A visual, multiplatform editor for the game's `.DEObjects` content, so that people can make **new
pilots, tournaments, arenas and HARs**. The existing `deobjects-edit` CLI stays and is used for
scripting and batch work. Mockups: https://claude.ai/artifact/Bzjd4q5YcLaSkPPWA511LL (private until
shared). Status: draft, 2026-11-06.

## 1. What the data allows (research summary)

- **Add-on files work.** Both the original and our engine load every `*.DEObjects` in `Data`
  (FINDINGS 192, 433), with no registration. Content is found by class and cast, not by a list:
  Choose Pilot / Robot / Arena / Match gather every description of their class that is listed,
  unlocked and in the right cast. So **all edits go into one new add-on file** that **adds** objects
  with new GUIDs; it cannot reliably override a base object in the original (§7). The shipped files
  are never changed, which also keeps the launcher's 2.123 checksum check happy.
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
- **Scripts ship as source plus VMC2 bytecode.** `vm::VMClass::write()` writes VMC2 exactly as the
  original saves it (all 255 shipped payloads round-trip, FINDINGS 430), and our compiler emits what
  the original's loader checks (FINDINGS 431). It compiles all 255 shipped classes, and every one
  runs as its 2.123 VMC2 does (FINDINGS 650-656); 26 payloads and 5,837 of 11,027 methods are
  byte-identical, the rest differing in the class and intrinsic table order, a few frame sizes and
  temporary slots, none of which matters for running it. The original compiles a class whose VMC2
  is missing or stale, and **quits** if that compile has an error (FINDINGS 436); so the editor
  compiles strictly: the four errors 2.123's own tools compiled past in shipped classes (FINDINGS 651)
  are errors there.
- **Assets:**
  - images and WAVs: decode and encode are easy;
  - meshes and skeletal animation: decode yes, import moderate to hard (per-face UVs, 3 LODs, a named
    15-17 bone rig the scripts refer to);
  - AVI and DirectMusic: reuse what's there.
- **Existing writer:** `tools/deobjects-editor` unpacks to JSON and packs back, byte for byte for all
  65 files (and every older version's), and leaves untouched objects byte-identical when others are
  edited. The writer (`engine/src/de/objects_model.*`) is shared with the engine.

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

### 5.3 New game modes: a story campaign with missions, choices, funds and upgrades
A design doc existed but was never used (to be reworked by the user, not copied verbatim). Beta players
also scripted a story-driven single-player "mission" system, with decisions that changed the outcome,
before the ship date cut it. Today single player is "unlock matches and play". The editor should be able
to author a **whole new mode** on top of the engine, not only new content for the existing ladder:
- **Campaign graph editor:** chapters → missions → outcomes, drawn as a node graph. Each mission is a
  match setup (opponents, robots, arena, rules, handicaps, drop-ons), plus objectives and win / lose /
  special-condition outcomes. Choice nodes branch the story. Consequences set career flags, funds,
  relationships, unlocks or the next mission.
- **Dialogue editor:** scenes made of lines with speaker, portrait, mood and optional voice. Player choices
  link to graph edges. It reuses the cinematic quote / speech-bubble presentation.
- **Economy and shop editor:** currency, prizes, fees and repair costs. Items and upgrades have a price,
  prerequisites, effects (robot stats, script constants within limits, pilot attributes, new moves or
  paint) and **graphics**: icons, shop art, descriptions. Includes a balancing sheet that simulates
  funds over a campaign path.
- **UI page editor:** new menu screens (shop, career status, mission briefing, choice screen) built from
  the game's own GUI controls (`OMFBGGUI`, FINDINGS 159), with a live preview.
- **Runtime the engine needs** (OpenBG-side work, not only data):
  - a persistent career save, stored like unlocks but richer;
  - natives to read and write career state;
  - a session master type for campaign missions;
  - the new UI pages;
  - applying upgrades to fighters at match start.
- **Original-game compatibility:** the original can't gain new natives. So either a campaign is limited
  to what existing natives and registry unlocks can express, or **campaign mode is OpenBG-only** (likely).
  The validator marks which mode a project targets.
- If any of the beta mission scripts survive, add an importer for them.

### 5.4 MCP support
The editor exposes its operations as an **MCP server**, so AI assistants and scripts can drive it:
- **Transport:** stdio (`openbg-editor --mcp`, headless, for agents) and an optional local-only socket
  while the GUI is open. In the GUI, AI edits appear live, go through the same undo stack, and are
  marked in the Output Log.
- **Tools:**
  - list and search objects;
  - read an object's details, references and "referenced by";
  - open and create add-ons;
  - copy a base object to the add-on;
  - get and set fields through the same schemas as the UI;
  - import an image or sound;
  - compile a script and report its errors;
  - validate;
  - save;
  - Launch Game, or a headless test run that returns screenshots and logs;
  - later, campaign-graph and economy operations.
- **Resources:** the data format docs, the schemas, FINDINGS-backed class descriptions, and the
  validation checklist.
- **Safety:** base data is read-only. Writes go only to the opened add-on. Nothing is written without
  an add-on chosen by the user (or named explicitly in a headless call). Each change is logged.

### 5.5 3D model import and export (glTF)
Status (2026-11-12): export (meshes, LOD switchers, props, arenas, robots with skin and
animations), prop, arena and robot (HAR) import are built, with the Import dialog (robots add Bones,
Animations and Hitboxes pages). See "Built" and "Built: robots" at the end of this section and
DEVELOPING.md. glTF 2.0 (`.gltf` + `.bin`, or `.glb`) is the exchange format:
Blender, Maya and 3ds Max read and write it, it carries skins and animations, and its
extensions take custom data (`extras`) for what DE needs beyond meshes.

**Export (any robot, arena or prop).**
- Mesh: each `Mesh\Standard` (and the three LODs of a `Mesh\LODSwitchingMesh` as `LOD0`-`LOD2`
  nodes, the switch distances in `extras`) with positions, normals and the per-face UVs split into
  per-vertex UVs (vertices are duplicated where faces disagree; the export records the original
  vertex index per glTF vertex so a round trip can merge them again).
- Materials: a `TextureProcess\MultiStage`'s stages (texture manager, blend, the 28 stage bytes) as a
  glTF material whose base colour is the first stage's image; every stage is kept in
  `extras.de_stages` so the import can rebuild it exactly.
- Textures: every `Image\Standard` as PNG (RGBA; a fighter's paint mask channels as they are).
- Skeleton: the fighter's 15-17 bone actors (Pelvis, Chest, Head, Left/RightUpperArm, ...,
  HeadAnchor) as joints with their rest transforms; skin weights from the `ActorDeformInfo` and the
  mesh's `Deform*VertexData` channels (up to 4 influences per vertex, normalised).
- Animations: each `AnimationSequenceGroup` sequence as a glTF animation named as in the bank
  (`Standing`, `Ground.Attack.LeftPunchLight`, ...), sampled per channel; markers in `extras`.
- Collision and gameplay for arenas: fighting surfaces, force affectors, death volumes, jump pads,
  spawn points and pickup spawners as empties / meshes under named collections (below).

**Import (robots).**
- Mesh: triangles to `Mesh\Standard` with an `ObjExtCon` payload; UVs back to per-face UVs and one
  `TextureManager` per face set; three LODs, either supplied (`LOD0`-`LOD2`) or generated by
  quadric decimation to the base robot's triangle budgets (~2,500 / 1,300 / 600).
- Materials: to `TextureProcess\MultiStage`; a material with `extras.de_stages` is rebuilt as it
  was, others get the fighter template (RGBMain, ReflectionMask, environment reflection).
- Rig: the target is the fixed rig the scripts name (bone actors, `HeadAnchor`, the hit and attack
  spheres' parents, the animation channel names). A **bone-mapping step** pairs the file's joints
  with those names (by name, then by a picked template such as Mixamo or Rigify, then by hand); an
  unmapped required bone is an error. Weights are quantised to the 0-255 per-vertex channels.
- Animations: to `AnimationSequenceGroup` sequences keeping the script's names (a script naming
  a sequence the bank lacks is a validation error); an animation not supplied reuses the base
  robot's (retargeted when the rest pose differs).
- Hitboxes: spheres can be authored as empties named `Sphere:<affector name>` parented to a bone,
  their scale the falloff; otherwise the base robot's spheres are kept on the mapped bones.
- Paint: the RGBMain image plus the paint mask (R, G, B: the pilot's three colours) and the
  ReflectionMask and Bump images; missing masks are generated blank.

**Import (arenas).** Collections tag what each mesh is: `Visual` (drawn), `Collision` (fighting
surfaces, by material: floor, wall, kill), `Death` (death volumes), `JumpPad` (force affectors with
an `extras.impulse`), `Spawn` and `Pickup` empties (to spawn points and pickup spawners referencing
SharedObjects). The navigation net is generated from the walkable collision (a grid over the
fighting surfaces, linked where a fighter can walk or jump, pruned to the format's limits) and can
be edited in the arena viewport afterwards.

**Validation of format limits.** Vertex and face counts per mesh (u16 indices), bones per vertex,
the bone count (17), texture sizes and formats the game accepts, the navigation net's link count,
names the scripts look up (bones, sequences, `DefaultCamera`, `Music`), and every reference the
new objects make resolving. Imports keep their source files in the project's sidecar folder so
they can be redone.

**Built (2026-11-11).**
- Scale: 10 DE units a metre (a robot is ~30 units tall, Grudge Match 770 across);
  `asset.extras.de_units_per_metre` makes a re-import exact. Space: DE is left-handed, so z is
  negated and triangles reversed; collision meshes keep the drawn winding (FINDINGS 543).
- Export round trip: an exported mesh imports back with the same vertex order, faces, adjacency,
  smoothing groups, mesh flags, MultiStage stages (version, mode, colour, stacked, flags), per-stage
  UVs and pixels (`_DE_VERTEX`, `de_faces`, `de_face_flags`, `de_stages`; ctest `editor-gltf-import`).
- New objects are written in the shipped layouts (FINDINGS 540-542): images RGB (mask 7, format 20)
  or RGBA (mask 15, format 0) at power-of-two sizes up to 1024; TextureManagers with their UV count;
  mesh affectors with no stored octree and a cell size, so the game builds one.
- Arena import copies the environment of a template arena (lights, sky, music, cameras, values,
  sounds; its geometry and gameplay objects are left out) and the gameplay templates from Grudge
  Match (spawn point, pickup spawner, death box) and Desert (fighting-surface affector, jump pad,
  nav area and link); the nav net is a grid over the floors, linked where a robot can walk (no wall,
  no step over 12 units, no hole), at most 64 areas and 256 links (DesertRings ships 245 links).
- Verified: a generated box arena and the Desert exported and re-imported play in the editor and in
  `openbg-fight` (fighters stand on the floors, walls hold them, the AI walks the generated net to
  its opponent); an imported prop placed in the arena draws in the viewport and in the game.
- Not yet: props have no collision; jump pad strength is the shared launcher's (only the direction follows `extras.impulse`);
  an exported arena's death volume is written only when it is a box or mesh (Desert's is a passive
  reference); vertex colours are exported for neither direction.

**Built: robots (2026-11-11, `engine/editor/core/har_import.h`, `editor/app/robot_import.cpp`).**
- What it makes: a HAR variant of the chosen base robot (`create_har_variant`: its scripts, inputs,
  sounds and spheres) whose model is a copy of the base's default `FighterModel` rewritten in place:
  each LOD's `Mesh\Standard` geometry, the fighter material's per-face UVs (RGBMain / ReflectionMask
  texture managers; the environment reflection takes none), one `Deform<Bone>VertexData` channel per
  skin bone (weights summed over the file's up to 4 influences, unmapped joints' weight given to their
  nearest mapped ancestor, quantised to bytes), the paint mask / reflection / bump images, and an
  `RGBStatic` image with the model's own colours (added unpainted by the fighter filter, FINDINGS
  165). The bank's channels and sequences, the deform info's binds, the bone actors' rest and the
  spheres are rewritten in the copy. The description and its first paint job name the new model; the
  base's other paint jobs are unlinked (FINDINGS 560).
- Bone mapping: rows are the base skin's bones (required: an error blocks the import) plus
  `HeadAnchor` and `Body` (optional, exact name). Exact or normalised names first (`mixamorig:`,
  `DEF-`, `Bip01` prefixes; `Left` / `L_` / `.L` / `_l` sides), then a preset (Auto picks the one that
  matches most: Mixamo, Blender Rigify, Unreal, 3ds Max Biped, then Generic), then the skeleton's
  shape (the pelvis joins the two lowest joints, the chest the two widest, each limb's two longest
  consecutive segments are its upper and lower bones, the head the last weighted joint towards the
  top). Each row shows how it was found and a confidence; picks by hand are kept by "Map again" and
  saved (with the base robot, preset, facing and paint) as `mapping.json` beside the kept source, so
  a re-import of the same name restores them.
- Orientation: the file's left (left minus right upper arm) and forward (the feet's vertices ahead
  of the ankles, else the toes) against the base's; a different handedness is mirrored (x negated:
  glTF characters are the mirror image of the game's convention, FINDINGS 561), then turned in 90
  degree steps; the lowest vertex goes to the floor. Exports of ours are taken as they are.
- Retargeting: each skin bone's bind frame is the base's turned onto the new one (pelvis and chest by
  two directions; limbs by their direction and the hinge they bend about, measured from the joints
  when bent, else from the facing, so the base's elbows and knees bend the new ones the same way
  round; hands and feet to their vertices, feet level; the head as modelled; FINDINGS 670); every
  sequence keeps the base's rotations, and each channel's offset is scaled by its bone's change of
  length (others by the overall size). Contacts are then corrected by two-bone IK about the hinge on
  samples of each retargeted sequence (FINDINGS 671): where the base's foot is down, the new one
  stays on its floor and moves as the base's does (no sliding against the game's speeds); where a
  base hand or foot holds `ThrowFollowTarget` in a throw, the new one reaches the same place on it
  and the target follows the limb, so the victim hangs from the new hand; `HeadAnchor` stays on the
  new head. A file animation named as a sequence (Blender's
  `Armature|` prefix ignored, or chosen on the Animations page) replaces it: sampled at 30 keys a
  second and normalised to 0..1 (`extras.de_normalised` exports are 0..1 already), each mapped bone
  following its joint; the root's horizontal drift is removed unless the file has a `Body` node.
  Every sequence the scripts name (`seq<Name>` globals) is kept, so none can go missing.
- LODs: nodes named `LOD0`-`LOD2` (or ending so), else every mesh is LOD0; missing LODs are quadric
  decimations to the base's triangle counts; LOD0 is decimated to fit 65,535 vertices (or to the
  base's budget when asked). Meshes parented to a joint without a skin follow that joint.
- Materials: an export's `extras.de_stages` give the RGBMain and ReflectionMask images and each
  stage's UV set back; otherwise the materials' textures and colours go into one atlas, used as the
  model's own colours (RGBStatic, paint mask blank) or, for untextured files by default, as a paint
  mask (each material one of the pilot's three colours). Images named `PaintMask`, `ReflectionMask`
  or `Bump` (or material extras `de_paint_mask`, `de_reflection_mask`, `de_bump`) are used as such.
- Hitboxes: an empty `Sphere:<affector>` under a joint moves that sphere to the joint's bone (its
  scale the falloff, in metres; the second of a name the second sphere, Blender's `.001` ignored);
  further ones named `GetHitAffector...` or after an attack prefix the fighter scripts gather
  (`IsNamePrefix`, FINDINGS 672) are new copies of such a base sphere; other names are left out with
  the reason on the Hitboxes page. The base's other spheres keep their bone, offsets scaled with it.
- Further parts (Gargoyle's wings, FINDINGS 673): their bones are optional rows on the Bones page.
  Mapped, the file's vertices those joints carry (and their unmapped children's) become the part's
  mesh (UVs in the body's atlas, its images the body's), its binds and bone actors go where the
  joints are and its bank's offsets scale per bone; unmapped, the base's part is moved and scaled
  with the bone it hangs from.
- Paint jobs: further paint jobs come from images laid out as the model's texture (Source > Paint >
  Add from an image, `--import-paint Name=file.png`): copies of the model with that image as its
  colours (or paint mask). In the HAR editor, Paint lists the paint jobs, previews the selected one,
  imports or exports each image (RGBStatic, RGBMain, ReflectionMask, Bump) and makes new ones (a copy
  of the selected one's model, base or add-on).
- Choose Creature: the import names the robot in its panel (a copy of the base's ControlCreator with
  title, text and picture); the HAR editor's General tab edits them and imports the picture from a
  PNG or renders it from the model (four poses of the selected paint job).
- Game side: a description's robot is set up from the robot object itself and its own bank, inputs,
  class and deform info (the nearest in an add-on holding several, `de::robot_part`), so an add-on's
  HAR variant fights as itself in `openbg`, `openbg-fight` and Play in editor; renderers draw
  `RGBStatic`.
- Verified (ctest `editor-har-import`): Jaguar exported and imported onto Jaguar comes back with the
  same vertices, binds, channel rest and, posed in every 7th sequence, within 0.008 units of the
  base; a generated humanoid with numbered joint names and other proportions maps by shape alone and a
  Mixamo-named rig (centimetre armature) by preset; in every sequence their limbs point within a
  degree of the base's and no edge stretches more than the base's does; the add-on validates, saves
  and the engine reads its skin, bank and materials back. Both fight in `openbg-fight`: hits land
  both ways.
- Verified (2026-11-12): planted base feet keep the new ones still (4 of 1,533 and 7 of 1,863 samples move) and on
  the floor except out of reach (4-7%: thrown, falling, legs at full stretch); Jaguar's overhead throw
  keeps the grip within 0.15 units; a second paint job, a new body and attack sphere (the trace shows
  the scripts gathering it), the Choose Creature panel and Gargoyle with and without wing joints
  import, save and read back. Characters made in Blender (`engine/tests/scripts/make_character.py`:
  8,400 triangles, a texture atlas or five materials with external PNGs, Mixamo-named T-pose or
  Rigify DEF bones in an A-pose with rolls and twist bones, Idle / Walk / Punch) import onto Jaguar,
  Katana and Pyros, show textured in the HAR editor, in Choose Creature (picture, title, text, paint
  jobs) and in `openbg-fight` (`--model` picks a paint job).

## 6. Prerequisites and risks
1. **Lossless writing.** Fix the 49 of 65 files whose JSON round trip loses 2-32 bytes, or have the
   editor write at the object level through the engine's writer. Writing must be byte-exact for
   untouched objects.
2. **Schemas.** Add the pilot, fighter-stats, input-sequence, behaviour-globals and
   arena-description chunk layouts (their readers already exist in the engine) to the writer, with
   round-trip tests.
3. **Script output.** Every Class the editor writes carries VMC2 from `vm::compile_class` +
   `VMClass::write()`, with change information that matches the `value84` of the behaviors it
   attaches (FINDINGS 431). Without it, or with a stale one, the original compiles the source at load
   and a compile error quits the game (FINDINGS 436). Settled; see §7.
4. **Override semantics.** Settled: a GUID clash is not an override in the original (references go to
   the later file, description lookups to the earlier one, lists show both; FINDINGS 433), so add-ons
   only add objects with fresh GUIDs. Copied objects keep their `value84`, EX flags and unknown
   fields byte for byte; new objects get a `value84` of their own, and a script's VMC2 is rebuilt
   whenever a behavior it attaches changes `value84`.
5. **Arena consistency.** Navigation, spawns and collision groups must stay consistent, or the AI
   breaks.
6. **Importer scope.** glTF model, skin and animation import against the fixed rig is the largest
   single item.

## 7. Original-game compatibility rules

Established from the 2.123 modules (Core etc.; we do not run or analyse the protected exe), FINDINGS
430-437. A test add-on built to these rules loads in OpenBG; the user checks it on a real 2.123 install.

**Loading.** Every `*.DEObjects` in `<install>\Data` is an agent, in directory order, with no list,
index or registry entry to update. All headers are read before any object is built and objects are
created on demand, so an add-on may reference objects in any other file (SharedObjects, Pilots…)
whatever its name. A reference to a GUID no loaded file has is silently null.

**GUID clashes.** Both objects load and nothing reports it. A cross-file reference resolves to the
later file's object (agents are searched last to first), but description lookups by GUID go to the
earlier file's, and the selector lists show both. So an add-on **adds** objects; it never reuses a
base GUID to replace something.

**Scripts.** With VMC2 present and current the original runs it as is. "Current" means: chunk
version 13; the change information equals, for each attached behavior, its `value84` and then the
`value84` of each behavior it imports; every class name exists in 2.123 with the same layout
position. Otherwise the original compiles the Behavior sources at load, raises a notice, and **quits
on any compile error**.

**Validator checklist** (every item is an error unless marked):
1. Signature `Diversions Entertainment Objects File2`, format byte **2**, agent EX table count 0.
2. Header chunk v9; agent info v4 with class `TransferAgent\Win\File`, a fresh non-zero GUID, eager
   flag 0, flags 0; agent contents v1 with the GUID pool listing every external GUID referenced.
3. Agent name non-empty, unique, and neither a prefix of nor prefixed by another agent's name, the
   registry's (`DiversionsRegistry`, `PhoenixRegistry`), `OMFBG`, `Pickups`, `SharedObjects` or
   `COREMeshes` (`FindAgent` matches by prefix).
4. Every object info v4, a class path the 2.123 factories know, a fresh non-zero GUID unique across
   the add-on and all 2.123 files, eager flag 0, flags bit 1 only when the contents are compressed.
5. Compression method 0, 6 or 7 (7 with a correct MD5); never 5 or 8+.
6. Local references below the object count, pool references below the pool size, and every pool
   GUID present in the 2.123 data or the add-on (warning: a missing one loads as null).
7. Hierarchy children are local objects only.
8. Passive references (description resources, photos) carry the right agent GUID (the add-on's
   own for its objects) and object GUID.
9. Every Class has a VMC2 EX entry built by `vm::compile_class` against 2.123 plus the add-on, whose
   change information matches the current `value84` of the behaviors it attaches and their imports,
   and whose class names and layout positions are 2.123's. Rebuild it on every behavior change.
10. Every Behavior's source also compiles without error with our compiler (a stand-in for the
    original's, which would otherwise quit the game).
    Validation lists each error at its line; selecting it opens the tournament's script tab, or for
    any other add-on behavior (a HAR variant's fighter script) its source in the object view, at that
    line with the error marked.
11. Descriptions meant to be playable: `listed` set, `locked_by_default` clear (locks are keyed by
    GUID in the registry, so a new GUID starts in its default state), and in the casts the screen
    gathers (`PilotSPLadder`, `MultiplayerPilot`, …).
12. Warning: list order follows file names (later names come first in the original's lists).

## 8. Decisions and open questions
Decided (2026-11-06):
- **Both engines** (deferred 2026-11-06: no machine to test the original on yet; OpenBG first, the
  compatibility checks stay in the validator as warnings). Add-ons must load and work in OpenBG **and** the original 2.123 game, so the editor
  writes VMC2 bytecode for every script it touches and enforces the original's loader rules
  (§6 items 3-4; the rules and the validator checklist are in §7).
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

## 9. Milestones
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

Progress (2026-11-06, first editor iteration, `openbg-editor`; DEVELOPING.md "The content editor"):
- **1 (done for pilots):** the writer is lossless on all 65 files; `engine/editor/core` builds add-ons
  (deep copy with fresh GUIDs, references remapped to the copies or the GUID pool) and decodes the
  pilot, pilot description (with its cached stats, name and colours) and string value layouts.
- **2 (partly):** the docking shell with menus, document tabs, toolbar, Place, Viewport, Outliner,
  Details, Content Browser (add-on and read-only base game), Output Log, Validation, Script Console
  (editor commands), status bar, saved per-user layout, undo / redo, and Launch Game
  (`openbg --addon`). Not yet: arena viewports (the viewport is an empty grid), Play / Simulate.
- **2, browsing (2026-11-07):** no start screen: once `Data` is chosen the full layout shows with no
  add-on open (an add-on is only opened or created when the user chooses; actions that need one ask
  which). The Content Browser holds the whole base game (28,337 objects, 65 files) by type (16
  types) or by file and folder, with lazy thumbnails, BASE / NEW badges, a type filter and search
  over everything, virtualised tiles or list. Details decodes any object as far as the editor can
  (identity, description / pilot / image / value / sound / behavior fields, hierarchy, references
  through known layouts plus a best-effort grammar that explains 94% of objects, a background
  "referenced by" index, raw chunks). Viewer tabs preview images (channels), scripts (syntax
  coloured), sounds (play), HARs (animated robot) and base pilots (the pilot editor, read-only, with
  Copy to Add-on). Not yet: mesh and music previews; editing types other than pilots.
- **3 (done):** new pilot -> edit -> photo import -> save -> reopen, and the pilot shows in the game's
  Choose Pilot with its photo, stats, colours and bio. Validation runs §7 rules 1-8, 11 and 12, the
  original-only ones as warnings; rules 9-10 wait for script editing.
- **2 and 6 (2026-11-11, world side):** arena viewports with the game's renderer (1/2/4 panes,
  perspective / top / front / side, lit / unlit / collision, Show menu overlays, Outliner tree,
  picking, move / rotate gizmo with snapping); play in editor (Play, Simulate, Pause, Step, speed,
  Eject, Live Inspector, debug toggles) in the selected viewport; arena variants (description,
  lights, spawn points, pickup spawners; arena checks) that play in the editor and in the game.
  Not yet: prefabs from other arenas, placing hazards / surfaces / nav links / lights, a tournament
  start in Play Mode, the network test, live light edits during play.
- **4 (done, 2026-11-11):** Content > New Tournament copies a base tournament (its description, session
  master, sub-session handler, BMaster / BSubSession and Choose Match panel; fresh GUIDs) or starts a blank
  ladder. The tournament editor (tree, match form, generated scripts with `// <user ...>` blocks kept,
  Quotes) generates both scripts in the shipped patterns, compiles them into VMC2 with change information
  that matches the behaviors' revisions, and "Play from match" launches the game at a chosen match
  (`openbg --start-match`). The new tournament shows in Choose Match and plays through its matches.
  Validation runs rules 9-10 (current VMC2, compile errors with line links). The compiler gained the
  value-class instances, local arrays and state tables a session master needs (FINDINGS 520).
- **5 (mostly done, 2026-11-11):** Content > New HAR Variant copies a robot's fighter file, description
  and paint-job descriptions with fresh GUIDs (references of unknown layouts rewritten through the
  reference grammar); the HAR editor edits movement and jump stats, input sequences (direction pad,
  buttons, timing), sphere sizes (drawn on the preview's bones), sound swaps, new paint jobs (a copied
  model whose images take PNG/BMP imports) and the behavior's numeric finals. The variant shows in Choose
  Creature and fights as its own robot (the game builds it from the variant's file). A changed
  constant recompiles the fighter's classes (the compiler takes the fighter scripts' `master`,
  inherited elements and sequence names, FINDINGS 522-527) and the game uses the new value. The
  preview shows the paint job selected on the Paint tab (2026-11-12).
- **7 (mostly done, 2026-11-11):** glTF export of meshes, props, arenas and robots; prop, arena and
  robot (HAR) import through the Import dialog (§5.5 "Built", "Built: robots"): bone mapping with
  presets, guesses and a mapping UI, retargeted sequences, file animations, decimated LODs, hitboxes;
  the imported HAR opens in the HAR editor and fights in the game. Robots (2026-11-12): twist and
  contact correction in retargeting, several paint jobs (and the HAR preview shows the selected one),
  the Choose Creature panel, new hitboxes, Gargoyle's wings. Not yet: file animations for the wings,
  a walk speed that follows a robot's leg length.
