# Official patches (2.090 → 2.123)

The port follows the disc release, **2.090**. The game was patched eight times after it; this
file says what each patch changed and what the port has to do to follow it. Nothing here is
applied yet (PLACEHOLDERS, "Patch level").

Each change is tagged *observed* (seen in the patched data or code), *inferred* (read from it,
not seen running) or *uncertain*. Script changes name the Behavior object and its state /
method; DLL changes name the function and its address in the patched build (and on the disc
where the build was rebased). Values are given old → new.

## The versions

| Patch file (as named) | What it really is | Entries | Build (PE time, UTC) | Tree |
|---|---|---|---|---|
| (disc) | 2.090 | | 2003-11-24 | `source/extracted/App_Executables` |
| `omf-battlegrounds-patch-v2090-to-v2095.exe` | 2.090 → 2.095 | 53 | 2003-12-16 | `source/patched/v2095` |
| `omf-battlegrounds-v2095-v2096.exe` | 2.095 → 2.096 | 43 | 2003-12-25 | `source/patched/v2096` |
| `omf-battlegrounds-v2096-v2097.exe` | 2.096 → 2.097 | 10 | 2003-12-28 | `source/patched/v2097` |
| `one-must-fall-battlegrounds-v2097-to-v2100-patch.exe` | 2.097 → **2.101** | 51 | 2004-01-05 | `source/patched/v2101` |
| (none) | 2.100 → 2.101 | | | not needed (see below) |
| `one-must-fall-v2101-to-v2111-patch.exe` | 2.101 → **2.112** | 39 | 2004-01-15 | `source/patched/v2112` |
| `one-must-fall-v2111-to-v2112-patch.exe` | 2.111 → 2.112 | 16 | 2004-01-15 | (no-op on our 2.112) |
| `one-must-fall-battlegrounds-v2112-to-v2121-patch.exe` | 2.112 → **2.123** | 49 | 2004-01-24 | `source/patched/v2123` |
| `one-must-fall-battlegrounds-v2121-to-v2123-patch.exe` | 2.121 → 2.123 | 41 | 2004-01-24 | (no-op on our 2.123) |

**The disc is 2.090.** The InstallShield header on the disc (`Setup/data1.hdr`) carries the
product version 0.20.2090, and the first patch's old files are exactly the disc's files
(RTPatch checks every old file; all 53 were accepted). The game keeps its version in the
registry (`HKLM\Software\Diversions Ent\One Must Fall Battlegrounds\{B8EF8B5A-…}\Version`,
read by OMF.ModuleDLL; no patch writes it), but the build number is also compiled into the
modules: OMF's `DE_COMF2Manager::Activate` and Core's `WinSystemInit` push it as an immediate
(`re/scripts/build_number.py`). It reads 2090 on the disc and 2095, 2096, 2097, **2101**,
**2112**, **2123** in the rebuilt trees, which is how the trees are named.

**How the trees were rebuilt.** Every patch is an RTPatch 7.00 (Pocket Soft) self-applying
stub with the patch file appended after its last PE section. The patch format is
proprietary and compressed, so it was not decoded: `re/scripts/rtpatch_emu.py` runs the
game's own `AutoPatch/patchw32.dll` (RTPatch 7.00, the same engine the stubs embed) under
Unicorn, serving its Win32 calls from Python against a sandboxed copy of the install, and
`re/scripts/rtpatch_chain.sh` chains the patches from the disc. Nothing from the patches
runs natively. RTPatch verifies each old file before patching and each new file after
("Verifying - Passed!" for every entry), so the trees are byte-exact for every file a patch
touches. `--header` prints a patch's entry count and the summed sizes of its old and new
files, which is how the labels below were checked.

**Mislabelled patches; what is lost.** The patch files were evidently rebuilt against the
newest release of their day and kept their old names. *Observed* for each:
- *"2097 → 2100" yields 2.101.* Its output's build number is 2101. Consistently, the
  "2101 → 2111" patch accepts all 39 of its old files from that tree (RTPatch checks each old
  file: it rejects 2.097's Input2 and a 2.101 file with one byte changed), and its header's
  old-size total equals the tree's exactly. So the missing 2.100 → 2.101 step costs nothing:
  2.101 is rebuilt, and 2.100 itself (never on its own) is folded into the 2.101 section.
- *"2101 → 2111" yields 2.112.* Its output's build number is 2112; the "2111 → 2112" patch
  reports all 16 of its entries (the launcher included) as "New Version of File already
  exists", rejects the same file with one byte changed, and its header's old-size total
  (96,046,582) is not the tree's (96,905,845, which is its new total).
- *"2112 → 2121" yields 2.123.* Build number 2123; all 41 entries of the "2121 → 2123" patch
  are already up to date.
- So 2.100, 2.111 and 2.121 cannot be rebuilt; their changes are folded into 2.101, 2.112 and
  2.123 below, and which of the two releases made a given change is unknown.
- Later patches still apply on top of these trees (each one checks its old files), so the
  chain disc → 2.095 → 2.096 → 2.097 → 2.101 → 2.112 → 2.123 is complete for every file the
  patches touch. No patch adds or deletes a file; every one replaces `Engine/OMFBG.exe` whole
  (an ADD entry) and modifies the rest in place.

**The launcher stays protected.** Every patched `OMFBG.exe` still has the SafeDisc sections
(`stxt774`, `stxt371`) and an encrypted `.text`, so only its COFF symbol table and import
names are compared (`re/scripts/launcher_diff.py`); its code is not decrypted or run.

**Tools** (all in `re/scripts/`): `deobj_diff.cpp` (build with `build_deobj_diff.sh`) matches
two `.DEObjects` by GUID and reports objects added / removed / changed, Behavior source
elements and compiled Class methods; `patch_diff.py` walks two trees; `pe_funcdiff.py` diffs
module DLLs function by function from their COFF symbols, with relocated operands and
branch targets normalised to symbol names; `launcher_diff.py` as above; `build_number.py`. Their detail output is
game data and stays in scratch.

**Reading the diffs.** Most "changed" DLL functions are rebuild noise (register allocation,
renumbered string symbols, offsets that moved because a class grew); a Class whose only change
is its VMC2 change-info block was recompiled, not changed; a script edit renumbers its class's
globals and intrinsics, so many compiled methods differ by index only. The entries below come
from the Behavior source diffs and the real code differences.

## Applying the patches to the port

Most changes are script and data: the port runs the shipped Behavior/Class code, so pointing it at
a patched `Data/` tree picks them up. Apply one version as a whole rather than mixing: 2.112
renumbers the attack-type bits, so a 2.112+ data tree needs the 2.112 natives and vice versa. The
engine-side work, cumulative up to 2.123 (details and confidence in each version's section):

| Version | Engine change | Where in the port |
|---|---|---|
| 2.095 | `TransitionUIManager` activates the transition-doors actor before sending it START/END/BUSY | `engine/src/game/ui.cpp` |
| 2.096 | `CAttackInfo::SetNoCounter` / `IsNoCounter` (flag 0x80 at +0xc4) | `engine/src/game/natives_combat.cpp` |
| 2.096 | `CFighterDescription::GetRandomFighterModelDescription`; `GatherFighterModels` real | `engine/src/game/session.cpp`, `descriptions.cpp` |
| 2.096 | `DE_CActor::CanStateInterrupt` (lagged-state replacement; matters for networking / replays) | `engine/src/game/replay.cpp` |
| 2.097 | Null checks in Options > Controls assign / capture | `engine/src/game/ui.cpp` |
| 2.101 | `NextUIManager()`, `CurrentUIManager()`, `CStandardClient::IsAdministrator()` natives | `engine/src/game/natives_script.cpp` |
| 2.101 | `CloseUIManagerStack` deletes a queued auto-destroy page; `BRulesDisplay` close rule (cf. FINDINGS 200) | `engine/src/game/ui.cpp` |
| 2.101 | Mesh placement times (mesh base chunk +0x44) put lava / fog in the earlier transparent layer | `engine/src/de/mesh.cpp`, `engine/app/render.cpp` |
| 2.101 | Script frames defer destroying their class instances | `engine/src/vm/executor.cpp` |
| 2.112 | Light attack bit 4 (damage < 15) in `StartSimpleAttack`, later bits shifted; `IsAttackTypeLight` | `engine/src/game/natives_combat.cpp` |
| 2.112 | `UnblockableAttacks` BOOL value honoured by `TestForFighterBlock` | `engine/src/game/natives_combat.cpp` |
| 2.112 | `CDescription::UnlockSinglePlayerMatch`; Starting Match combo on Choose Cast (`Matches`, `StartingMatch`) | `engine/src/game/natives_script.cpp`, `ui.cpp` |
| 2.112 | Session values fall back to the session's description (`ObjFindValueByName`) | `engine/src/game/runtime.h` (`World::object_value`) |
| 2.112 | Speaker playing frequency capped at 1.5 | `engine/app/audio.cpp` |
| 2.123 | `CActor::RenderQuality()` (get-hit debris), `ReplaceWithThenDie` natives | `engine/src/game/natives_core.cpp` |
| 2.123 | Creature global interest read from contents (+500) instead of 0 | `engine/src/game/runtime.h` |
| 2.123 | Observer: major-move factor 1.1, major timer clamped at −1 | `engine/src/game/observer.cpp` |
| 2.123 | `GetPathAffector` widens its search by 100 up to 1000 | `engine/src/game/navigation.cpp` |
| 2.123 | No door transition on the first page change | `engine/src/game/ui.cpp` |
| 2.123 | `UnpackForGetHitEffect` uses the packet's damage byte (the port reproduces the old bug) | `engine/src/game/natives_combat.cpp` |
| 2.123 | Ambient sounds attached to the camera (`PlayNoiseAmbient`) | `engine/src/game/natives_script.cpp`, `engine/app/audio.cpp` |

The modules were rebased in 2.096 (Core 0x20000000, OMF 0x10700000, Creature 0x10200000,
Session 0x11000000, Direct3D 0x10300000, Input2 0x10600000, GameSpy 0x10500000, Particles
0x10900000; AudioPlayer stays 0x10000000), and 2.112 adds a `DE_CObject` virtual
(`ObjFindValueByName`) that moves every later vtable slot by 4: FINDINGS addresses and vtable
offsets are the disc's and need translating before comparing with a patched build.

## v2.095 (from v2.090, disc) — built 2003-12-16

PE timestamps: every module 0x3fde6edd..0x3fde6f9a (2003-12-16 UTC); disc 2003-11-24.

Files touched: 53 patched, 0 new, 0 deleted (OMFBG.exe is shipped whole as an RTPatch "ADD").
- Engine (10): OMFBG.exe, AudioPlayer, Core, Creature, Direct3D, GameSpy, Input2, OMF, Particles, Session `.ModuleDLL`. (dbghelp.dll untouched.)
- Data (43): BladePit, Canyon, Chronos, ChronosModel1, CinematicCharacterCollection, CinematicMasters, City, Desert, DesertRings, Effects, Fighter, FirePit, Force, ForceModel1, Gargoyle, GargoyleModel1, Grudge Match Arena, Iceberg, Jaguar, JaguarModel1, Katana, KatanaModel1, Mantis, MantisModel1, Music, OMFBGGUI, OMFHelp, Pickups, Pilots, PowerPlant, PracticeArena, Pyros, PyrosModel1, Session, SharedObjects, SinglePlayer, SinglePlayerFull, SpaceStation, Sport, Trophy Room, Volcano, Warlord, WarlordModel1.

Almost every gameplay change is in Behavior source / compiled Class code in the data, which the port runs as shipped. The only new script intrinsic the 2.095 classes reference is `CObject::CGUID::bool (opp !=)(class CGUID)` (Core `GUIDIsNotSameCaller` 0x7014cf70), already registered in `engine/src/game/natives_script.cpp`. So loading the 2.095 Data tree should apply the gameplay section without engine edits; the engine-side items are listed separately.

### Gameplay and balance

- **New jump-evades (all eight fighters).** Double-tapping right / left / back during the jump prelude now launches `Ground.Evade.RightEvadeLongJump` / `LeftEvadeLongJump` / `BackEvadeLongJump` (side flips with a 360° pelvis roll; the back one has a short no-fighter-collision window at 30-50 % to pass over an opponent). Side variants cost 2× evade stamina, back 3×; a mid-air turn is possible (press the opposite direction at 60-80 %, or AI at distance < 100 with high evade affinity); hit-evade air attacks are allowed late (>70 %, >90 % for back). Per-fighter tuning finals `m_SideJumpEvade{Movement,Durration,Height}Multiplier` and `m_BackJumpEvade…` (e.g. Chronos 0.9/1.3/1.2, Katana 0.9/0.86/0.85, Warlord 1.0/1.2/1.1 and back 0.9/1.3/0.8, Gargoyle all 1.0). — *where*: `BFighter` (Fighter.DEObjects) new `Control.Ground.JumpPrelude` with a new insertion point for per-fighter jump-prelude moves; per-fighter copies in `BChronos`, `BForce`, `BGargoyle`, `BJaguar`, `BKatana`, `BMantis`, `BPyros`, `BWarlord`; three new animation sequences `LeftEvadeLongJump`, `RightEvadeLongJump`, `LongEvadeBack` in each fighter's `*AnimationBank` (AnimationSequenceGroup 130 → 133 sequences in Chronos). Mantis' `BugDropLeap` and Katana's `ReboundStrike` jump-prelude moves moved into the new insert point (still there). — *port*: data only (scripts + animation banks); check `SkipFighterCollision` (`engine/src/game/forces.cpp`) and the double-tap sequences in `engine/src/game/controls.cpp` input banks. [observed]
- **Side grapples.** `Grapple` takes a `Type` (front 0 / right 1 / left 2): holding left/right during the front grapple prelude switches to a side grapple that turns the fighter 1.4°/frame (× pilot speed) and the chest up to 55° toward that side. Grapple length 70 → 78 frames, hit window starts at 0.22 (was 0.15), forward propel × 0.8 and only for the front type, `GuideToBestMeleeTarget(0,0)` at start. Pyros (`Ground.PunchThrow` renamed `Ground.Grapple`) and Force now go through the standard `Ground.Grapple.TryLaunch`, so the **AI now grapples with Pyros and Force** (cone 26 units, grapple affinity). — *where*: `BFighter` `AttackTemplates.Attacks.Grapple`, `AttackTemplates.TryLaunch.Grapple`; `BPyros`/`BForce` `Control.Ground.TestSpecials`. — *port*: data only. [observed]
- **PunchThrow input loosened.** The standard bank's `PunchThrow` sequence went from one requirement (both punches with up/up-left/up-right, within 5 frames) to two: both punches within 7 frames, plus a direction among up, up-left, up-right, left, right — so side directions trigger it (needed for the side grapples). — *where*: Fighter.DEObjects `StandardInputSequenceBank` (JoystickInputSequenceGroup). — *port*: data only; read by `read_input_bank` (`engine/src/game/runtime.h`). [observed; direction-bit reading per runtime.h's layout]
- **Long side evades nerfed.** Sideways long evade runs 1.2× faster through its animation, homing `GuideToBestMeleeTarget(20,.3)` → `(20,.2)`, overall push × 0.9, and the near-target damping now starts at 50 units (was 100), scales 0.5..1 (was 0.4..1 right / 0.1..1 left) and adds up to 0.5 more when the target is not in front. — *where*: `BFighter` + every fighter's `Ground.Evade.RightEvadeLong` / `LeftEvadeLong`. — *port*: data only. [observed]
- **Stamina/stun/block tuning.** `m_StunGainFromDamageInflicted` 6 → 10; `m_StaminaLossEvade` 300 → 220 (× 0.8); `BFighterStats` `m_StunSubtractedOnStun` 30 → 60 and `m_FighterBuildStunMultiplier` 0.19 → 0.27 (longer, faster-building stuns); `BAttacker` block recover times light/medium/heavy/long 400/500/700/950 → 600/700/900/1150 ms; `Air.HitEvade` flight 52 → 59 frames. — *where*: `BFighter` members, `BFighterStats`, `BAttacker` (Fighter.DEObjects). — *port*: data only. [observed]
- **Jaguar**: `m_EvadeSidewaysAnimationFrames` 57 → 71, `m_EvadeSpeed` 1.6 → 0.95 (slower, shorter side evade). **Force**: `ForwardThrust` 2.3 → 1.3 on `Ground.Attack.LeftKickLight`. — *port*: data only. [observed]
- **Super-move chains only continue on a clean server-side hit.** Jaguar `Ground.SuperLeap.Attack1..8` and Mantis `Ground.SuperInsectStrike.*` chain to the next hit only if the attack connected, was not blocked, and on the server; a 10-tick `exSleep` follows each so no new move can be started; Mantis' hit states register as standard states with correction instead of standard attacks. — *port*: data only. [observed]
- **Head hurt-sphere removed.** Each fighter loses the `GetHitAffector` sphere under `Pelvis/Chest/Head` (Chronos, Force, Gargoyle, Jaguar, Katana, Mantis, Pyros; Warlord unchanged), shrinking the hittable volume around the head. — *port*: data only. [observed object removal; gameplay effect inferred]
- **Get-up**: holding duck while getting up face-up no longer gives the turn-around get-up, and the late `GuideToBestMeleeTarget(20,.1)` homing during the get-up is gone. — *where*: `BFighter` `Ground.GetUpFaceUp`. [observed]
- **Pickups**: a fighter can no longer immediately re-pick the carryable it just held (`CarryableLeft/RightLastKnownHolder() != this`). — *where*: `BFighter` pickup tests; `BCarryableLeft/Right` (Pickups.DEObjects) new accessor methods. [observed]
- **Crowd (match "love")**: love decay only runs while the monitored fighter has a vulnerable fighter target (no booing a fighter who can't hit anything); love resets to 0 when the fighter is dead; changes of more than 3 (excitement) / 2 (boredom) replicate as `CrowdExcited` / `WaitingForAction`. — *where*: Session.DEObjects `BCrowdResponseSlot`; its state manager's history field 3 → 2. — *port*: data only. [observed]

### Bug fixes

- **Death volumes.** Arena-local `DeathVolume` box affectors (City, Desert, FirePit, PowerPlant, Sport, Volcano; plus new ones in PracticeArena and SpaceStation) are replaced by `Actor\PassiveReference`s to SharedObjects' `DeathVolumeBoxAffector`, whose box grew from ±10 000 to ±100 000 horizontally with its top at y = −100 (was 0); Canyon's reference offset −10 → −100. `BDeathVolume` now checks in its executor loop (`CheckKill` every executor tick; `DeathVolumeExecutor` interval 0 → 1000 ms) instead of a scene delta-tick hook. Two arenas also drop an unused CommonForce particle material. — *port*: data only; our executor intervals come from the ClassExecutor contents (`engine/src/game/runtime.cpp`). [observed]
- **Effect faders off the gameplay clock.** `BEffectFader.Entry` hooks the render tick instead of the delta tick; a new `GameplayEntry` (new `GameplayFaderExecutor`) keeps the old delta-tick behaviour for faders that matter to gameplay. New `PGameplayPathFolower` / `EGameplayPathFollower`; `BPathFollower`/`BRenderUpdater`/`BGameplayUpdater` gain an overridable `Setup()`. — *port*: data only (`CScene::RegisterRenderTickHook` exists in `natives_script.cpp`). [observed]
- **Fighter loading / rules display.** `BCreatureAndPilotPicker` skips loading when the chosen creature, model, pilot and team are already loaded (jumps straight to `Fighting`), copies the chosen model GUID into the loaded one, handles the load events through a session event relay only while `WaitingToLoad`, ignores late `UIManagerComplete`s once authorised, and lets a client load if the server already authorised it. `BRulesDisplay` sends the load query with `SendToNow(Session())` instead of `SendToSessionAndSlots`, polls every 2000 ms (was 1500), `IsReadyToLoad` now returns false explicitly at its end. — *port*: data only; `CEvent::SendToNow` and event relays are in `engine/src/game/natives_script.cpp` / `session.cpp`. Relevant to the open Practice issue in PLACEHOLDERS (FINDINGS 200). [observed]
- `Ground.RunForward.Skid` ends in `Ground.Stand()` (fell off the end before). Inner block attacks (type 101) register as correction + no-interrupt. `Air.HitEvade`'s attack tests moved into a shared `Control.Air.TestHitEvadeAttacks`. — *port*: data only. [observed]
- **Single-player text**: dialogue reflowed with `<Return>` breaks, typos fixed ("Ibrahim's", "despicable", "his behavior", "non-disclosure"), one quote moved to the right speaker; a few match states gain a missing `exSleep()` / `.LateInMatch()`. OMFHelp: 36 strings and 26 descriptions get typo/whitespace fixes. — *port*: data only. [observed; the exSleep moves are partly obscured by trailing junk in the dumped sources — uncertain]
- **Music download leak** — `DE_CDirectXSpeakerElement::Activate` (AudioPlayer 0x10004460, same address): when a speaker switches to a noise with a different DirectMusic download object (+0x20), the previous one (+0x90) is now unloaded first; before, it was only unloaded when the noise had no band segment (+0x1c), so switching songs kept the old song's instruments downloaded. — *port*: none needed unless `engine/app/music_player.cpp` tracks per-song downloads. [observed]
- **Page transitions** — `DE_CAppManager::TransitionUIManager` (Core 0x7019bcc0, disc 0x7019ab30) no longer calls `SignalStartPageTransitionBegin/End` / `PageTransitionBusy`; it activates the transition-doors actor (+0x90) with the new `DE_CActor::SetActive(int)` if `IsActive()` is false, then sends `EVENT_PAGE_CHAGE_TRANSITION_START/END/BUSY` straight to it. — *port*: make sure the doors actor is active before sending transition events in `engine/src/game/ui.cpp`. [observed]

### New content

- None (no new fighters, arenas or modes). New data objects are support only: 3 animation sequences per fighter, `StarField` image + gameplay path-follower/fader classes in SharedObjects, SpecularFactor values (below).

### Engine and rendering

- **Reflection specular**: 28 `SpecularFactor` (Value\FLOAT) children added under fighter-model `EnvironmentReflection` managers in CinematicMasters (0.6), plus City/Volcano ones 0.6 → 0.4 and new ones on City's round building (0.4) — less glow from reflections. — *port*: already read (`engine/src/de/scene.cpp` `child_float(…,"SpecularFactor")`). [observed]
- **Assets re-saved**: every fighter model's `Image\Standard` contents gain one trailing byte (chunk v7, 25 → 26 bytes), `TextureManager\Reflection` gain an `ObjExtCon00` EX entry, LOD meshes set editor flag 0x1000 (cleared at load, FINDINGS 88), fighter model descriptions +2 bytes; Grudge Match Arena and OMFBGGUI texture sets rebuilt (OMFBGGUI drops 19 texture managers). — *port*: readers seek to chunk ends; verify the 2.095 models and GUI still load (`engine/src/de/image.cpp`, `texture.cpp`). [observed; meaning of the extra byte unknown]
- **Mesh material roles cached** — `DE_CMaterial::ObjPrepare` (Core 0x7020e050, disc 0x7020bd80) resolves its role name (+0x5c) to a data-channel role ID (+0x70, init −1) and `IsValidSurface`/`ProcessForce` call the new `DE_CMesh::GetElementInfo(int&,int,int,const char*)` with the ID. Same behaviour, faster. — *port*: none. [observed]
- **Debug switches moved from the "runtime cookie" to the command line**: `LO_RES_TIMER` (`gTime` Core 0x7002abf0), `dedicatedserver` (`DE_CNetworkService::IsDedicatedServer` 0x701ad6b0), `no_renderer`, `sound_off`, `NO_AVI`, `autoselect_off`, `high_geometry_off`, `use_default_keyboard`, `trapfloaterrors`, `no_exception_handler`, notification filters, …; the `-network off` cookie is gone (`UpdateNetworkingEnabled` reads only registry `Network\Enabled`). Direct3D drops the `simulatefullscreen` (`CreateDevice` 0x20015910) and `candyoff` (`GetValidActorType` 0x20023580) cookies; `DE_CRuntimeCookie::IsExist` is no longer imported by OMF/Direct3D. — *port*: none (we have no cookie); use these names if we add such switches. [observed]
- **Layout growth**: a Core class used by creatures grew 4 bytes (`DE_CClassExecutorInstance::SymbiotExecutorInstanceDisconnect` +0x104 → +0x108; Creature `MainExecutor`/`Tick`/`SetAsUserControlled` +0x108 → +0x10c, +0xf8 → +0xfc); `DE_CSessionInfo` +8 (Session callers +0xac → +0xb4). Only matters when reading offsets from 2.095 binaries against FINDINGS addresses. [observed]
- Script compiler internals (`DE_CPCNode` FindIdentifier/VariableSize/Link, `DE_CClass` FindPublicCompiledMethod, …) and `DE_CString::CutSubString`/`GetSubString`/`operator==` changed; the shipped classes come precompiled, so no runtime effect is expected. [inferred]
- Input2, Particles: rebuilt, no function changed. Creature: no logic change beyond the layout shift. Direct3D: only the two cookie removals (plus the statically linked D3DX code being re-laid out).

### Network / GameSpy

- **GameSpy SDK upgrade**: the old CEngine server list + `qr` query/reporting (ServerList*, qr_*, `?basic/info/players/rules_callback`) is replaced by the ServerBrowsing + `qr2` SDK (key/player/team callbacks, GOA crypt, `GSIStartAvailableCheck` with "gmtest", MD5 helpers); `DE_CGameSpyFactory::Init` (GameSpy 0x2500a900, disc 0x250068d0) shows "Gamespy failed to initialize, server list will not be available." on failure. Game name `omfbattle` and LAN-only option unchanged. — *port*: GameSpy is a stub (defunct); nothing to apply. [observed]
- **Direct connect**: `-connect <address>` on the command line makes `TransitionUIManager` call `DE_CWinSockService::AttemptConnection` (now returns a `CNodeInfo`) and show a "Connecting to Server" dialog with Cancel (`DE_CDialog3d::AttachTextAndCloseButton`, new); quits when the dialog is cancelled; joins the first listed session once it appears. — *port*: optional, with networking (`engine/app/frontend.cpp`). [observed]
- **Server browser**: Core adds `DE_CPlayersControlRobot` (a players list control with name/score/team/ping sort functions), `DE_CSessionInfo::NumTeams` / `NumPlayersOnTeam` / `EnvironmentDescriptionText`, `DE_CSessionMasterDescription::NewPlayersControl`. — *port*: Join screen (missing in PLACEHOLDERS). [observed names; layout not traced]
- **State replication**: Session `DE_CSignificantStateManager` keeps its managers sorted by urgency (`gSortByStateManagerUrgency`), `ReplicateSessionStateManagerStates` (0x4500a610) and `AssureSessionSignificantStateManager` (0x45005de0) rewritten; Core `DE_CSession::ServerIssuedID`, `DE_CStandardStateManager::AddLaggedState*`, `DE_CClassExecutorInstance::CompareStatus` touched; OMF `DE_CFighterPhysics::PackState`/`UnpackState` (0x3503a5e0 / 0x3503aa20) reorder their packed flag bits (one more bit). Script side: `AttackerTryAttack` returns early on a client for a fighter it doesn't control, projectiles (`BStandardProjectile`, `BLaser`) only apply hits on the server, get-hit states use `RegisterGetHitState` + `RegisterLastStateNoReplicate` on clients, `m_AllowAirEvade` is replicated through the fall state. — *port*: only matters for networking / our replay packing (`engine/src/game/replay.cpp`); the script parts come with the data. [observed; bit meanings not decoded]
- One SinglePlayerFull `BMaster` source sets its `m_StartingMatch` to `matchCinematic` (was `matchViolenceLite`), a development setting; the compiled `PMaster` classes show only index renumbering, so it appears not to have been compiled in. [uncertain]
- **Capture Snapshot** debug hotkey (S, with the debugger active) added to `BStandardOMFSession.Begin` / `BMatchServer`. [observed]

### Launcher (OMFBG.exe)

- Still SafeDisc 2 (`stxt774`/`stxt371`); not decrypted. Timestamp 0x3fde6f9a, 1 392 278 bytes (was 1 392 287), same import DLL list. Its readable symbol table now names `DE_GLOBAL::CommandLineParameters`, `DE_CStringPool::FindStringIndex`, `CompareStrings` and `InvalidatePosition`, matching the switch to command-line options; `DE_CMainMenuUIManager::Begin` (OMF 0x3505a460, disc 0x3505a3c0) now picks the start page from command-line words `menu_replay`, `menu_multiplayer_join`, `menu_multiplayer_create`, `menu_singleplayer` (was runtime cookies). — *port*: optional start-page switches in `engine/app/frontend.cpp`. [observed for the DLL; exe code not read]

### Rebuild-only noise

- 130+ Class objects differ only in VMC2 change-info/index renumbering; most "changed" references/hierarchy blocks are re-ordered object indices after the removals; many DLL "changes" are register allocation, `__LINE__` constants in asserts, jump-table relocations and statically linked D3DX code moving.

### Summary

2.095 (built 2003-12-16, 53 files) is mainly a fighting-system patch shipped as script and data: every fighter gets new double-tap jump-evades (side flips and a back flip, three new animation sequences each) and side grapples, the PunchThrow input accepts side directions, long side evades are slowed and damped, stun builds faster and lasts longer, block recovery is 200 ms longer, the head hurt-sphere is removed and the AI can now grapple with Pyros and Force. Arena death volumes become one shared prefab checked once per second with a far larger box, effect faders move to the render clock, and the fighter/pilot picker skips needless reloads. Engine-side, debug "runtime cookie" switches become command-line switches (including new `-connect` and `menu_*` start options), GameSpy moves to the qr2/ServerBrowsing SDK with a players list in the browser, state replication is reworked, the music speaker unloads the previous song's instruments, and page transitions activate the doors actor before signalling it. The only new script intrinsic (`CGUID !=`) is already in the port, so running the 2.095 Data should pick up the gameplay changes without engine work.

## v2.096 (from v2.095) — modules built 2003-12-24/25 (PE timestamps 0x3fea2ac9..0x3fea2bc1)

Files touched: 43 (42 modified, 1 replaced). Engine: all nine `*.ModuleDLL` and `OMFBG.exe` (ADD, whole file). Data (33): ArenaDescriptions, BladePit, Canyon, Chronos, Desert, DesertRings, Effects, Fighter, FirePit, Flyers, Force, Gargoyle, Grudge Match Arena, Iceberg, Jaguar, Katana, Mantis, OMFBGGUI, OMFHelp, Pickups, Pilots, PowerPlant, PracticeArena, Pyros, Session, SharedObjects, SinglePlayer, SinglePlayerFull, SpaceStation, Sport, Trophy Room, Volcano, Warlord. No files added or deleted. Both old and new files are verified by RTPatch's own checksums.

Most of this patch is fighter tuning plus a reworked "delay on miss" recovery rule, network-play fixes (server-authoritative deaths and hit reactions), the session browser's filters, and a rewritten in-game help.

### Gameplay and balance

- **"Land delay on miss" became a general "delay on miss".** The BFighter members `m_HighestLandPauseTimeIfHitMisses` and `m_AccumulatedLandPauseTimeIfHitMisses` are renamed `m_HighestPauseTimeIfHitMisses` / `m_AccumulatedPauseTimeIfHitMisses`. The methods `FighterIncrementLandDelayOnMiss` / `FighterClearLandDelayOnMiss` are renamed `FighterIncrementDelayOnMiss` / `FighterClearDelayOnMiss`, and there is a new `FighterPopDelayOnMissFrame`: it returns true and takes one frame off the accumulated counter, or else off the highest one, while either is non-zero. The caller stalls a frame each time it returns true. Projectile attacks now use it: the shared projectile template gains a `_NumDelayOnMissFrames` setting, adds that many frames to the post-launch loop, and skips them again once a hit clears the counters. A projectile that hits recovers fast, and one that misses recovers slowly. — *where*: Fighter.DEObjects `BFighter` (members, `FighterIncrementDelayOnMiss`, `FighterClearDelayOnMiss`, `FighterPopDelayOnMissFrame`, projectile template) and every `B<Fighter>` that inlines the templates; Class `P<Fighter>` method lists change to match. — *port*: script-only; the port runs these scripts. [observed]
- **Projectile special timings, all with a 40-frame (Warlord rocket 45, Pyros 40) delay on miss** (old -> new frames):
  - Gargoyle Whirlwind: prelude 10->7, aim 23->18, pre-launch 23->20, post-launch 50->40+40.
  - Mantis AcidSpit: same as the Gargoyle Whirlwind. AI aim prediction constant 23->20.
  - Katana Projectile: aim 23->19, pre-launch 23->22, post-launch 50->40+40.
  - Jaguar ConcussionBolt: post-launch 60->40+40. Jaguar FireLaser takes `_AimStrength=LaserAimStrength`.
  - Pyros FireBall: prelude 5->7, pre-launch 17->18, post-launch 55->45+40.
  - Warlord LaunchRocket: prelude 5->7, aim 10->15, pre-launch 12->19, post-launch 95->45+45, total 110->90. AI aim constant 12->19.
  - Warlord LaunchGrenade: post-launch 37->45, no delay on miss.
  - In all of these, head and chest aim-follow is halved (×0.5, Warlord's chest ×0.75 / ×0.5), and the chest now turns as well as the head.
  - Force MeteorCall: 89->69 frames plus a 45-frame delay on miss, popped one frame at a time.
  - Mantis SuperSpiderBall: post-projectile 31->40 frames, with the delay pop.

  — *where*: `B<Fighter>` `Ground.<Move>` and `.Settings`. — *port*: script-only. [observed]
- **Long-evade jump distance cut.** Side-jump evades go from 1.4 to 1.25, and the back-jump evade from 1.2 to 1.05, both × the global long-evade multiplier. These are new evades from 2.095. Air.HitEvade flying time 59->63. — *where*: `BFighter` `Ground.Evade.*EvadeLongJump`, `Air.HitEvade` (copied into every fighter). — *port*: script-only. [observed]
- **Inner-block attacks disabled.** Launching any standard attack from the last 7 frames of a block was commented out (a source comment says they did not sequence well). — *where*: `BFighter` `Ground.Block.Block1` (and copies). — *port*: script-only. [observed]
- **Counters can no longer counter a "no counter" attack.** There is a new `CAttackInfo` flag, NoCounter (bit 0x80 of the flags byte at +0xc4, next to NoEvade 0x10), with natives `SetNoCounter` and `IsNoCounter`. Every fighter's `Left/RightBlockCounter.AttackableReceiveAttack` now also requires that it is not set. The Football (laser ball) sets it on its attack. — *where*: OMF `SetNoCounterCaller` 0x10713140 / `IsNoCounterCaller` 0x10713090, `DE_COMF2ManagerFactory::Init` (registration); `BFootball.Attack`; `B<Fighter>` counters. — *port*: add `CObject::CAttackInfo::SetNoCounter()` / `bool IsNoCounter()` to engine/src/game/natives_combat.cpp next to SetNoEvade/IsNoEvade (`kFlags` 0x80). [observed]
- **Carryable stealing.** The steal test now measures from the collided pickup's owner actor to the thief's focal point, not holder to thief with Y zeroed. When the holder is a fighter, 10 is taken off positive Y (per a source comment, to let shorter fighters such as Jaguar steal from Pyros). The range is still 15. — *where*: `BFighter` left/right carryable steal (copies in every fighter). — *port*: script-only. [observed]
- **Popup momentum.** When the victim rests on the floor, the opponent's Y momentum is zeroed before the momentum transfer. — *where*: `BFighter` popup state. — *port*: script-only. [observed]
- **Gargoyle.**
  - Right Kick Heavy hit window from 1.62 to 1.59.
  - Right Punch Heavy hit window 1.4–1.8 -> 1.43–1.7, streamer 1.37–1.61 -> 1.2–1.85, guide-to-victim ×1.0 -> ×1.3.
  - Left Kick Heavy transferred momentum (0,−3,0.5) -> (0,−2.5,0.4).
  - Air Right Kick Heavy recovery prelude step 0.05 -> 0.03.
  - Air LegGrab: guide 0.03 -> 0.02, a small forward/up push, frame step 0.06 -> 0.05, hit momentum z +2.4 -> +1.6 and launch (0,−4.2,1.5) -> (0,−2.7,1.5). Its block recovery Y +0.4 -> +0.2 and fall/land frames 40/30 -> 50/42.
  - Air Scream: 65 -> 58 frames, at most 6 -> 5 instances, landing 60 -> 15.

  — *where*: Gargoyle.DEObjects `BGargoyle`. Its `AnimationBank` also changed. — *port*: script/data-only. [observed]
- **Jaguar.** Right Kick Heavy 45 -> 51 frames, deepest link 50 -> 55, and its link check from frame 1.62 to 1.97. Right Punch Heavy recovery prelude 25 -> 35. Air Right Kick Light movement (1.55, −0.9) -> (1.1, −0.6), and its on-hit air recovery branch was removed. — *where*: `BJaguar`. — *port*: script-only. [observed]
- **Mantis.**
  - Right Punch Light 47 -> 57 frames, deepest link 58 -> 62.
  - InsectStrike: registers correction states and doesn't replicate on clients, and waits 30 ticks for a server correction. Its leap is NoEvade.
  - SuperInsectStrike: faster hits (Hit2 60->30, Hit3 21->17, FlipOverKick 60->44, Hit5 30->24, Hit9 34->29, Hit10 45->35), lower launches (2.3->2.0, 1.2->0.9, 1.4->1.0, 1.3->0.9), and attack trailers added.
  - Spider pre-activation 35 -> 55 frames. Fly waits 55 ticks after being thrown (non-super).
  - AcidSpit blast radius is now 15 + 0.05 × lifespan (it was multiplied into the old radius). Its projectile's HitTestAffector radius goes 5 -> 4, and the left spawn reference offset 20 -> 22.

  — *where*: Mantis.DEObjects `BMantis`, `BSpider`, `BFly`, `BAcidSpit`, `AcidProjectileActor/HitTestAffector`, `LeftAcidSpitRef`. — *port*: data-only. [observed]
- **Pyros.**
  - Right Kick Heavy links to Right Punch Heavy, and Right Kick Light links to Left Punch Light (frames 1.8/1.9–2).
  - Left Kick Light deepest link 60 -> 55, transferred momentum ×3.0 -> ×4.5.
  - Right Punch Light deepest link 50 -> 40, link window 1.78–1.8 -> 1.8–2.0.
  - Air Left Punch Heavy 65 -> 62, momentum 1.5 -> 1.4. Air Right Punch Heavy 85 -> 82, momentum −4 -> −3.
  - SuperFlameThrower 8 -> 6 ticks per flame, faster start (0.0015 -> 0.0021).
  - Right FlameThrower 80 -> 74 frames.
  - FireSpin momentum ×0.6 -> ×1.2 with Y 0.8 -> 0.6, adjust ×1.0 -> ×0.5. SuperFireSpin momentum ×0.5 -> ×0.16, spin-down 0.0002 -> 0.0005.
  - Flamethrower bits do damage ×0.64 -> ×0.57 with Y push 0.35 -> 0.12.

  — *where*: `BPyros`, `BFlamethrowerBit`. — *port*: script-only. [observed]
- **Warlord.**
  - Initial melee damage multiplier 1.1 -> 1.25. Left/Right Kick Heavy damage ×0.5 -> ×0.8.
  - SuperGrenade spreads its grenades ±8 sideways. Grenade blast radius 28 + 0.28 × power -> 24 + 0.24 × power, and the right grenade's health is 1.85.
  - Rocket block push ×1.7 -> ×0.4, delay between cluster explosions 15 -> 8, hit-test sphere 8 -> 4.
  - The rocket's smoke trail and particles were retuned (trail 50/1500/0.7/1.5/0.5 -> 75/2200/1.2/3/0.7, spawner and size values).

  — *where*: `BWarlord`, `BGrenade`, `BRocket`, `RocketProjectile/HitTestAffector`, `SmokeTrail`, the rising/explosion particle objects. — *port*: data-only. [observed]
- **Force.**
  - Vortex (gravity well): the pickup charges while held (power += 0.0015 per frame, up to 1), which scales its ring and its noise. It now enters a `PreActivation` state (60 ticks, clients wait for the server) before `Activated`.
  - Pull strength is max 0.010 -> 0.022 × a smooth 0→1→0 envelope × power (`ApplyVortexGravity` gains a multiplier argument).
  - Speaker sphere 20/80 -> 35/120, light colour retuned. Vortex particle objects renamed SmallRing….
  - ForceBomb gets a light that fades out on explosion (a new `Light` + `LightAffector`, falloff 160).
  - MeteorCall step distances 3.5 -> 2.5 and 4.5 -> 3.5, sideways 16 -> 14. The super version spawns from 17 (was 15) in 13 steps of 2.5 (was 25 × 3), as a "wide call".
  - Generate-carryable 82 -> 52 frames.

  — *where*: Force.DEObjects `BForceVortex`, `BForceBomb`, `BMeteorCall`, `BForce`. — *port*: script/data-only. [observed]
- **Chronos.** SuperCombo.Hit no longer waits 60 ticks on clients for the teleport. It registers a correction state and replicates a `Blocked` flag, so a blocked super combo plays the same on every machine. — *where*: `BChronos` `Ground.SuperCombo.*`. — *port*: script-only. [observed]
- **Arena hazards.**
  - Iceberg cruise missile damage 40 -> 34, and its intensity step 0.009 -> 0.0082.
  - SpaceStation asteroids: momentum Y 0.3 -> 0.4, block push 0.2 -> 0.3, half as many flight ticks (60 -> 30) at twice the sleep (8 -> 16), wall-hit tick 3 -> 2.
  - SpaceStation adds a `RumbleAffector` box with a `RumbleForce`, and moves the spawn parent (Y −60) and the observer teleport destination.
  - Grudge Match Arena `BladesAffectorPOS` flags cleared.
  - PowerPlant lightning wall plays its final-attack sound, and its state manager flag is set.

  — *where*: the arena files' behaviours and objects. — *port*: data-only. [observed]
- **Crowd response.** A player's attack excites the crowd less with distance: × (1 − distance/350), at least 0.2. Waiting-for-action stops while the watched creature is dead. Adrenaline rush plays a new looping noise (`AdrinalineNoise` actor, `AdrinalineActiveLoop`, faded in by a `BEffectFader`) for the focused slot, and the activation sound was replaced. — *where*: Session.DEObjects `BCrowdResponseSlot`, new objects under `OMFCrowdResponse`. — *port*: script/data-only. Check that `BEffectFader` and `NewCopy`/`Scene().Add` cover the new path. [observed]

### Bug fixes

- **Deaths and hit reactions decided on the server.** `Health()<0` -> death, `m_Status==statusDead` checks and air-evade permission now run only on the server, and the fighter enters `Ground.Death.Fall()` directly. `m_AllowAirEvade` is set after the attack data is unpacked, not before. In block code the special server/within-block correction branch was collapsed. Throw reactions no longer mark NoInterrupt/NoReplicate. Every inner-block attack registers `RegisterLastStateAsCorrection` + `RegisterLastStateNoInterrupt`. — *where*: `BFighter` (Air/Ground get-hit, LandFromFall, block, throw states) and copies. — *port*: script-only, but `RegisterLastStateNoInterrupt` is a no-op stub in engine/src/game/natives_core.cpp. It only matters once replays and network replication are real. [observed]
- **State interrupt rule for lagged states.** There is a new virtual `DE_CActor::CanStateInterrupt(old method, new method)`, Core 0x2007d340 (returns 0), vtable slot +0x130. Every later DE_CActor virtual moves by 4 bytes. `DE_COMF2Fighter::CanStateInterrupt` (OMF 0x1072a5c0) returns 1 when the current state method is `Fall(byte)` and the new one is `HitEvade(byte)`, or the current one is `Block(class CAttackInfo,bool)` and the new one is `FromBlock`, `RightBlockCounter` or `LeftBlockCounter`. `DE_CStandardStateManager::AddLaggedStateDestIndex` (Core) now asks the owner, through its behaviour executor's current method, before letting a lagged state replace newer ones, and `CheckForDiscardState` changed with it. — *port*: engine/src/game/replay.cpp (AddLaggedState, FINDINGS 176/201) once network/lagged states exist. Nothing to do for local play. [observed; exact discard semantics inferred]
- **AttackAllInSphere guard.** The early return for an attackable victim now also requires that the victim is not a live creature. — *where*: Fighter.DEObjects `BAttacker.AttackAllInSphere` / `…ExceptOwner`, inlined into the Chronos time-stop, time-acceleration and super-freeze projectiles. A per-fighter insertion point after BuildAttack was added. — *port*: script-only. [observed; game effect uncertain]
- **Get-hit cleanup.** Hit states call `GuideToBestMeleeTarget(0,0)` and `FighterRecoverFromBeingHit()`, invulnerability is cleared past frame 0.9 of the get-up, and AI attack tests run every frame the flag is set, not only when the toss test fails. — *where*: `BFighter`. — *port*: script-only. [observed]
- **HAR Master tournaments.**
  - The Gargoyle and Warlord masters now declare `m_TournamentAIDifficulty` (0), so their sub-sessions no longer see an undeclared identifier (compare FINDINGS 132 "version skew").
  - The Jaguar HAR Master starts at `matchViolenceLite` instead of `matchCinematic`, which skips its opening cinematic match. It looks like a test toggle left switched.

  — *where*: SinglePlayerFull.DEObjects `BMaster` #82, #180, #525. — *port*: data-only. [observed; intent of the Jaguar change uncertain]
- **Creature picker picks a random model.** When a fighter description is chosen with no model, `SetCreatureDescriptionGUID` picks a random model through the new native `CFighterDescription::GetRandomFighterModelDescription()`. OMF `DE_CFighterDescription::GetRandomFighterModelDescription` 0x107336b0 is a uniform `MATH::Rand` over `GatherFighterModels` 0x10733540, which is now real: it collects every FighterModelDescription master object whose fighter GUID matches and that has a model. It was a 2-instruction stub. The Fighter description class node is created in `DE_CFighterDescriptionFactory::Init`. — *where*: Session.DEObjects `BCreatureAndPilotPicker`, OMF. — *port*: add the native next to the CFighterDescription natives in engine/src/game/session.cpp and the model list in engine/src/game/descriptions.cpp. [observed]
- **Passive reference copies.** `DE_CPassiveActorReference::Create` (Core) skips references the created actor already holds when it appends the reference's own references (FINDINGS 96). — *port*: engine/src/game/creatures.cpp `CPassiveActorReference::Create`. [observed]
- **Script VM.** `DE_CPCExecuter::ContinueExecutionInternal` gains null checks in one reference-loading opcode handler (+2 tests). — *port*: engine/src/vm/executor.cpp, if a null dereference shows up there. [observed; which opcode not identified]
- **Observer camera.** `DE_CObserveCommand` (Creature) rework:
  - `DesiredViewAngle` uses the watched creatures' bounding-box centre and normalised distance.
  - `PositionWatchQuality` and `MajorMove` were rewritten (MajorMove sets +0x124 = 2).
  - `Update` uses 12 instead of 20 for one distance and adds a damping term.
  - `BObserverCamera.Tick` drives the camera's momentum from the PropelOffset values (× 0.0001 × delta) and its AI momentum multiplier goes 0.002 -> 0.2.

  — *port*: engine/src/game/observer.cpp (and the BObserverCamera script). [observed; exact formulas need a closer read]
- **Choose Fighter page.** A button gets `ClickOnKey(0x1b)`, so Escape acts on it (Back). — *where*: OMF `DE_CChooseFighterUIManager::SetupControls`. — *port*: engine/src/game/ui.cpp. [observed]

### New content

- **Rewritten in-game help.** OMFHelp.DEObjects goes from 1170 to 1294 objects. The help is reorganised into numbered chapters under "Help Tree": 01 Introduction, 02 Orientation, 03 Fighting Fundamentals, 04 In The Arena, 05 Starting a Game, 06 Arena Reference, 07 HAR Reference (one page per HAR), 08 Default Control Setup, 09 Options, 10 Replays, 11 Tips for Beginners, 12 Credits. The old pages moved to "X-Old Help". Many move descriptions were rewritten, and ShortEvade/LongEvade were renamed Ground Evade / Sidestep. — *port*: data-only. engine/src/game/ui.cpp's help tree walks it. [observed]
- **Practice Arena tips.** A "Tips" folder adds a billboarded tip image particle system, about 1.1 MB. — *port*: data-only. [observed]
- **Shared objects.** `PGameplayActorMover` / `EGameplayActorMover` (BActorMover's `RenderUpdateSetup` renamed `Setup`), a `60KTickLifeSpanExecutor` with `BActorKiller.60KTickEntry` (60 s lifespan), and a shared `BW RisingSmoke/Dust` image, which replaces Fighter's own `RisigParticleImage` (rising dust is 1.5× bigger). — *port*: data-only. [observed]
- **Removed dead content.** Flyers.DEObjects drops `BCarManager` / `PCarManager` / `CarManagerExecutor` / `CarManagerTemplate`, which nothing referenced (FINDINGS 132). — *port*: none. [observed]
- **`BPickup.PickupResetLastKnownHolder`.** New, and every pickup class carries it. No caller was seen in the changed sources. — *port*: script-only. [observed]

### Engine and rendering

- **Glow clamps its taps.** `Downsample4x4`, `SuperSampleForGlow` and `DrawGlow` (Direct3D) set ADDRESSU/ADDRESSV = CLAMP on their sampler stages. — *port*: engine/app/glow.cpp already uses GL_CLAMP_TO_EDGE. Nothing to do. [observed]
- **Info log.** `DE_GLOBAL::AddInfoText` (Core) no longer appends to `ViewableInfoText`. [observed]
- **Dialog close.** `DE_CDialog3d::Close` no longer special-cases the master control. `AttachTextAndCloseButton` gains an `int*` result argument. [observed]
- **Page viewer scroll bars** created and destroyed on demand (`UseScrollBars`, `Destroy*ScrollBar`, `AdjustViewClient`). — *port*: engine/src/game/controls.cpp, if the help page viewer needs it. [observed]
- **Class layout change.** DE_CActor gains a virtual and DE_CClassExecutorInstance a field (+0xf4, set in ObjInitContents), so several hundred Core, OMF, Creature, Session and Direct3D functions differ only in field offsets and vtable slots. [observed]
- **Module rebase.** New preferred bases: Core 0x20000000, OMF 0x10700000, Creature 0x10200000, Session 0x11000000, Direct3D 0x10300000, Input2 0x10600000, GameSpy 0x10500000, AudioPlayer 0x10000000, Particles 0x10900000. Addresses in FINDINGS (disc bases) must be rebased to compare with 2.096 and later. Particles, Input2 and AudioPlayer have no logic change. [observed]
- **Build number.** The build number pushed by `DE_COMF2Manager::Activate`, `WinSystemInit`, `UnhandledException` and `DE_CGameSpyFactory::Init` goes 0x82f -> 0x830 (2095 -> 2096). — *port*: whatever version string the front end shows. [observed]

### Network / GameSpy

- **WinSock service rework (Core).** `DE_CWinSockService` sends and receives through queued packet lists guarded by critical sections, with send and receive thread events, trash lists for reuse, and new `Disconnect` methods on `CNodeInfo`, `CSubNodeInfo` and the loopback service. `ProcessServicePacket`, `Send`, `Update` and the destructors were rewritten. — *port*: networking is not ported. Note it for later. [observed]
- **Joining.** `DE_CSessionInfo::Join` destroys local sessions, remembers `g_SessionBeingJoined` and shows a "Joining Server <name>" dialog. `JoinComplete` / `SessionBeingJoined` are new. `DE_CSession::LocalClient` takes an int, which Session's callers pass. — *port*: none yet. [observed]
- **Session browser filters.** `s_SessionInfoListShowEmpty` / `ShowFull` are BOOL registry entries, and `IsListable` counts `s_SessionInfoListTotalServers` / `TotalPlayers` and filters. GameSpy's `ProcessEvent` adds "Show Full" / "Show Empty" check buttons (with the new `DE_CAutoSizeVerticalRobot`), and the matchmaking text shows the server count, "Players: N" and "(N Filtered Out)". `DE_CButtonControl3d` gains `AddBOOLRobot` / `AddFunctionOnClick` / `NewCheck`. — *port*: Join screen is missing (PLACEHOLDERS). [observed]
- **Standard client interface.** `DE_CStandardClientComInterfaceVersion1::ProcessPacket` / `ReplicateSessionStateManagerStates` changed with the layout shift and some logic. Not read in detail. [uncertain]

### Launcher (OMFBG.exe)

- Replaced whole: 1,392,278 -> 1,404,566 bytes, timestamp 0x3fea2bc1 (2003-12-24). Same imports, and still SafeDisc-protected (`stxt774`/`stxt371`), now with an extra unnamed 0x2260-byte section before them, probably a SafeDisc revision. Not analysed. [observed]

### Rebuild-only noise

- BladePit, Canyon, Desert, DesertRings, Effects, FirePit, OMFBGGUI, Pilots, SinglePlayer, Sport and Trophy Room only have Class VMC2 recompiled: change info and renumbering, no method or source change. ArenaDescriptions only gains an external GUID-pool entry (22 -> 23). `BThrowable`, `B2TeamScoringSystem`, `BRenderUpdater`, `BGameplayUpdater`, `EMissilePickup`, `BIceberg` and `BCarryableLeft` differ only in stored text slack. [observed]

### Summary

2.096 (built 2003-12-24) reworks the "delay on miss" recovery into a general per-frame stall: `FighterIncrementDelayOnMiss`, `FighterClearDelayOnMiss` and the new `FighterPopDelayOnMissFrame`. Every projectile special now recovers about 40 frames faster on a hit than on a miss, and most fighters had frame, momentum and damage values retuned. The scripts move deaths, air-evade permission and hit reactions to the server. The engine adds `DE_CActor::CanStateInterrupt`, which fighters use to let Fall→HitEvade and Block→counter states replace lagged states. Two natives are new: a `CAttackInfo` NoCounter flag (0x80 at +0xc4) and `CFighterDescription::GetRandomFighterModelDescription`, which random model choice in the creature picker uses. The rest is data: the in-game help was rewritten into twelve chapters, Practice Arena got a tips display, and Flyers lost its dead car manager. The engine side reworks WinSock send/receive queues, adds session-browser Show Full / Show Empty filters, clamps the glow taps, adds a new class layout slot, and rebases every module to new image bases. The port needs the two natives. Everything else is data it already runs, apart from network-only items.

## v2.097 (from v2.096) — modules built 2003-12-28 (PE timestamps 0x3fee68c7..0x3fee69a2)

Files touched: 10, all in Engine: the nine `*.ModuleDLL` (MODIFY) and `OMFBG.exe` (ADD, whole file). Every file keeps its exact size (the patch header's old and new totals are equal, 15,195,487 bytes). No data files changed.

### Gameplay and balance

- None.

### Bug fixes

- **Controls panel crash guard.** `DE_CControlsPanelRobot::ProcessButtonClickEvent` (OMF 0x10760570) now null-checks the selected controller entry before dereferencing it, at the point where it reads the entry's +0x4c / +0x34 for the Assign/Remove path. — *port*: engine/src/game/ui.cpp (Options > Controls), as a defensive check. [observed]
- **Control capture crash guard.** `DE_CControlCaptureRobot::CaptureControl` (OMF 0x107608c0) null-checks the object returned by the dialog's virtual +0x204 before using it. That is the "press a key/button to assign" capture dialog. — *port*: engine/src/game/ui.cpp, defensive. [observed]

### New content

- None.

### Engine and rendering

- **Build number 0x830 -> 0x831 (2096 -> 2097)**, pushed in `DE_COMF2Manager::Activate` (OMF 0x10714430, turned into text with `_ultoa`), `WinSystemInit` and `UnhandledException` (Core), and `DE_CGameSpyFactory::Init` (GameSpy 0x1050a900, the version GameSpy reports). — *port*: the version text only. [observed]
- Every other changed function is padding or overrun noise past the function end (`DE_CUniqueActorProperty::ObjAlterChild`, `DE_CVCRControls::ObjUpdate`). AudioPlayer, Creature, Direct3D, Input2, Particles and Session are bit-for-bit rebuilds apart from timestamps. [observed]

### Network / GameSpy

- Only the reported build number changes. [observed]

### Launcher (OMFBG.exe)

- Replaced whole, same size (1,404,566 bytes), timestamp 0x3fea2bc1 -> 0x3fee69a2 (2003-12-28). Same imports and section layout. Still SafeDisc-protected, not analysed. [observed]

### Rebuild-only noise

- Everything not listed above. [observed]

### Summary

2.097 (built 2003-12-28) is a hotfix to the 2.096 executables only: no data file changes and every module keeps its size. Its only behavioural changes are two null checks in OMF's Options > Controls code: `DE_CControlsPanelRobot::ProcessButtonClickEvent` and `DE_CControlCaptureRobot::CaptureControl`, both crash guards when assigning controls. The build number 2097 (0x831) is compiled into OMF's `DE_COMF2Manager::Activate`, Core's `WinSystemInit` / `UnhandledException` and GameSpy's factory `Init`, so the version the game shows and reports comes from code, not from data. The launcher was reissued, still SafeDisc-wrapped. The port needs nothing beyond optional defensive checks in the controls page.

## v2.101 (from v2.097, via 2.100) — modules built 2004-01-05 (PE timestamps 0x3ff91d06..0x3ff93535)

The "v2097-to-v2100" patch yields build 2101: the build number compiled into the modules (Core `WinSystemInit`, OMF `DE_COMF2Manager::Activate`, GameSpy `Init`) goes 0x831 → 0x835 (2097 → 2101). 2.100 itself is folded in and never seen on its own.

Files touched: 51 (50 modified, 1 replaced whole).
- Engine (10): OMFBG.exe (replaced), AudioPlayer, Core, Creature, Direct3D, GameSpy, Input2, OMF, Particles, Session .ModuleDLL.
- Data (41): BladePit, Canyon, Chronos, City, Desert, DesertRings, Effects, Fighter, FirePit, Force, Gargoyle, Grudge Match Arena, Iceberg, Jaguar, Katana, Mantis, Music, MusicCrystalCold, MusicExecutor, MusicHexed, MusicHighOrbit, MusicK-Faktor, MusicRebellion, MusicSnakeirons, MusicSongNine, MusicWTMI, OMFBGGUI, Pickups, Pilots, PowerPlant, PracticeArena, Pyros, Session, SharedObjects, SinglePlayer, SinglePlayerFull, SpaceStation, Sport, Trophy Room, Volcano, Warlord .DEObjects.
- Build number 0x831 → 0x835 (GameSpy reports "0.1.2101"). This matches the next patch ("v2101-to-v2111") accepting these files as its 2.101 input [observed].

### Gameplay and balance
- **Chronos air kicks retimed.** Air right light kick: 50 → 38 frames (deepest link 50 → 35). Air left light kick: 40 → 62 frames (deepest link 38 → 60), hit window frame 1.4–2.0 → 1.45–1.73, danger delay factor 1.4 → 1.45, and it gains an attack streamer (frames 1.31–1.82, like the other kicks). — *where*: Chronos.DEObjects `BChronos` `Air.Attack.RightKickLight` / `Air.Attack.LeftKickLight` (+ their `.Settings` notes); compiled `PChronos`. — *port*: script only, none. [observed]
- **Gargoyle heavy attacks.** Ground left heavy punch hit window 1.61–1.95 → 1.49–1.57 (danger delay factor 1.61 → 1.49); ground left heavy kick hit window end 1.75 → 1.70; air right heavy punch transferred-momentum multiplier 1.1 → 1.7 (knockback up ~55 %). — *where*: Gargoyle.DEObjects `BGargoyle` `Ground.Attack.LeftPunchHeavy`, `Ground.Attack.LeftKickHeavy`, `Air.Attack.RightPunchHeavy.AttackerBuildAttack`. — *port*: none. [observed]
- **Katana tuning.** Ground left heavy punch 60 → 55 frames (deepest link 60 → 54), its follow-up window opens at frame 1.85 instead of 1.96; ground right heavy punch follow-up window opens at 1.6 instead of 1.8; ground left heavy kick 45 → 40 frames (deepest link 53 → 38), recovery `PreludeFromFast` 30 → 27, transferred momentum (0, 1.5, 0.1) → (0, 1.0, 0.1); ground right light kick momentum (0, −3.5, 0.3) → (0, −3.0, 0.4); ground left light kick momentum (−0.1, 0.1, 0.3) → (−0.03, 0.3, 0.3); ground right light punch hit-effect directions (−4, 0, 0) / (0, 0, −4) → (0, 2, 6) / (0, 0, 4) and momentum (0, 0.4, 0.4) → (0, 0.3, 0.1). — *where*: Katana.DEObjects `BKatana` `Ground.Attack.*` methods. — *port*: none. [observed]
- **Animation banks re-exported** for Chronos, Force, Gargoyle, Katana, Mantis (same size) and Pyros (`*AnimationBank` `AnimationSequenceGroup` contents changed by tens of bytes to ~1 KB). Likely retimed or trimmed sequences matching the script changes; not decoded per sequence. — *port*: none (data). [uncertain what changed]
- **Single player, "Foe Within" (WO tournament): easier early AI.** In `MatchFoeWithin.EarlyInMatch` the four opponents' AI difficulty offset drops from tournament + 2 to tournament + 1 (the later + 0 step is unchanged). — *where*: SinglePlayerFull.DEObjects `BSubSessionWO`. — *port*: none (`CPilot::SetAIDifficultyOffset` exists in engine/src/game/natives_ai.cpp). [observed]

### Bug fixes
- **Music no longer restarts during silence.** `BMusicPlayer.Entry` only starts the queued segment when the mood is above 1 (Silent); the `MusicPlayerAssureDefaultMusic` handler sets the mood to 2 so the default (menu) theme still starts. Before, a stopped speaker was restarted every frame even at mood Silent. — *where*: Music.DEObjects `BMusicPlayer` (`Entry`, `ProcessEvent`). — *port*: script only; check engine/src/game/music.cpp `play_default` / ui.cpp:188 still drive the speaker through the script so the new mood gate is honoured. [observed]
- **Last Man / Last Team Standing draws.** `MatchOver` no longer calls `NotifyOfWin` when the result is a draw. — *where*: Session.DEObjects `BLastManStanding.MatchOver` (and the `Templates.Misc` copy in `BStandardOMFSession`). — *port*: none. [observed]
- **Demolition (2-team) double end.** The time's-up branch now also requires `!m_SessionEnded`, so a match that already ended is not ended again. — *where*: Session.DEObjects `B2TeamDemolitionMaster.DemolitionSessionProcessEvent`. — *port*: none. [observed]
- **Rules display vs. UI stack (relates to FINDINGS 200).** `BRulesDisplay.AssureRulesDisplay` calls `CloseUIManagerStack` only when the next UI manager is not already the rules display (new native `NextUIManager()`). In Core, `DE_CAppManager::CloseUIManagerStack` (0x2019b… new build) first `DeleteThisLater`s a queued next UI manager that is not persistent, not active (+0xa0 = 0) and auto-destroy (+0xa4). — *where*: Session.DEObjects `BRulesDisplay`; Core `CloseUIManagerStack`, `DeleteUIManagerStackAutodestroyOnly`, new `NextUIManagerCaller` 0x20046d60 and `CurrentUIManagerCaller` 0x20046d80. — *port*: add natives `NextUIManager()` (returns `DE_CAppManager::NextUIManager` of the active manager) and `CurrentUIManager()` in engine/src/game/session.cpp next to `SetNextUIManager`; give `AppManager::close_stack` (engine/src/game/ui.cpp ~1820) the delete-pending-autodestroy step. This may be what lets Practice's rules display close in 2.100+ without our autopause stand-in (PLACEHOLDERS / FINDINGS 200). [observed code; effect on FINDINGS 200 inferred]
- **Script frame teardown deferred.** `DE_CClassCompiledMethod::CloseStack` no longer destroys the frame's class instances immediately (vtable +0xd4); it queues them in a new global `g_ClassInstancesNeedingDestruction` for later. Crash fix for instances destroyed while still referenced. — *where*: Core `CloseStack`, `DE_CPCExecuter::InternalExecute` / `ContinueExecution`. — *port*: engine/src/vm/executor.cpp frame teardown, only if we see use-after-free style issues; ours does not free script instances eagerly. [observed; motive inferred]
- **Scene ownership includes the root.** `DE_CScene::OwnTree` (and `DE_CStandardSessionSlot::OwnCreature`, which inlines it) now also adds the branch's root object itself to the scene's owned list and notifies it (vtable +0x34), not only its descendants. — *where*: Core/Session `OwnTree` (Session copy 0x11018540), `OwnCreature` 0x1101ac10. — *port*: none (engine/src/game/session.cpp `CScene::OwnBranch` is a no-op since our objects live until deleted). [observed]
- **Joystick re-enumeration.** `DE_CDirectInputControllerComponentFactory::ProcessEvent` clears the known-device list before re-enumerating (previously devices could be listed twice). — *where*: Input2 0x106012d0. — *port*: none (SDL input). [inferred]
- **Rules display "Press any key".** `DE_CRulesDisplayRobot::ProcessPreDraw` ignores key presses while `DE_GLOBAL::DebuggingActive`. — *where*: OMF. — *port*: none. [observed]

### New content
- **Admin "reset game" command.** New event `g_OMFSessionResetGame` ("SessionResetGame") in `BOMFEvents`. On the server, a chat text event from a slot whose client `IsAdministrator()` and reads "admin reset_game" (case-insensitive) is broadcast with `SendToSessionAndSlotsAndCreatures`. Handlers: the session elapses its timer (new `BSessionTimer.ElapseTimer`, start time −9999999, and `BStandardOMFSession.ElapseTimer`), `BScoringSystem` resets the score to 0, LMS/LTS reset their living-creature lists, and `BFighter.FighterProcessEvent` treats it like the session-begin action. `ResetTimer` moved from the two Demolition masters into `BStandardOMFSession`. — *where*: SharedObjects `BOMFEvents`; Session.DEObjects `BStandardOMFSession`, `BSessionTimer`, `BScoringSystem`, `BLastManStanding`, `BLastTeamStanding`, `BDemolitionMaster`, `B2TeamDemolitionMaster`; Fighter.DEObjects `BFighter`; every master class recompiled with `ElapseTimer` (`PMaster`, `PpMaster`, `PCinematicMaster`, `PMatchServer`, …). New native `IsAdministratorCaller` Core 0x201f6b40: a proxy client asks its true client's `DE_CClient::AdministratorLevel`; otherwise a client of a server session with +0x50 set is admin. — *port*: add `CStandardClient::bool IsAdministrator()` (engine/src/game/session.cpp, beside `CurrentClient`); multiplayer chat is not ported, so the command stays unreachable for now. [observed]
- **Raven unlock.** Finishing the "Redemption" single-player tournament now also unlocks pilot Raven (`g_PilotRaven.Unlock()` in `Cinematic`). — *where*: SinglePlayer.DEObjects `BSubSession` (guid 67f05bae, under the Redemption `Description\SessionMaster`). — *port*: none (Unlock already handled, FINDINGS 158). [observed]
- **Main menu news logo** image `DEnews` replaced. — *where*: OMFBGGUI.DEObjects. — *port*: none. [observed]
- **App URLs.** New `DE_CAppManager` virtuals `SupportURL` / `ProductURL` / `CompanyURL` (Core 0x20174630.. return null); OMF's `DE_COMF2Manager` overrides return "support.omf.com", "www.omf.com", "www.omf.com" (0x10701240..60). No data caller seen; likely the crash/notification dialog. — *port*: none. [observed; use inferred]

### Engine and rendering
- **Arena rebuilds (visual).** Desert: base geometry, its 53 texture managers, 14 multi-stage processes, 7 reflection managers and colour channels re-exported (new GUIDs); file shrinks ~3.1 MB. Sport: the fighting surface (`FightingSurfaceNEW` → `FightingSurface` with a new mesh), 67 texture managers, 24 multi-stage processes re-exported, new `EndZoneStripes` image and new `JumpPadCollision` mesh; `New MeshAffector` moved 1 unit down (y 0 → −1); many objects only re-parented / re-referenced. — *port*: none (data); our loaders already read these classes. [observed]
- **City.** The flame, train-runner-lights and water animated textures are dropped: `BCity` comments out their `SetPhase` animation and the AVIs `LargePoolAVI`, `WaterFlow1Image`, `WaterFlow2Image`, `RippleAVI`, `TrainRunnerLightsAVI`, `HolographicFlicker`, `Water` are removed; the force-field phase speeds fall from 0.0043 / 0.00623 to 0.0007 / 0.0015 per frame. The force-field AVIs (`ForceFieldImageShort/Long`) change from 32×32 Indeo 5 to 64×64 uncompressed 24-bit DIB; new `AVIScaler` scale modifier. SpaceStation: `ForceFieldHitAvi` likewise becomes a 64×64 DIB, seven unused images and `ForceFieldAVI` removed, new `TextureScaleY` modifier, `UShift2` value tweaked. — *port*: none; engine/src/de/avi.cpp already decodes 24-bit DIB. [observed]
- **Draw order of transparent surfaces.** Volcano's `LavaFace Mesh` / `MeshLowDetail` and `LavaFaceHighUnderToneMesh` get mesh +0x44 (base chunk v4's second u32, set through `DE_CMesh::SetBumpValue`, passed to `DE_CRenderMaterialElement::SetPlacementTimes` in `PostProcessRenderMeshElement`) of 25 / 25 / 35 instead of 0; Iceberg's `Fog Renderer` gets 90. Positive placement times put blended shaders in clip-volume layer 6 instead of 8 (FINDINGS 147). — *port*: read mesh base chunk v4's +0x44 in engine/src/de/mesh.cpp (currently only flags are read) and use it for the transparent layer/sort key in engine/app/render.cpp / particles_gl.cpp. [observed field; render effect inferred]
- **Per-shader enable/disable in the D3D renderer.** New `DE_CClipVolume::EnableShader` / `DisableShader` / `DisableShaders`, `DE_CGeometryPatch::AssureEnabled` / `Disable`, `DE_CD3DRenderMeshElement::SetActiveFaces` / `EnableAllShaders`; `DrawLayer(EPlacement,K,H)` (Direct3D) removes shaders queued for disabling after drawing, `DrawShaders` now draws layers through `DrawLayer`, and `RunProcess` (179 → 53 instructions) is simplified. Meshes whose active faces change no longer leave stale shaders in the layer lists. — *port*: none expected (we rebuild draw lists per frame). [inferred]
- **Statically linked D3DX updated** (D3DXShader compiler/assembler, D3DXTex, jpeg, inflate functions differ). — *port*: none. [observed]
- **Noise elements moved to Core and shared.** `DE_CNoiseElement` gets a global registry (`g_NoiseElements` moves from AudioPlayer to Core), `IsReadyForDestruction`, noise-ref disconnect notifications; speakers hold an active noise element (`DE_CSpeakerElement::SetActiveNoiseElement`); AudioPlayer drops `ReleaseDuplicateSoundBuffer` and reworks `DE_CDirectXSpeakerElement::Play` / `Activate` / `SetActiveNoise`, `DE_CDirectXNoiseElement::CheckForReload`, `DE_CDirectMusicContainer::AssureLoaded` / `ObjDestroyContents`, `DE_CStandardAudioPlayer::SetScene` / `PlayAudio` (listener now copies the whole coordinate system). Lifetime/reuse rework of sound buffers. — *port*: none (engine/app/audio.cpp has its own lifetime model). [inferred]
- **Music segments re-saved.** Every `Noise2\DirectMusicSegment` in the Music*.DEObjects files goes from noise contents v2 to v3 (28 more bytes: frequency variance, alternate chance and further zero fields) and its noise references chunk v1 → v2 (the alternates list). `MusicExecutor`'s `Executor Container` has 84 bytes changed in its DLS collection (`insh` bank/program of its first instrument and `wsmp` sample fields). — *port*: engine/src/de/sound.cpp already reads noise v3; engine/src/game/music.cpp's `reference_at(…, 2, …)` is unaffected. Check the DLS change against engine/third_party/dmusic. [observed; musical effect of the DLS edit not determined]
- **Core class layouts grew.** Many changes in Core, Particles, Direct3D and OMF are offset shifts of +4 (e.g. `DE_CClassExecutorInstance`-style members, particle renderer instance info +0x60 → +0x64) and renumbered statics. — *port*: none. [observed]
- **AI / creature code.** `DE_CDigitalOrientationPropulsionExecutor::Tick` (Creature 0x10229890) loses three `fabs` calls and its aligned frame (340 → 289 instructions); `DE_CObserveCommand::Update`, `DE_CEnvironment::ObjProcessReferences` changed by a few instructions. Not decoded; likely compiler/inlining. — *port*: none known. [uncertain]

### Network / GameSpy
- GameSpy-reported version "0.1.2097" → "0.1.2101" (`DE_CGameSpyFactory::Init` 0x1050a900). Session client interface (`ProcessPacket`, `Update`, `ReplicateSessionStateManagerStates`) changed by a few instructions, not decoded. `SetLastUIManager` gains an int argument (OMF Quit menu passes 0). — *port*: none (networking not ported). [observed / uncertain]

### Launcher (OMFBG.exe)
- Replaced whole; still SafeDisc 2 (sections `stxt774` / `stxt371`), size 1 404 566 → 1 404 563, timestamp 0x3fee69a2 (2003-12-28) → 0x3ff93535 (2004-01-05); same nine import DLLs. Not analysed further. [observed]

### Rebuild-only noise
- Every Class in the changed DEObjects files has new VMC2 change info and "finals" (recompile); BladePit, Canyon, DesertRings, Effects, FirePit, Grudge Match Arena, Jaguar, Mantis (except its animation bank), Pickups, Pilots, PowerPlant, Pyros (except its animation bank), Trophy Room, Warlord contain nothing else. Many reference / hierarchy changes in City, SpaceStation, Desert, Sport are index renumbering after removed objects. OMFBGGUI moves four images from `F.MainMenu` to `Maps`. Creature/Session/Input2/OMF `ObjAlterChild` and many caller stubs differ only in inlined template code or renumbered statics. [observed]

### Summary
The "2.100" patch yields build 2101 (0x831 → 0x835, built 2004-01-05), and is a balance and fix release: Chronos, Gargoyle and Katana get frame and knockback retuning, the WO "Foe Within" match is easier early on, and finishing Redemption unlocks Raven. Scripts fix music restarting at mood Silent, wins announced on draws and Demolition ending twice, and add an admin "reset_game" chat command backed by a new `SessionResetGame` event, `ElapseTimer` and a new `IsAdministrator` native. The rules display only closes the UI stack when it is not already queued, and Core's `CloseUIManagerStack` deletes a queued auto-destroy page — likely relevant to the unresolved Practice rules-display issue (FINDINGS 200). Visually, Desert and Sport are re-exported, City loses its animated water and train lights, force-field AVIs become 64×64 DIBs, and lava/fog meshes get placement times that move them to the earlier transparent layer, a mesh field our reader does not read yet. Engine-side, script frames defer instance destruction, scenes own the branch root, and sound buffers/noise elements are reworked.

## v2.112 (from v2.101, via 2.111) — modules built 2004-01-15

The base tree (output of the patch labelled 2.097 -> 2.100) carries build number 2101 compiled in (OMF `DE_COMF2Manager::Activate`, Core `WinSystemInit`), so it is 2.101. The patch labelled 2.101 -> 2.111 produces what the 2.111 -> 2.112 patch recognises as 2.112. This section is therefore the net 2.101 -> 2.112 change; what 2.111 did on its own cannot be separated.

Files touched: 40 (all modified, none added or deleted).
- Engine (10): OMFBG.exe, Core, OMF, Session, Creature, Direct3D, AudioPlayer, Particles, Input2, GameSpy.
- Data (30): BladePit, Canyon, Chronos, Desert, DesertRings, Effects, Fighter, FirePit, Force, Gargoyle, Grudge Match Arena, Iceberg, Jaguar, Katana, Mantis, OMFBGGUI, OMFHelp, Pickups, Pilots, PowerPlant, Pyros, Session, SharedObjects, SinglePlayer, SinglePlayerFull, SpaceStation, Trophy Room, Volcano, Warlord.

### Gameplay and balance
- **New "light" attack type, set automatically on weak hits.** `StartSimpleAttack` now ORs a Light bit into the attack-type word (+0x4c) when the damage is below 15.0. Light takes bit 4, and every later type moves up one bit: Projectile 4 -> 8, Cold 8 -> 0x10, Heat 0x10 -> 0x20, Electrical 0x20 -> 0x40, Gravitational 0x40 -> 0x80, TimeStop 0x80 -> 0x100. Standard (2) and NoReplicate (0x80000000) keep their bits. There is a new script native `CAttackInfo::IsAttackTypeLight()`. — *where*: OMF `DE_CAttackInfo::StartSimpleAttack` 0x107175a0 (now out of line), `StartSimpleAttackCaller` 0x107133f0, `IsAttackTypeLightCaller` 0x107132f0, the `Add/IsAttackType*Caller` family 0x10713250..0x107133d0, `SetupExplosiveProjectile` 0x107162a0 (writes 8, not 4), and registration in `DE_COMF2ManagerFactory::Init` 0x1070ff80. — *port*: `engine/src/game/natives_combat.cpp`. In `StartSimpleAttack(float,float)`, set the Light bit when arg0 < 15. Add `IsAttackTypeLight` and `AddAttackTypeLight` if wanted. Our bit values are internal, so renumbering them is optional, as long as the C++ (`SetupExplosiveProjectile`, line ~370) and the natives agree. [observed]
- **Unblockable attacks.** `TestForFighterBlock` now returns "not blocked" straight away when the attacking object (attack info +0x14) has a BOOL value `UnblockableAttacks` set. Two single-player matches set it on the player's creature: Raven's Redemption "Recovery" and IFL "Unleashed". — *where*: OMF `DE_CAttackInfo::TestForFighterBlock` 0x10717800, and the scripts `BSubSession` (SinglePlayer, Raven's Redemption) `MatchRecovery.InitialBeginAction` and `BSubSessionIFL` `MatchUnleashed.InitialBeginAction`. — *port*: add the check to our fighter block test in `engine/src/game/natives_combat.cpp` (grep `TestForFighterBlock`), reading the value through `World::object_value` (`natives_script.cpp:91`). [observed]
- **Get-hit sound volume now scales with damage.** `FighterPlayNoiseForAttack` used to play at a fixed volume of 1. It now plays at clamp(damage × 0.1 + 0.68, 0.82, 1). Light standard hits use the new `GetHitHighLight` / `GetHitMiddleLight` noises instead of the medium ones. — *where*: Fighter.DEObjects `BFighter` `FighterPlayNoiseForAttack`. — *port*: script-only, but it needs the `IsAttackTypeLight` native above. [observed]
- **Each fighter pitches its sounds.** `BFighter` gains the members `m_StandardSoundGetHitFrequency`, `m_StandardSoundFootstepFrequency` and `m_StandardSoundLandingFrequency` (default 1.0). Get-hit, footstep and landing sounds now use the 6-argument `PlayNoiseSphere` / `PlayNoiseSphereVolumetric` with that frequency. Overrides (get-hit / footstep / landing): Jaguar 1.25 / 1.15 / 1.1, Katana 1.35 / 1.25 / 1.2, Mantis 1.15 / 1.1 / 1.1, Pyros 0.9 / 0.9 / 0.9, Warlord 0.8 / 0.8 / 0.8. Chronos, Force and Gargoyle keep 1.0. — *where*: BFighter `Effects.StandardBiped.*`, `FighterPlayNoiseForAttack`, and the members of BJaguar, BKatana, BMantis, BPyros and BWarlord. — *port*: script-only. The 6-argument natives already pass the frequency (`natives_script.cpp:669-683`, `app/audio.cpp:190`). [observed]
- **Jaguar loses air control during two moves.** During `Ground.SuperLeap.DownStrike` and `Ground.Grapple.Hit`, before the hit frame, Jaguar used to run the full `Control.Air.Jump` checks (air attacks and tosses). Now it runs only the new `Control.Air.TestEvade`, which runs `Control.Air.GetHitFall`, the air hit-evade (roll-out double-tap). — *where*: BJaguar, plus the new element `BFighter` `Control.Air.TestEvade`. All fighter classes recompile, which adds `BFighter:Control.AirTestEvade()`. — *port*: script-only. [observed]
- **Power Hit pickup launches higher.** After the knock-away is added, any upward launch component gets +0.4 on y. — *where*: SinglePlayer.DEObjects `BPowerHit`. — *port*: script-only. [observed]
- **Bot-killer cheat damage multiplier 1000 -> 20.** — *where*: Session.DEObjects `BotKiller\DamageMultiplier` (Value\FLOAT, guid d66fd841…). — *port*: data-only. [observed]

### Bug fixes
- **`BSinglePlayerSubSession` now declares `m_TournamentAIDifficulty`** (final int 0). That resolves the version skew in FINDINGS 128: Training, Practice and Trophy Room sub-sessions used a member that their masters did not declare. The base `BeginAction` no longer raises `ERROR("Base BeginAction…")`. — *where*: SinglePlayer.DEObjects `BSinglePlayerSubSession` (members, `BeginAction`); Trophy Room `PSubSession` recompiled (its `BeginAction` shrinks from 95 to 12 bytes). — *port*: script-only. Once we load 2.112 data, our compiler (`engine/src/vm/compiler.cpp`) no longer needs to tolerate that skew. [observed]
- **`PBoulder` now has compiled code.** At 2.101 its class had no VMC2 (the BPickup/BOMFEvents skew, FINDINGS 128); at 2.112 it carries the `BBoulder`, `BPickup` and `BCarryableRight` methods. Canyon's `BoulderSpawner` is still not added to the arena tree (Canyon only changes `PShell` and `BFighterTrackingGun`), so boulders presumably still never spawn. — *where*: Pickups.DEObjects `PBoulder`. — *port*: none. [observed; the spawn conclusion is inferred]
- **Null-client crash guard.** `DE_CStandardClient::LaunchRemoteUIManager`, `LocalUIManagerActivated`, `LocalUIManagerUpdate` and `LocalUIManagerComplete` now check the client's +0x30 before forwarding to it. — *where*: Session 0x11002dc0..0x11002e60. — *port*: `engine/src/game/session.cpp` if we model remote clients (we do not yet). [observed]
- **DirectX failures are no longer fatal.** A DirectSound error when a speaker element activates, and a swap-chain creation failure, no longer call `gNotifyNewFatalError`. — *where*: AudioPlayer `DE_CDirectXSpeakerElement::Activate` 0x100042a0; Direct3D `DE_CD3DViewport::CreateSwapChain` 0x10339660. — *port*: none (we have no such paths). [observed]

### New content
- **Choosing the starting match in single player.** The session-settings ("Choose Cast") page gains a **Starting Match** combo box. It is filled from the master description's new `Matches` string value, split on newlines. The list is cut back to the matches that are unlocked, with lock id 0x10063 + index, unless debugging is active, and the box appears only when more than one is available. `DE_CSessionSettings::UIManagerComplete` writes the choice to the settings' description as INT `StartingMatch`. `BSinglePlayerSession` then starts at that match when `StartingMatch` > 0, and on each new match calls `Session().MasterDescription().UnlockSinglePlayerMatch(m_MatchNumber)`. That native is new (`CDescription::UnlockSinglePlayerMatch(uint MatchNumber)` -> description vtable +0x13c with id = n + 0x10063). Every tournament's `BMaster` makes the same call. `Matches` values were added to 6 SinglePlayerFull descriptions and 6 SinglePlayer descriptions. — *where*: Core `DE_CChooseCastRobot::DE_CChooseCastRobot` 0x201e9970 (global `g_StartingMatch`), `DE_CSessionSettings::UIManagerComplete` 0x201e8d00, `UnlockSinglePlayerMatchCaller` 0x201f3eb0 (registered in `DE_CDescriptionFactory::Init` 0x201f39f0); scripts `BSinglePlayerSession` (`WaitingToStartMatch.Update`, `StartNewMatch1`) and every `BMaster`. — *port*: register the native in `engine/src/game/natives_script.cpp`, next to `CDescription::Unlock(dword)` (line ~164), using `unlock_description` (`descriptions.cpp`). Add the combo box to the Choose Cast page in `engine/src/game/ui.cpp`. [observed; that vtable +0x13c is `Unlock` is inferred]
- **A session's values fall back to its description.** `DE_CObject` gains the virtual `ObjFindValueByName` (vtable +0x108, so every later DE_CObject slot moves up by 4). Every `Get*Value` / `Set*Value` / object-reference value accessor now goes through it. `DE_CSession` overrides it to look in its description (+0x38) first, then in its own children. That is how `Session().GetINTValue("StartingMatch")` reaches the settings value. — *where*: Core `DE_CObject::ObjFindValueByName` 0x2003c710, `DE_CSession::ObjFindValueByName` 0x201fdfa0, and the accessors 0x2003c780..0x2003d350. — *port*: in `World::object_value` (`engine/src/game/natives_script.cpp:91`), when the object is a session, look up its description's values before its own. [observed]
- **Single-player tournaments rewritten.**
  - *Ultimate Warrior* (SinglePlayerFull, `BSubSession` 57d9773e): matches Banter, Deadly Ground, Phoenix Rising and Rebels are removed; The Fix, Sibling Rivalry and Payoff are added. The per-match Early/Late phases merge into `EntireMatch`. The `BMaster` enum and its development `m_StartingMatch` follow.
  - *IFL Finals* (`BSubSessionIFL`): match Favor becomes Imbalance (+2 AI difficulty for slots 3 and 4), and Unleashed gains an `InitialBeginAction` (unblockable attacks for the player, AI −2).
  - *North American Open* match 3 (SinglePlayer `BSubSession` bedff273): opponents change from Force/Ciaran and a random fighter with Cho Dox to random fighters with Suki and Velocity, the arena is random instead of Desert, and the player's Power Punch drop-on is gone.
  - *Raven's Redemption* (`BSubSession` 67f05bae): Recovery adds unblockable attacks for the player and AI −2. Between-round AI offsets are reworked (+2 / +3 where +1 was used). The Power Hit drop-on delay goes 3000 -> 1000 ms.
  - All tournaments rewrite or prune their dialogue (the cheat/hack jokes are gone), and `BSinglePlayerSubSession` adds per-round team-score tracking (`m_TeamScores`, `m_LastRoundBestTeamPerformer`) for 2-team rounds.
  - Smaller text edits: Escalation/Multiplicity, Transition, HAR Master, World Open.
  — *port*: script-only. [observed]
- **Hit and block shockwave rings.** `StandardBlockEffect` and `StandardDamageEffect` each gain a `ShockwaveRenderer` (Mesh\ParticleRenderer\Ring), with new `BlockEffectShockwave` / `HitEffectShockwave` value folders (inner and outer ring size, colour, fade). — *where*: Effects.DEObjects. — *port*: data-only; it needs our Ring renderer (`engine/src/de/particles.cpp`, `app/particles_gl.cpp`) to handle these values. [observed]
- **New and remixed get-hit sounds.** `GetHitHighLight` and `GetHitMiddleLight` are added. `GetHitLight_02` is removed; `GetHitLight_03` is renamed `GetHitHighMedium2` and `GetHitMiddleMediumNoise2_16bit` is renamed `GetHitMiddleMedium2`. The samples of `GetHitHighMedium` and `GetHitMiddleMedium` are replaced. Noise header floats change: ShortBluntHit and ShortRakingHit (0.9, 1) -> (1, 1); GetHitHighMedium (0.9, 0.9) -> (1, 0.95); GetHitMiddleMedium (0.9, 0.9) -> (1, 1). — *where*: SharedObjects.DEObjects. — *port*: data-only. [observed; that the two floats are volume and frequency is inferred]
- **Help rebuilt.** OMFHelp.DEObjects is reorganised: 196 objects removed and 594 added (topics regrouped into Main / Title / Body / Final-creator folders). — *port*: data-only. [observed]
- **Cheats.** The session console accepts `elapse_timer` / `elapsetimer` (runs `ElapseTimer`) and underscore aliases `kill_ai`, `kill_enemy_team`, `kill_my_team`, `boom_ai`, `kill_me`. — *where*: Session.DEObjects `BStandardOMFSession` `SessionProcessEvent`. — *port*: script-only. [observed]
- **Pilot "Lt. Reece" is renamed "Reece"**, and his `PilotDescription` text is edited. — *where*: Pilots.DEObjects (actor and description). [observed]
- The unused `PSlidingText` / `BSlidingText` (FINDINGS 128) is removed from Effects. [observed]

### Engine and rendering
- **Speaker frequency becomes a plain multiplier, capped at 1.5×.** `DE_CSpeaker::SetFrequency` now stores a float at +0xb0, where it used to set a FLOAT value reference at +0x9c. `SetActiveNoise` sets the playing rate (+0xb4) to (1 + CenterRand(noise variance)) × speaker frequency × noise frequency, clamped to at most 1.5. `Frequency()` returns +0xb4, or 1 with no noise. Before, the speaker factor was applied at read time and nothing clamped the result. `NewNoisePlayerSphere` stores the call's frequency in +0xb0 before activating the noise. `ObjProcessReferences` and `AutoActiveNoise` now call `SetActiveNoise` rather than inlining it. — *where*: Core `DE_CSpeaker::SetFrequency` 0x2021dbb0, `Frequency` 0x2021dbc0, `SetActiveNoise` 0x2021d9f0, `NewNoisePlayerSphere` 0x2021dbf0, `ObjProcessReferences` 0x2021d490, `SetActiveNoiseCaller` 0x2021cb00. — *port*: clamp the product to 1.5 in `engine/app/audio.cpp:191`. This matters now that fighters pitch up to 1.35. [observed]
- **Class layout / vtable shift (noise for diffing).** The new `DE_CObject` virtual shifts every later vtable slot by 4 in all modules. That accounts for most of the ~1750 "changed" functions in Core and ~380 in OMF. Other noise: line numbers in asserts, renumbered statics, register allocation. — *port*: none, but older addresses and vtable offsets in FINDINGS are pre-shift. [observed]
- Direct3D's statically linked D3DX (shader compiler, texture codecs, `D3DXVec3Project`) is a newer D3DX build. Not port-relevant. [observed]
- Core `DE_CAppInterface::EnsureMouseCursor` 0x20023db0 also loads the standard system cursors (IDC 0x7f00..0x7f04). `NewINTComboBox` 0x200e0000 is reworked to support the new combo box. [observed; the exact purpose is uncertain]
- Creature, Particles and Input2: rebuild only.

### Network / GameSpy
- No behavioural change found. GameSpy `Init` and Core WinSock functions differ only in registers and assert line numbers. [observed]

### Launcher (OMFBG.exe)
- Still SafeDisc-wrapped (`stxt774` / `stxt371`), so its code was not analysed. Timestamp 2004-01-05 (2.101 tree) -> 2004-01-15, size 1404563 -> 1408662. The decoy single imports from Core and MSVCRT change (`EndEdit` -> `FindExactAgent`, `_itoa` -> `_ultoa`). The rest of the import table is unchanged. [observed]

### Rebuild-only noise
- Most changed DEObjects only recompile their classes (VMC2 change-info, index shifts): BladePit, Desert, DesertRings, FirePit, Grudge Match Arena, Iceberg, OMFBGGUI, PowerPlant, SpaceStation, Volcano, Canyon (`PShell`), and the Chronos/Force/Gargoyle classes. Many Behavior "element changed" entries are only stale bytes in the stored source text (e.g. BChronos/BForce/BGargoyle `Ground.Block`, BFootball `Thrown`, BFighterTrackingGun `FireShell`).

### Summary
2.101 -> 2.112 (net of 2.111) is mostly a single-player and audio patch. The single-player work: tournaments rewritten, a Starting Match chooser backed by per-match unlocks (`UnlockSinglePlayerMatch`, lock ids 0x10063 + n), and session value lookups falling through to the session description via a new `DE_CObject::ObjFindValueByName` virtual. Combat gains an automatic "light" attack type (damage < 15), which renumbers the attack-type bits, plus a creature `UnblockableAttacks` flag honoured by `TestForFighterBlock`. Get-hit sounds become damage-scaled and per-fighter pitched, and speaker frequency is capped at 1.5×. The base sub-session script now declares `m_TournamentAIDifficulty` and `PBoulder` compiles, fixing two version skews from FINDINGS 128. For the port, the engine work is the new natives (`IsAttackTypeLight`, `UnlockSinglePlayerMatch`), the light bit in `StartSimpleAttack`, the unblockable check, the session value fallback, the 1.5 frequency clamp and the Starting Match UI; the rest arrives with the data.

## v2.123 (from v2.112, via 2.121) — modules built 2004-01-24 (PE timestamps 0x4012d993..0x4012da57; v2.112 was 2004-01-15)

2.121 itself cannot be reconstructed: the patch labelled v2112→v2121 already produces files the v2121→v2123 patch accepts as its *new* versions (every entry "already up to date"), so this section is the net change 2.112 → 2.123. The compiled-in build number (OMF `DE_COMF2Manager::Activate`, Core `WinSystemInit`) moves 2112 → 2123.

Files touched: 49 (39 `.DEObjects`, 9 `.ModuleDLL`, `OMFBG.exe`); none added or removed.
- Data: ArenaDescriptions, BladePit, Canyon, Chronos, ChronosModel1, CinematicCharacterCollection, Desert, DesertRings, Effects, Fighter, FirePit, Force, ForceModel1, Gargoyle, GargoyleModel1, Grudge Match Arena, Iceberg, Jaguar, JaguarModel1, Katana, KatanaModel1, Mantis, MantisModel1, OMFBGGUI, OMFHelp, Pickups, Pilots, PowerPlant, Pyros, PyrosModel1, Session, SharedObjects, SinglePlayer, SinglePlayerFull, SpaceStation, Trophy Room, Volcano, Warlord, WarlordModel1.
- Engine: AudioPlayer, Core, Creature, Direct3D, GameSpy, Input2 (rebuild only), OMF, Particles, Session (layout only), OMFBG.exe.

### Gameplay and balance
- **Katana's right-hand toss releases later**: `m_GroundRightTossReleaseTime` and `m_AirRightTossReleaseTime` 0.3 → 0.6 — *Katana.DEObjects / BKatana (members)* — *port*: data only, no engine change. [observed]
- **Warlord walks and runs slower on screen**: `m_RunForwardAnimationSpeedMultiplier` 0.91 → 0.84, `WalkForwardAnimationSpeed` 0.016 → 0.014; the RightKickMedium hit-frame literals change from 3 to 3.0 (same value) — *Warlord.DEObjects / BWarlord* — *port*: data only. [observed]
- **Gargoyle's win and lose poses are longer**: `Ground.Winning` NumFrames 150 → 240, NumWaitFrames 50 → 0, PreludeStep 0.1 → 0.5; `Ground.Losing` NumFrames 180 → 290, NumWaitFrames 50 → 25. Its `AnimationBank` grows (486496 → 489992 bytes), presumably the new longer sequences — *Gargoyle.DEObjects / BGargoyle* — *port*: data only. [script observed; which sequences changed in the bank: inferred]
- **Pyros' lose pose wait cut**: `Ground.Losing` NumWaitFrames 480 → 80 — *Pyros.DEObjects / BPyros* — data only. [observed]
- **Animation banks re-exported** for Chronos (+1620 bytes), Jaguar (+2136), Pyros (+384), Warlord (−144) and Mantis (same size, contents differ) — *\<Fighter\>.DEObjects / \*AnimationBank* — *port*: data only (engine/src/de/anim.cpp reads them unchanged). Which sequences changed was not determined. [observed / uncertain]
- **Camera interest of fighters and projectiles reset to 0**: the creature contents float at +500 (DE_CCreature's own global interest, read by `ObjProcessContents`) goes 1 → 0 on the Chronos and Mantis fighters and 0.2 / 0.5 → 0 on Chronos' TimeStop/TimeAcceleration crystals and SuperFreeze, Force's bomb, vortex and super-vortex projectiles, Mantis' fly and spider projectiles; Force's `BForceVortex`, `BForceBombPelet`, `BSuperVortex` no longer call `SetGlobalInterest`; `BFighter`'s death sets `InterestLevelDead` before hiding the fighter instead of after the death event. Net effect: the observer camera stops chasing projectiles and judges fighters only by the scripts' interest levels — *Chronos/Force/Mantis.DEObjects, Fighter.DEObjects / BFighter Ground.Death & Air.Death* — *port*: engine/src/game/runtime.h `GameObject::global_interest` starts at 0 and is never loaded from the contents; to be faithful for every version, load +500 from the creature contents (Creature `ObjProcessContents`, v2.112 data has non-zero values), which with 2.123 data gives 0. [observed]
- **Escalation tournament reworked** (single player, `SinglePlayerFull` / Tournaments\Escalation\BSubSession): matches Searching, Desperation and TheUltimate replace the early/late-in-match taunt states with PlayerWon / PlayerLost / MatchComplete outcomes decided on team score; rosters change (Searching: Brick → Dawn; Desperation: Phoenix (Velocity, Professor) → United Alliance (Honey, Dutonicus); TheUltimate: Judgement + Milano (Illuminari) against Zyris, Ibrahim, Gunnar (Saber), 2v3 instead of 1v2), and the opponents no longer get the High-Power Acid and Kamikaze drop-ons (only the player does). The Escalation description text and the master's `Matches` string change too — *port*: data only. [observed]
- **Full tournaments get harder**: `BMaster` (SinglePlayerFull) `m_TournamentAIDifficulty` 0 → 2 — data only. [observed]

### Bug fixes
- **Replicated hit damage was ignored**: `DE_COMF2Manager::DE_CAttackInfo::UnpackForGetHitEffect` (OMF 0x10717110) read the damage byte but then used the receiver's own damage clamped to 0..50; 2.123 sets damage = byte / 255 × 50. `PackForGetHitEffect` is unchanged (still writes trunc(clamp(damage, 0, 50) × 255) as one byte), so the round trip is still lossy — *port*: engine/src/game/natives_combat.cpp `UnpackForGetHitEffect` (currently reproduces the old bug on purpose) should use the unpacked byte for 2.123. [observed; whether the pack byte wraps as written: inferred]
- **Fighters without an input manager no longer crash**: new `DE_COMF2Fighter::InputManager()` (OMF 0x1072fd70) returns null when +0x140 is null; `GetInputState`, `GetWalkAction`, `GetRunAction`, `GetJumpAction`, `GetPreJumpAction`, `Tick`, `RegisterGetHitState` and the script callers `IsJoystick*`, `Is*ButtonUp/Down` now check it — *port*: engine/src/game/controls / natives (input queries) should return "not pressed" for a fighter without controller instead of dereferencing. [observed]
- **Uninitialised fields**: `DE_CAttackTrailer::ObjInitContents` now zeroes +0xdc, `DE_CFighterPhysics::ObjInitContents` +0x188, `DE_CObserveCommand::ObjInitContents` (Creature) +0x124 (the observer's best-quality value; engine/src/game/observer.cpp already assumes 0) — *port*: nothing. [observed]
- **Debris and explosion effects delete safely**: `BExplosion` and `BPreDestructionEffect` (Effects) use `DeleteThisLater` instead of `DeleteThis` in their render ticks — data only. [observed]
- **Particle renderers / point streamers hold their render mesh elements by reference**: per-instance info classes in Core and Particles gained `DE_CSingleReferenceMeshElement` members and `NotifyMeshElementInvalid/Disconnect`, so a destroyed mesh element no longer leaves a dangling pointer (Core 0x201c9600.., 0x201d99d0..; Particles 0x1090fa10..) — *port*: nothing (no raw-pointer ownership there). [observed; crash it fixed: inferred]
- **Reference resolution skips system copies**: `DE_CTransferAgent::ExtractReference` (Core) now takes the newest *non-system* object created for a local object info (new `DE_CTransferObjectInfo::LastNonSystemObject`), checking `ObjIsSystem`/`ObjIsMaster`, rather than the newest one — *port*: engine/src/de/objects_file.cpp / runtime object lookup resolves to the loaded object directly; no change expected. [observed; motivation (precached copies) inferred]
- Script tidy-ups: `BFighter`'s debris image replacement uses `else if` (no double test), many `TRACE` calls commented out (fighter AI counters, projectiles, Mantis), comment text removed from `AttackTemplates`; single-player commentary pronouns fixed ("his" → `<Slot0SexHis>`), trailing double spaces removed, `BPilotQuote` forces the observer camera even without a slot creature, `BSinglePlayerSubSession` re-enables an `ERROR("Match Complete")` trace — data only. [observed]

### New content
- **Get-hit debris ("bits")**: Effects.DEObjects gains `GetHitBitsParent` with six physics pieces (`Piece1..6`, each a sphere affector + mesh + simple physics + smoke point streamer + `ExplosionActor`), `GetHitBitExplosion`, `BitExplosionParticleColor(Multiplier)`; fighter models reference it (each \*Model1 file's external GUID pool +1). `BFighter.ResolveFighterMembers` reskins the pieces with the fighter's `RGBMain` / `ReflectionMask` and precaches them; `Effects.GetHit` (high-detail geometry only) accumulates `damage × RenderQuality()` and throws one random piece per 11 points from the hit point with a randomised momentum (length clamped 0.05..1.2). The damage spark spawn's third argument goes 290 → 360. `BStandardDebris` now spins from entry, explodes when slow (momentum < 0.04) as well as after bounce damage, lives 20 s (was 10) and deletes 4 s after exploding — *port*: needs natives `CActor::float RenderQuality()` (new in Core: 0x20095260 / caller 0x200924b0; returns 0 without a renderer, else clamp(120 / max(distance(actor, camera) − 100, 1), ≤ 1) × renderer +0x78) and `ReplaceWithThenDie` (not yet in engine/src/game/natives_core.cpp); `CScene::PreCacheObject` can stay a stub. [observed; renderer +0x78 = the detail/quality setting: inferred]
- **Front-end GUI rework** (OMFBGGUI.DEObjects, 1964 → 1583 objects, 21.7 MB → 12.0 MB): the electric-arc, spark, bubble and "server browser test" effects are gone (≈380 objects removed: textures, particle generators, folders); buttons light a glass panel instead (`LightedGlass_Back/Next`, `LightingGlass` activated on `GUIEnter`, deactivated on leave/select; rotators and spark spawners commented out in `BButton`/`BCommonButton`); new frame, colour box, glass box and screen-haze meshes and new `VE.Frame/MatchName/MatchScreenShot/NextTextBox/Progress/CommentaryText` actors. **Transition doors become an iris**: `BDoorMover` instances 16 copies of `g_IrisLeafActor` and drives them along `g_IrisLeafPath` (an AnimationStreamPlayer) with the door position, each rotated by t × 22.5°; slide-in takes 410 ms (was 350) and the slide-out no longer moves the doors (only the iris) — *port*: data only, provided `CAnimationStreamPlayer::SetTime(CActor,float)` (engine/src/game/natives_transform.cpp) and the GUI scene handle the copies; check engine/src/game/ui.cpp's transition wiring against the new door actors. [observed]
- **DesertRings moon and arena textures replaced** (new `Moon` actor/mesh/image, lightmaps and trim images removed; 1.5 MB smaller); **SpaceStation's fighting-surface collision mesh refined** (`FightingSurfaceCollision` contents 11724 → 30408 bytes); Chronos drops four duplicate `RGBMain`/`ReflectionMask` images (1.5 MB smaller) and re-encodes `CrystalReflectionMask`; OMFHelp re-encodes four help images; the \*Model1 `LODSwitcher` meshes (checked for Jaguar; the other models change by the same 28 bytes) clear bit 0x1000 of the u32 after the name (0x105e → 0x5e; meaning unknown) — *port*: data only (engine/src/de/scene.cpp, mesh.cpp, image.cpp). [observed; LOD field meaning uncertain]
- **Two-team demolition ends properly**: `B2TeamDemolitionMaster` renames `NotifyOfWin` → `NotifyWinningTeam`, notifies the winning team on `g_OMFSessionGameComplete` and gains an `EndingSession` state; `BStandard2TeamSession.NotifyWinningTeamOfWin` and the template `BStandardOMFSession Templates.States.2TeamEndingSession1` (with `Update`, `IsGameComplete`) are added — Session.DEObjects — data only. [observed]
- **Single-player matches unlock when started**: `BSinglePlayerSession` calls `Session().MasterDescription().UnlockSinglePlayerMatch(m_MatchNumber)` before starting a match — *port*: native `UnlockSinglePlayerMatch` (Core; absent in 2.090, present by 2.112) is missing; implement in engine/src/game/descriptions.cpp alongside the `DLock` registry records. [observed]

### Engine and rendering
- **Ambient sounds follow the listener**: `DE_CScene::PlayNoiseAmbient` (Core 0x20076b10) now places its 1e7-radius sphere at (0, 0, −10000) and, when the scene has a current renderer with a camera (+0x160 → +0x58), parents it to the camera (vtable +0xe0); before, it sat in the world at (10000, 10000, 0). It is therefore centred on the listener instead of panned — *port*: engine/src/game/natives_script.cpp already plays ambients non-positionally, which matches 2.123. [observed]
- **No door transition on the first page**: Core `_m_FirstTransition` (starts 1) gates `DE_CAppManager::IsTransitionNeeded` (0x201a0820), `TransitionUIManager` (0x201a0910, cleared when the first transition completes) and `UpdateUI`, and `BeginUI` becomes empty, so the launch → main-menu page change closes nothing and only the first END (the iris opening) plays (FINDINGS 225) — *port*: engine/src/game/ui.cpp `transition_ui` / `update`. [observed]
- **Observer camera retuned**: `DE_CObserveCommand::Update` (Creature 0x10223610) uses (1.1 + 0.5 × timer / 15000) × quality < best instead of (1 + …) for a major move, and clamps a negative major timer (+0x120) to −1 after the test — *port*: engine/src/game/observer.cpp (line with `(1 + 0.5f * (major_timer / 15000))` and the `major_timer` bookkeeping). [observed]
- **Navigation area search widened**: `DE_CNavigationLink::CPath::GetPathAffector` (Creature 0x1020ef70) keeps growing the test sphere by 100 up to 1000 when the affectors it touches are not on the path, instead of giving up — *port*: engine/src/game/navigation.cpp `Navigation::area_at` (an approximation today: doubling to 500). [observed]
- **Precache**: `DE_CStandardRenderer::PreCacheObject` (Direct3D 0x1030c140) calls the object's own `PreCache` (new `DE_CStandardMesh::PreCache`, Core 0x201355f0; `DE_CTextureManager::PreCache` filled in) and the "Image Needs to be PreCached" warning in `EnsureRenderTexture` is removed — *port*: nothing. [observed]
- **Rules display text colour**: `DE_CRulesDisplayRobot` constructor (OMF 0x10764380) draws the match name, commentary and next match in (0.8, 0.9, 1) instead of (1, 1, 0.65), (1, 0.75, 0.8) and (0.8, 0.8, 1) (FINDINGS 231) — *port*: engine/src/game/ui.cpp. [observed]
- **D3DX relinked**: the D3DX shader compiler/assembler, JPEG and inflate code in Direct3D changed throughout (a newer DirectX SDK library) — no port impact. [inferred]
- **Script VM/compiler**: Core `DE_CPCExecuter::ContinueExecutionInternal`, `DE_CPCNode::Link/CanCast/VMCastToType/VariableSize` and the parser changed, but the opcode range (0..0x13e) and instruction names are identical; no behavioural difference was attributed — *port*: none known (engine/src/vm). [uncertain]

### Network / GameSpy
- GameSpy.ModuleDLL: the QR2 / server-browsing SDK code was relinked (70 helper functions now carry COFF function records; `DE_CGameSpyFactory::Init` only moves). `DE_CSessionInfo::JoinComplete` gains an int argument (Core 0x20205a30) — *port*: GameSpy stays a stub. [observed; whether the SDK version changed: uncertain]

### Launcher (OMFBG.exe)
- Still SafeDisc-wrapped (`stxt774`, `stxt371`); not decrypted. Timestamp 0x4006b288 → 0x4012da57, size 1408662 → 1404565, same nine imported DLLs. Nothing further determined.

### Rebuild-only noise
- Every module rebuilt; build number 2112 → 2123. `DE_CActor` gained a virtual (`RenderQuality`), shifting vtable slots by 4 (85 Creature, 87 OMF, 6 Session functions differ only in offsets); `ObjAlterChild` templates, jump tables and register allocation differ across modules; AudioPlayer gains `DE_CReferenceTarget` virtual stubs; Core's zlib functions now have COFF records. Many Class `VMC2` blobs differ only in change info; most fighters' `Ground.Evade.RightEvadeLong` element differences are uninitialised bytes after the source, not edits.

### Summary
2.123 (built 2004-01-24; 2.121 is not separately recoverable) adds get-hit debris: fighters throw reskinned physics bits from the hit point in proportion to damage scaled by a new `CActor::RenderQuality()` native (camera distance and detail), and ambient sounds are now parented to the camera. It reworks the front end visually (iris transition doors, lit glass buttons, electric effects removed, no transition on the first page) and retunes the observer camera (fighter/projectile base interest 0, major-move factor 1.1, timer clamp). Balance changes are small (Katana toss release 0.3 → 0.6, Warlord walk/run animation speed, Gargoyle/Pyros win/lose timing, tournament AI difficulty 0 → 2, Escalation tournament rosters and outcomes). Bug fixes cover replicated hit damage (`UnpackForGetHitEffect` now uses the packet), null input managers, uninitialised fields and dangling particle mesh elements. The port needs `RenderQuality`, `ReplaceWithThenDie` and `UnlockSinglePlayerMatch` natives, observer/navigation/first-transition tweaks, and the creature +500 interest loaded from contents.
