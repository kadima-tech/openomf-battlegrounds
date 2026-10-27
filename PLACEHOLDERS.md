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

| What | Kind | Notes |
|---|---|---|
| Robot, pilot, arena and score-limit choices | Stand-in | Picked up front by `openbg-fight`, the way `BSinglePlayerSubSession` does, instead of through menus. |
| Factions (teams' pilots) | Approximation | Ported for what Last Man Standing does (FINDINGS 112): a pilot's faction is null unless the picker sets one in a team game; `CPilot::Faction`/`SetFaction` and `CFaction::Photo` are ported. Team game modes (and with them `CFaction::TeamIndex`, faction vtable +0x180, and the session's team lists) are not run or decoded. |
| UI managers (description selectors, choose-fighter UI) | Stand-in | A launched UI with a preset choice completes on the next update; without one it never completes. `IsUIManagerActive`/`IsActiveLocally`/`IsDisplayed` return false. |
| Loading screen | Stand-in | `update_session` imitates the engine's loading screen (`SessionQuerryIsLoadingRequired` → `SessionLoadElements`, once per element). The real screen lives in OMFBG.exe, which stays unanalysed. |
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
| Music | Approximation (timing, level) | Music plays. The game side runs Music.DEObjects' `MusicPlayer` actor and its `BMusicPlayer` behaviour, the `CSpeaker` natives, the session's moods and the mood jumps between segments (`game/music.h`). `app/music_player.cpp` renders the segments with GothicKit dmusic (`third_party/dmusic`), resolving their style and DLS references against the song's `DirectMusicContainer`. Two parts are inferred. (1) The almost-end notification fires 1 s before a segment and its repeats end; DirectMusic's prepare time is unknown, and the chained segment starts exactly at the end rather than on DirectMusic's boundary flags (speaker element `+0xa0`: `\|0x80`, or `\|0x800000` with `+0x74` set; not decoded further). (2) The music plays 6 dB down, because dmusic at full scale clips with the sounds on top. dmusic's synthesizer is not Microsoft's, so instruments and mixing differ; it also ignores some controllers (CC1 modulation) and chunk fields. `OPENBG_DEBUG_MUSIC=1` traces it. |
| Sound | Approximation | Positional volume (hotspot to falloff) and stereo pan from the listener; no Doppler, occlusion or the original mixer's voice limits. Volumetric sounds play as spheres. |
| Particle modifiers | Ported | `Rotator`, `OriginMagnet`, `PushDownZModifier`, `MiscellaneousNewParticleFunctions`, `BeamSpawner`, `LighteningSpawner`, `AutoExternalSpawner` (FINDINGS 114). The lightning midpoint jitter on all three axes is confirmed from the machine code (three `CenterRand` calls). The 0x78..0x94 lightning fields are read but unused (zero in every shipped spawner). |
| Particle renderers | Approximation | `StreamParticleRenderer` and `PointStreamer` trails are built in screen space as the original does (FINDINGS 116): projected points, pixel-space perpendiculars, half-width over depth, stream joints averaged. Ours works on the projection plane and puts the vertices back in world space at each point's depth, which is the same picture for square pixels. All stream texture modes (0, 2, 4, with the time scroll) and colour modes (0-5) in the data are decoded; the trails' blend is confirmed as the particle renderers' material mapping. `FourFaceTriangle` (clouds, fog) is ported; its camera-facing modes 1-3 are inferred (the shipped ones use 0 and 2). |
| Billboard details | Approximation | Streak modes 1–5 are drawn as quads from the particle along the direction; modes 14–23 (rare) fall back to mode 0; the z offset and the light-cone fades (cone modes 1/2) are ignored; cone mode 3 scales by the glow value at 0. "Coth Type" curves are linear. |
| Ring renderer | Approximation | Annulus of 40 segments from the decoded edges and colours; the `+0xd4/+0xd8/+0xdc` flags and the detail value are not used, and plane modes are inferred from the 90-degree turns in `Process`. |
| Particle stepping | Stand-in | Effects step every game frame for every visible actor in the scene; the original steps an instance when its element is drawn (so culled effects pause). Particles are not depth-sorted. |
| Attack trailer image | Approximation | Additive ribbon with the fighter's `AttackTrailer` image; the point's life in `Process` is read as 1 − age × decay (the decompile drops the subtraction), with creature time in ticks. |
| Arena path animation | Approximation | Ported (`AnimationStreamPlayer`, FINDINGS 99): paths place City's trains, SpaceStation's cruiser and tazers, FirePit's target drones and every intro camera. Arena actors a path moves leave the baked arena mesh and are drawn live, without their glow-layer passes. |
| Animated images (`Image\AVI`) | Approximation (colour) | `CImage::SetPhase` picks the video frame (`de/avi.h`). Every shipped video decodes: uncompressed DIB, Cinepak, and Indeo 4/5 through openindeo (`openindeo/` next to `battlegrounds/`, our own MIT decoder; all 18 Indeo files bit-exact against FFmpeg's decoder). Inferred: the YUV → RGB step. Intel's codec converted to RGB for the game itself; we use BT.601 studio range, each chroma sample covering its 4×4 pixels (no chroma interpolation). openindeo's tables are FFmpeg's for now, to be diffed against Intel's DLLs. Procedural images ignore the phase (no script animates one). |
| Observer camera | Approximation | Its natives are ported (FINDINGS 112): the controller list, the analog axes, `CSimplePhysics::DeltaTick`. Not ported: its command executors (`MouseAxisNavigation`, `DigitalOrientationPropulsion`, `Teleport`), which are its autopilot, and binding its mouse controller to the mouse. So `CCreature::DeltaTick` has nothing to run and the camera holds still. The fights never show it (only spectating does). |
| Hit sparks | Approximation | Ported (FINDINGS 103). The sparks spawn and move in the game tick rather than when drawn, and their spark process ticks once a frame (the original's temporal-process rate, fighter +0x1dc, is taken as every frame). With `--direct` there are no sparks: the fighter's `SparkEffect` template comes from a passive reference that only the session path prepares. |
| Gargoyle's wings | Approximation | The second rig is drawn (FINDINGS 104): skinned under the body's Chest pose by `WingAnimationManager`'s sequence, time and prelude. `WingBase`'s own transform is taken as identity (as in the data), and a prelude blends from the wing pose last drawn, like the body's. |

## Waiting on large reverse-engineering pieces

| What | Kind | Notes |
|---|---|---|
| Command manager | Approximation | Ported (`World::run_commands`, FINDINGS 98, 106): the command list, sort, per-type assignment, primary/alternate arbitration and counters run once per creature tick (the engine's `DeltaTick` path is never registered). Look-at offsets are not kept (no script sets one). |
| Creature needs | Approximation | Ported (`needs.cpp`, FINDINGS 106): the 100000-unit test sphere at the focal point over group 0x800, each touched affector's Need, walked from the last; fighters have no team (TeamIndex -1). The touched affectors come in our scene order (see "Collision result order"). |
| Interface calls that switch state | Approximation | A carryable's `CarryableLeftToss` that enters a state (the jet pickup's `LaunchFighters`) ends the call early, as `ContinueExecutionInternal` case 0x22 does, so the holder keeps its reference until the pickup deletes itself; our VM gets there by restarting the callee's executor rather than through the engine's separate per-call executer. |
| Force details | Approximation | Floor events pass a null `COMF2Event` (no handler reads it); `AddSurfaceChangeRecipient` hooks are kept but never called; a material's incident threshold is kept per material and touched affector, not in a scene incident recorder; the `CommonForceParticleSpawningMaterial` +0xc4 option (mesh height values on the created actor) is ignored; a `Physics\Simple`'s force is taken to be the common force on its actor (its +0x60 reference is not decoded); |
| Creature update rate | Approximation | Ported (FINDINGS 108): one world tick is 16 ms of scene time (the original's clock is real milliseconds, fighters update every 16 ms, 62.5 a second); creatures with a SceneTicksPerUpdate update on multiples of it (from 1 with actor flag 0x10), catching up to 100 at once. Scene time advances by exactly 16 ms a tick here, where the original's moves with the real frame times (a frame every 20 ms or more), so the updates that share a frame in the original are spread out here. |
| Who controls a slot | Stand-in | `OwnCreature` makes the creature user-controlled when the slot's client is the user's (`SetupCreatureControl` and the controller binding are not decoded). A fighter's controllers are made at run time by that binding, so it gets a stand-in controller (`Controller(i)` past its data controllers). |
| Fighter creation (`CFighterDescription::NewResource`) | Stand-in | The app's `setup_fighter` instantiates the robot's data file, attaches `P<Name>`, adds the command manager and hookup executor by hand; the description's replace-by-name and add-children lists are not applied. |
| A fighter counts as spawned | Stand-in | When `BFighter::ObjectSpawn` first places it (`SetLocalPosition`); depends on how the original adds creatures to the scene (fighter creation above). |
| Scene membership and executor start | Approximation | `scene_roots` plus `enter_scene`, which prepares a branch (every executor attached) before running any entry. Passive references follow `DE_CPassiveActorReference::Create`: placement 0 puts the created actor under the reference's parent (so it is in the scene through the tree), placement 1 leaves it parentless, in the scene only when +0x120 adds it; +0x124 deactivates it; prepared mode 2/3 references hand their children and the references naming them to the created actor. A parentless actor added by +0x120 is a scene root (the original's `DE_CScene` +0x130 call is taken to be `Add`). |
| Attached session modifiers | Stand-in | Parented to the session rather than the master, so that `Session().Interface(...)` finds them; and `Interface()` searches an object's own brains and its direct children only. Both wait on decoding `DE_CObject::Interface`'s search. |
| Teams (`IsEnemy`, enemy list) | Approximation | `IsEnemy`, `NumEnemies`/`Enemy`, `GlobalInterest` and `CurrentTarget` are ported (FINDINGS 100, 106, 111). A fighter's team (+0x49c) comes from its pilot's faction's `TeamIndex`, and factions exist only in team games, which are not run; so no two fighters are allies, as in Last Man Standing. |
| Collision result order | Approximation | Results come in scene order, as `DE_CAffector::IsTouching` walks the root scene's affector list (+0x184); ours is the order objects were created, which is taken to match the order affectors joined the scene. |
| `CObject::Die` | Stand-in | Registered by OMFBG.exe (unanalysed); removes the object like `DeleteThis`. See "Natives registered by OMFBG.exe" below. |
| Creature components (`AddComponent`) | Approximation | The component becomes the creature's child and records its owner; the creation-time alignment that keeps their updates in step (+0x1d8) is not modelled, since update phases are not. |
| Scene tests | Approximation | `TestSphere`, `TestSpherePath`, `TestCone` and `TestLineSegment` are ported (FINDINGS 102, 108). The ray (and every cone) touches a mesh or box at its nearest face per affector, one collision each; the original's `DE_CMeshReference::IsTouchingCone` is not decoded, and a cone with a penumbra touches spheres only (`DE_CBoxAffector::IsTouchingCone` does the same; meshes are not tested). |
| Arena surface hits | Ported | `ProcessEvent` hands the event to the object's relays, as `DE_CActor::ProcessEvent` does with its properties (FINDINGS 116). No handler in the DLLs or scripts reacts to the surface-hit type (0xb); the fighter's own switch covers types 1-10. |
| Fighter push order | Approximation | `DE_CFighterPhysics::Tick`'s push against the other fighters is ported (FINDINGS 100); with three or more fighters the original walks its list (+0x334, filled by `ObjPrepare`) backwards, ours in creation order. |
| `IsDebuggingActive` | Fixed answer | False (the debug-only paths stay off). |
| `--direct` mode pilot | Stand-in | `pilot_tick` keeps each computer fighter on an attack command against its first enemy; only with `--direct` (sessions run the real pilot script). `--direct` creates the arena and picks spawn points as the session does (FINDINGS 109). |

## Natives registered by OMFBG.exe (guessed)

No module DLL registers these: neither a `Caller` nor the name string exists in any of them, and
OMFBG.exe, which presumably registers them, is SafeDisc-encrypted and stays unanalysed. Each is
our reading of what the scripts expect from its name and use, so all count as approximations.
Found by `OPENBG_LIST_NATIVES=<file>`, which lists every registered native, checked against the
DLL dumps.

| Native | Kind | Our guess |
|---|---|---|
| `CObject::Die()` | Stand-in | Deletes the object like `DeleteThis` (projectiles call it once their explosion is over). |
| `CCollisionEvent::SourceAffector(int)` | Approximation | The tester's affector that made collision i (the counterpart of `TouchAffector`). |
| `COMF2Fighter::IsEvadeButtonDown()` | Approximation | The duck/evade button (one button, id 5), like `IsDuckButtonDown`. |
| `CObject::NewGenericDescriptionSelector()`, `NewChooseFighterUIManager()`, `NewRulesDisplay()` | Stand-in | A runtime object of that UI class; the UI itself is the front end's (see above). |
| `CTextPrimitive::SetShadowOff()` | Approximation | Draws the text without its drop shadow. |
| `CSimplePhysics::SetGravity(float)` | Approximation | Sets the physics' gravity (+0x74, the field the data sets). |

`CStandardSessionSlot::IsCreatureActive` and `CStandardSessionModifier::RegisterUrgentState` have no
registration in the DLLs either, but their methods are in Session.ModuleDLL and match ours
(creature present; empty in release).

## Data decoding

| What | Kind | Notes |
|---|---|---|
| Pilot stat block offset | Approximation | Read at body+90 of the v4 chunk (90 bytes of references in every shipped pilot); the reference layout itself is not decoded. |
| VMC2 trailing bytes | Unknown | 1–3 bytes after the final instances are not interpreted. |
