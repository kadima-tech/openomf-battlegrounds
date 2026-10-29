# Placeholders and approximations

Everything in the engine that is **not** a faithful port of the original. These are stand-ins,
shortcuts or fixed answers, kept here so none of them is mistaken for finished work. Remove an
entry when the real behaviour lands, and say in `re/FINDINGS.md` what replaced it.

Kinds:
- **Stand-in**: our own logic doing a job the original does differently.
- **Approximation**: the original's logic, with a detail simplified or inferred.
- **Fixed answer**: a native that always returns the same value.
- **Stub**: a native that is accepted and does nothing.
- **Missing**: not implemented at all. Calls are counted in the "natives still missing" report.

Every entry says what finishing it waits on: a later step of the port, or a large piece of
reverse engineering of its own. Small, self-contained fixes are not left here.

## Waiting on the front end (menus, options, single player)

The launcher's own part is known (FINDINGS 124), and so are the UI managers' page flow, controls and single-player screens (FINDINGS 125–127). `openbg` runs the front end: the main menu and the Quit page are ported, on the GUI scene in OMFBGGUI with its own scripts (buttons, doors). The rest of the front end is below.

| What | Kind | Notes |
|---|---|---|
| Launcher GUI wiring (`InitGUI`) | Approximation | Not readable. The app manager's main frame, transition and backdrop are `V.MainFrame`, `V.MainDoors` and `V.Backdrop` by their names; the UI parent is taken as the frame-control root actor (x 0..1 across the screen, y 0..0.75 up, a 60° camera looking down −Z), which the data's layout and the 4/3 in `SetupControlArea` agree with (FINDINGS 125, 126). |
| GUI scene | Approximation | One `World` scene holds the GUI and the game: GUI roots are flagged (`GameObject::gui`) and drawn in their own pass with the GUI camera and lights. Scripts in the GUI see the game's `Scene()` singleton (they only use its clock, ambient sounds and render-tick hooks). |
| Controls | Stand-in | Buttons are hit rectangles at the `PositionControl` rectangles with their labels drawn by the overlay, not `DE_CControl3d` meshes in a frame-control scene. Label colour is white (the colour every screen passes to `NewButtonControl`; the launcher's `ColorButtonText` override is not readable). The GUI is drawn 4:3, pillar- or letterboxed. |
| Notifications (`NewUserInformation`) | Stand-in | Our own panel (title, wrapped text, OK; Enter, Esc or a click closes it), not `DE_CDialog3d::UserInformation`'s dialog. |
| Registry | Stand-in | Values the menus keep (`SinglePlayerPlayed`, the main menu's last choice) live for the run only. |
| Screens | Missing | Options, Help, Replays, Credits, Join (session browser) and the session chooser behind Single Player and Host (`MenuSelect`) say "not ported yet". The in-game menu, description selectors, choose-fighter and rules display are not ported. |
| Main menu music | Missing | `GH_MUSIC_PLAYER_ASSURE_DEFAULT_MUSIC` to the music player is not sent. |
| Robot, pilot, arena and score-limit choices | Stand-in | Picked up front by `openbg-fight`, the way `BSinglePlayerSubSession` does, instead of through menus. |
| Factions (teams' pilots) | Approximation | Ported for what Last Man Standing does (FINDINGS 112): a pilot's faction is null unless the picker sets one in a team game; `CPilot::Faction`/`SetFaction` and `CFaction::Photo` are ported. Team game modes (and with them `CFaction::TeamIndex`, faction vtable +0x180, and the session's team lists) are not run or decoded. |
| UI managers (description selectors, choose-fighter UI) | Stand-in | A launched UI with a preset choice completes on the next update; without one it never completes. `IsUIManagerActive`/`IsActiveLocally`/`IsDisplayed` return false. |
| Loading screen | Stand-in | `update_session` imitates the engine's loading screen (`SessionQuerryIsLoadingRequired` → `SessionLoadElements`, once per element). The launcher only fetches the splash image (`GH_GET_SPLASH_SCREEN_OBJECT`) and stamps its build on it (FINDINGS 124); the loading itself is the session's. |
| Rules display | Stand-in | Created, shown nowhere, and authorised to continue at once. |
| `CSession::EndAndDie` | Stub | The match ends in the script's EndingSession state and stays there: nothing to return to. |
| `CImpulseRobot` (hotkeys), `GameDialog`, `SubSessionOwner` | Stub / fixed answer | No hotkeys, dialogs or sub-sessions until the front end and single player exist. |
| `CDescription::IsMemberOf` | Fixed answer | Always true; only menus filter descriptions by cast. |
| AI difficulty | Approximation | The original reads it from the registry (default 0, Rookie, which we use); `--difficulty` stands in for the options screen. |

## Waiting on networking (out of scope unless LAN play is wanted)

| What | Kind | Notes |
|---|---|---|
| Session status machine | Stand-in | Only a local, server-side session: every client is local, nothing is replicated. `IsServer`, `IsLocal`, `IsReadyToLoad`, `IsFullySequenced`, `AreMasterRequirementsSatisfied`, `IsStateAlterable` return true; `IsReplaying`, `IsNoReplicate` false. |
| `RegisterState`/`RegisterUrgentState`, `ForceClientsToLoad`, `LoadingComplete`, `Register*State` | Stub | Replication hooks. |

## Waiting on rendering (materials, lights, visibility)

| What | Kind | Notes |
|---|---|---|
| Paint model ("Apply") | Inferred | The fighter's `ImageFilter\OMF2Fighter` paints the model images named `RGBMain` (pilot colours) and `ReflectionMask` (the colours lightened); which images it applies to is taken from those names (the data has no filter link on the images), plus the images a script gives the filter (`CImage::AddFilter`: Mantis's bugs, painted with the fighter's pilot colours when first drawn). Its runtime static/scaled image lists (damage overlays) are not filled. A pilot's colour 0 override (+0x32c) is ignored. |
| Rendering | Approximation | Materials are drawn as the engine builds them: stage passes and blends, per-corner stage colours, constant colour values, environment and per-stage cube reflections, bump-bent reflections, the glow layer (GlowFactor and specular passes through the `HighRangeSpecular` cube, the 256² glow target, super-sampling, 4×4 reductions and the 8-tap composite). Not ported: shadow maps (`DE_CShadowCaster`: casters, mesh flag 0x40, rendered from the intensity-weighted average of the static lights and projected onto shadow targets, mesh flag 0x20, with `ShadowLayer` / `DarkMap`). They run only when the renderer's shadow option is on (`Preferences\Renderer\Shadow Type`, renderer +0xa8); the gameplay footage shows no robot shadows, so the default is taken to be off and the port is left for when there is an options screen; the D3D fixed-function bump matrix path (the pixel-shader path is used; its bump vector is taken in world X/Y), mesh normal elements (normals are smoothed face normals). The glow layer's super-sampling tap angles are inferred; the glow target has no occluders, as in the original. |
| Scene lights | Approximation | `Light` objects at their sphere/cone affectors: D3D-style vertex lighting (ambient from the environment, constant-attenuation point lights scaled by the affector's intensity at the actor) on vertex-lit meshes, projector passes on projector-lit ones (cone projectors as FINDINGS 105). Ported (FINDINGS 107, 110): the static-light rule; the reach test against the lightable render affectors (group 0x20) and their owners; camera culling, with no facing test; each cone's own image, with its blend (scorch decals, the target marker, darkening lights). Checked against gameplay footage in City, FirePit, Desert and the sand arena. Still inferred: (1) a projector pass draws a whole batch that overlaps the light's reach, not just the polygons in octree cells it touches; the projection already gives nothing outside the light, so only cells at the very edge could differ. (2) The alpha-blended image projectors take their alpha as the image's alpha × the attenuation ramp (the stages' alpha arguments are read from `SetStates`, not traced through `CStage::SetStates`). (3) Meshes that take no owner's lights (pickups, projectiles) assume the owner rule covers every render actor. |
| HUD and `IsRendered` visibility | Stand-in | A fighter's HUD (and `IsRendered`) shows only for the fighter in focus; the original decides by what the renderer's camera sees. |
| World-space text | Ported, untested | Primitives on a mesh that is not screen space are drawn in the 3D scene at their actor (FINDINGS 116): the same vertices in its local frame, overlays without the depth test. No shipped fight shows one with text (the crowd-response texts stay empty until the crowd reacts), so how it looks is unchecked; status bars are drawn only in screen space. |
| Status bar end cap | Approximation | Body plus cap as in `CStatusBarPrimitive::UpdateBar`; the cap's per-vertex fade needs coloured vertices in the overlay. |
| Camera | Approximation | The game's fight camera drives the view (FOV 70.5 degrees horizontal at 4:3, the vertical angle kept on wider screens), its rumble included; `Session().Camera()` and the observer/replay cameras are not used. |

## Effects, sound and music

| What | Kind | Notes |
|---|---|---|
| Music | Ported (synth port untested against the original's output) | Music plays. The game side runs Music.DEObjects' `MusicPlayer` actor and its `BMusicPlayer` behaviour, the `CSpeaker` natives, the session's moods and the mood jumps between segments (`game/music.h`). `app/music_player.cpp` renders the segments with GothicKit dmusic (`third_party/dmusic`), resolving their style and DLS references against the song's `DirectMusicContainer`. Timing and level follow AudioPlayer (FINDINGS 120): the almost-end notification comes DirectMusic's default prepare time (1000 ms; the game never sets it) before a segment and its repeats end, the next segment is played with `DMUS_SEGF_SEGMENTEND` so it starts at the end, and the music plays at −7.6 dB (the speaker volume curve with the global music volume's default of 0.6). dmusic's synthesizer has been replaced by a C port of Microsoft's DirectMusic software synthesizer (the WDK DDKSynth sample, MIT): its envelopes, LFOs, control-point ramps, curves, voice priorities and stealing, DLS1 articulation and 22050 Hz mix; we then resample linearly to the device rate. Compared with the soundtrack MP3s (omf.niraj.ws; Crystal Cold and Where the Mind Is, FINDINGS 122): tempo matches exactly and tuning is within 5 cents; we have 6-15 dB less treble above 1 kHz, which comes from the game's instrument samples (mostly 11 or 22 kHz) and the 22050 Hz synth, as the MP3s are studio renders rather than recordings of the game. Their mastering (peaks over full scale) also makes them no guide to the in-game level. The style/segment player (dmusic's own) still ignores some chunk fields. `OPENBG_DEBUG_MUSIC=1` traces it. |
| Sound | Ported (pan law approximated) | Each sound is a speaker sphere updated every frame as `DE_CDirectXSpeakerElement::Update` does (FINDINGS 121): the sphere's intensity, the global SFX volume (1.0) and the speaker's volume, the update-rate accumulator, the 10000 log10(1 − (1 − v s)²) buffer volume, starting only within the falloff. The music is at −7.6 dB (global music volume 0.6). The original has no Doppler, occlusion or voice limit, and neither do we. Two parts are assumptions. (1) Direction comes from the head-relative DirectSound3D position (with the front mirror) through an equal-power pan; the original's pan law was the sound card's own 3D (or none: without hardware 3D buffers it played every sound centred). (2) The noise length behind the update rate is taken as milliseconds. The near-field pull toward the centre is not applied, because a sphere speaker's distance ratio stays at 1. Volumetric sounds play as spheres, and looping noises (on above 0.09, off below 0.03) don't occur in the scripts' sound calls. |
| Particle modifiers | Ported | `Rotator`, `OriginMagnet`, `PushDownZModifier`, `MiscellaneousNewParticleFunctions`, `BeamSpawner`, `LighteningSpawner`, `AutoExternalSpawner` (FINDINGS 114). The lightning midpoint jitter on all three axes is confirmed from the machine code (three `CenterRand` calls). The 0x78..0x94 lightning fields are read but unused (zero in every shipped spawner). |
| Particle renderers | Approximation | `StreamParticleRenderer` and `PointStreamer` trails are built in screen space as the original does (FINDINGS 116): projected points, pixel-space perpendiculars, half-width over depth, stream joints averaged. Ours works on the projection plane and puts the vertices back in world space at each point's depth, which is the same picture for square pixels. All stream texture modes (0, 2, 4, with the time scroll) and colour modes (0-5) in the data are decoded; the trails' blend is confirmed as the particle renderers' material mapping. `FourFaceTriangle` (clouds, fog) is ported; its camera-facing modes 1-3 are inferred (the shipped ones use 0 and 2). |
| Billboard details | Approximation | Streak modes 1–5 are drawn as quads from the particle along the direction; modes 14–23 (rare) fall back to mode 0; the z offset and the light-cone fades (cone modes 1/2) are ignored; cone mode 3 scales by the glow value at 0. "Coth Type" curves are linear. |
| Ring renderer | Approximation | Annulus of 40 segments from the decoded edges and colours; the `+0xd4/+0xd8/+0xdc` flags and the detail value are not used, and plane modes are inferred from the 90-degree turns in `Process`. |
| Particle stepping | Stand-in | Effects step every game frame for every visible actor in the scene; the original steps an instance when its element is drawn (so culled effects pause). Particles are not depth-sorted. |
| Attack trailer image | Approximation | Additive ribbon with the fighter's `AttackTrailer` image; the point's life in `Process` is read as 1 − age × decay (the decompile drops the subtraction), with creature time in ticks. |
| Arena path animation | Approximation | Ported (`AnimationStreamPlayer`, FINDINGS 99): paths place City's trains, SpaceStation's cruiser and tazers, FirePit's target drones and every intro camera. Arena actors a path moves leave the baked arena mesh and are drawn live, without their glow-layer passes. |
| Animated images (`Image\AVI`) | Approximation (colour) | `CImage::SetPhase` picks the video frame (`de/avi.h`). Every shipped video decodes: uncompressed DIB, Cinepak, and Indeo 4/5 through openindeo (`openindeo/` next to `battlegrounds/`, our own MIT decoder; all 18 Indeo files bit-exact against FFmpeg's decoder). Inferred: the YUV → RGB step. Intel's codec converted to RGB for the game itself; we use BT.601 studio range, each chroma sample covering its 4×4 pixels (no chroma interpolation). openindeo's tables are FFmpeg's for now, to be diffed against Intel's DLLs. Procedural images ignore the phase (no script animates one). |
| Observer camera | Approximation | Its natives are ported (FINDINGS 112): the controller list, the analog axes, `CSimplePhysics::DeltaTick`. Not ported: its command executors (`MouseAxisNavigation`, `DigitalOrientationPropulsion`, `Teleport`), which are its autopilot, and binding its mouse controller to the mouse. So `CCreature::DeltaTick` has nothing to run and the camera holds still. The fights never show it (only spectating does). |
| Hit sparks | Approximation | Ported (FINDINGS 103). The sparks move once a frame by the frame's time; the original's spark process is a temporal at the fighter's rate (+0x1dc, 16 ms), so its steps are finer. With `--direct` there are no sparks: the fighter's `SparkEffect` template comes from a passive reference that only the session path prepares. |
| Gargoyle's wings | Approximation | The second rig is drawn (FINDINGS 104): skinned under the body's Chest pose by `WingAnimationManager`'s sequence, time and prelude. `WingBase`'s own transform is taken as identity (as in the data), and a prelude blends from the wing pose last drawn, like the body's. |

## Waiting on large reverse-engineering pieces

| What | Kind | Notes |
|---|---|---|
| Command manager | Ported | `World::run_commands` (FINDINGS 98, 106): the command list, sort, per-type assignment, primary/alternate arbitration and counters, once per creature update. Look-at offsets are always zero, as in the original (`DE_CLookAtCommand::SetOffset` has no caller). |
| Creature needs | Approximation | Ported (`needs.cpp`, FINDINGS 106): the 100000-unit test sphere at the focal point over group 0x800, each touched affector's Need, walked from the last; fighters have no team (TeamIndex -1). The touched affectors come in the scene's list order (FINDINGS 117). |
| Interface calls that switch state | Approximation (mechanism only) | Op 0x22 (`ContinueExecutionInternal`, FINDINGS 118) runs a state called from an instant method by tearing down the instance's main executer and setting the state, and runs an instant method called from a state on the instance's second executer (+0x8c), abandoning the calling state if it switched. Our VM runs both on one stack and stops the same way (the restart and the `state_switched_` flag), with the same outcome; it keeps no second executer. |
| Force details | Ported | FINDINGS 117, 118: floor-event recipients are `DE_CPCodeEventRelay`s that call their method with no argument (the null event is the original's); each material with a threshold has its own incident recorder (`AddIncidentIfAbsent` per touched owner, inclusive); the simple physics' own force, the particle material's created-actor orientation and face extent, `CCollisionResults::ApplyForce`; surface-change recipients are never called, as in the original. |
| Frame and update timing | Ported | `DE_CScene::PassTime` (FINDINGS 117, 118): the scene clock is real milliseconds, a frame passing at least 20 ms (headless and screenshot runs exactly 20); each rate's temporal set ticks on its multiples in time order, newest element first; creatures join as the scene prepares them (pre-order, last child first), executors once their entry has run, and every fighter's input manager (only the one bound to a device records); delta-tick and render-tick hooks, the session update and the effects run once a frame. Objects reaching the scene outside `enter_scene` are found by a scan before the next tick. |
| Who controls a slot | Ported | `OwnCreature` adds the creature to the scene, then `SetAsUserControlled(device)` binds the user's client's device to its controllers (FINDINGS 118); a bound `JoystickInputManager` records the user's input, the others none. The view follows the fighter's `CameraActor`, as `DE_CCreature::Camera(0)` gives the scene viewer. |
| Fighter creation (`CFighterDescription::NewResource`) | Ported | FINDINGS 117, 118: a fighter is a copy of its file's robot (two of the same robot can fight), with its own executors, its controller from its object reference, its pilot under it, and a slot modifier's components attached as creature components; descriptions apply their replacements and components. `--direct` runs give the fighter the `PFighterStats` executor as a component (the session attaches its stats creature). |
| Scene membership | Ported | `scene_roots` plus `enter_scene`, which prepares a branch the way `DE_CScene::Add` does (its affectors, temporals and executors in the original's order) before running any entry; parenting into the scene adds the branch then. Passive references follow `DE_CPassiveActorReference::Create` (placement 0 under the reference's parent, 1 parentless and in the scene only with +0x120; +0x124 deactivates; mode 2/3 hand over their children). Inactive and detail-hidden children are parked out of the scene with their branches, and `Activate` adds them back and prepares them (FINDINGS 119). |
| Teams (`IsEnemy`, enemy list) | Approximation | `IsEnemy`, `NumEnemies`/`Enemy`, `GlobalInterest` and `CurrentTarget` are ported (FINDINGS 100, 106, 111). A fighter's team (+0x49c) comes from its pilot's faction's `TeamIndex`, and factions exist only in team games, which are not run; so no two fighters are allies, as in Last Man Standing. |
| Collision result order | Ported | The scene's affector list (FINDINGS 117, 118): `DE_CScene::Add` adds a branch in post-order, last child first, also when an actor is parented into the scene; removal keeps the others' order. |
| Creature components (`AddComponent`) | Ported | The component becomes the creature's child and component (`Interface()` reaches its brains). Its creation-time alignment (+0x1d8, the owner's rounded down to its interval) only gates its first update, which no aligned time delays (both are at or before the present tick), so it is not modelled. |
| Scene tests | Approximation | `TestSphere`, `TestSpherePath`, `TestCone` and `TestLineSegment` are ported (FINDINGS 102, 108, 117). A cone with a penumbra touches spheres only, as in the original. A line segment reports every mesh face it crosses, in order along it; the original takes them in octree-cell order and counts a face only when crossed from one side (its face frame's sign convention is not decoded), so ours also counts faces crossed from behind. |
| Arena surface hits | Ported | `ProcessEvent` hands the event to the object's relays, as `DE_CActor::ProcessEvent` does with its properties (FINDINGS 116). No handler in the DLLs or scripts reacts to the surface-hit type (0xb); the fighter's own switch covers types 1-10. |
| `IsDebuggingActive` | Fixed answer | False (the debug-only paths stay off). |

## Natives once thought to be OMFBG.exe's

OMFBG.exe registers no natives: it is the launcher (front end and session glue), and its whole
engine import surface has no `DE_CClass` or registration entry point (FINDINGS 123, from the
readable symbol and import tables in `source/omfbg-exe/REPORT.md`; its code stays encrypted and
is not decrypted). What used to be listed here is settled:

| Native | Kind | Notes |
|---|---|---|
| `CObject::NewGenericDescriptionSelector()`, `NewChooseFighterUIManager()`, `NewRulesDisplay()` | Stand-in | Real intrinsics; their classes are Core's (`DE_CGenericDescriptionSelector`) and OMF's (`DE_CChooseFighterUIManager`, `DE_CRulesDisplay`). They wait on the front end (see above). |

`CStandardSessionSlot::IsCreatureActive` and `CStandardSessionModifier::RegisterUrgentState` have no
registration in the DLLs either, but their methods are in Session.ModuleDLL and match ours
(creature present; empty in release).

## Data decoding

| What | Kind | Notes |
|---|---|---|
| Pilot stats | Ported | `DE_CPilot::ObjProcessContents` v4: three colour references (`CValueRGBA::CAdvancedReference`) each with a float, the four stats, then the 31 AI stat descriptors, read in order. |
| VMC2 trailing bytes | Ported | `DE_CClass::LoadVMCode` seeks to the chunk end after the final instances: the 1–3 bytes there are never read by the original either. |
