# Shadow

Shadow, the One Must Fall: 2097 robot, made from the user's model `blender/shadow-model.fbx` and imported with the editor's robot import onto **Katana's moveset**, with his own 2097 stance and special moves.

## Moves
Shadow fights with Katana's basics (punches, kicks, throws, evades, the rising blade, razor spin, grapple, super rising blade and the block counters) and adds his 2097 specials. "Back" and "Forward" are the stick pulled away from and pushed towards the opponent; either punch or kick button works.

| Move | Input | What it does |
|---|---|---|
| **Shadow Punch** | Back, Forward + Punch | Shadow throws a projection of himself: it lunges at the opponent and uppercuts on arrival (heavy damage, pops up). Replaces Katana's blade projectile. |
| **Shadow Kick** | Back, Forward + Kick | The projection flies at the opponent in a side kick, further and faster (heavy damage, knocks down). |
| **Shadow Grab** | Double-tap Back + Punch | The projection leaps up, drops on the opponent and holds them for about a second and a half (light damage, fills the stun bar). |
| **Shadow Barrage** (super) | Double-tap Back + both Punches, one energy bar | Three projections rise around the opponent and strike in turn; the last one destroys a beaten robot (2097's scrap and destruction). |

His moves are also in the in-game Help, under "6. HAR (Robot) Reference" > Shadow (Quick Reference, Special Attacks, Super Attacks), in the shipped robots' notation; the pages come from `help.txt`.

The projections are only as real as Shadow's concentration: they vanish the moment Shadow is hit, and what strikes one is dealt to Shadow himself. Shadow is open while he casts. The computer pilots use all four (the grab most, close in; the punch and kick at range; the barrage with a full energy bar).

## Files
- `blender/shadow-model.fbx`: the source model. It is a single AI-generated mesh (Tripo): 5,406 triangles, one UV layer, no texture, no rig, no animation; 1 m tall, in a T-pose, facing -Y.
- `blender/prepare_shadow.py`: turns the FBX into the importable model, in Blender (headless):
  - scale: 3.1 m tall (Katana stands about 3 m), feet on the floor, facing kept;
  - mesh: all 5,406 triangles of the FBX (Katana's LOD0 has 2,528; the import generates LOD1 and LOD2 at Katana's 1,262 and 748). `--tris N` decimates with symmetry instead;
  - armature: Mixamo bone names in the T-pose, at joints measured from the mesh (the crotch where the legs actually join, knee pads, ankles, the waists at elbow and wrist, the neck). The crotch is taken from a 1.5 cm band about the midline: the inner thighs come within 4 cm of it 15 cm lower, which put the hip joints there before (FINDINGS 760);
  - weights: Blender's automatic weights. They fail on this mesh (holes and overlapping shells), so the script weights a watertight voxel remesh instead and transfers the weights back. The loose studs then follow the bone nearest them rigidly, and no limb takes weight from the other side (below the crotch not even at the midline);
  - texture: painted per texel in the model's own UVs: silver armour, black-and-white ribbed limbs and waist (as on the 2097 sprite), a dark face with a glowing visor, cyan studs, ear pods, claws and blade feet. `--texture file.png` uses a texture laid out in the same UVs instead;
  - materials: six, all sharing the one texture, each with a `de_paint` custom property and metallic for the reflection. Armour takes the pilot's first colour, limbs the second, hands the third; face, studs and blades keep their own colours;
  - glow: the studs, visor, ear pods and the blades' bright edges are emissive (`ShadowGlow.png`, the glTF emissive texture of the Glow and Blade materials); the import draws them into the game's glow layer;
  - the "Stealth" paint job (`ShadowDark.png`), the 512 × 128 Choose Creature picture (four staged poses) and turntable and pose renders.
- `blender/animate_shadow.py`: his own animations, keyframed on that rig after the 2097 sprites (FINDINGS 744): `Standing` (the deep, wide side-on stance, replacing Katana's: chest to the side, the left side leading, the front knee bent forward, the rear leg reaching back, the front claw forward low and the rear claw out behind), `ShadowCast`, `ShadowGrabCast`, `ShadowSummon`, and one-pose `Ghost*` sequences the projections are posed from. Arms are given as directions, the torso as turns, the legs by where each ankle goes from its hip (two-bone IK) and the feet flat; each key plants the lower foot.
- `script/shadow.bgs`: the moves, a script patch for his copy of Katana's behaviors (`BKatana`: the casting states, inputs and AI choices; `BProjectile`: the projection), applied with `--har-script`. It holds only Shadow's own code.
- `help.txt`: his Help topics (`--har-help`, docs/EDITOR_SPEC.md §5.6): his panel with the Choose Creature picture, the quick reference and one page per special and super, in the shipped pages' notation; the Special and Super Attacks topics show the shipped generic pages. Inputs checked against `script/shadow.bgs` and Katana's behavior (only the blade projectile, Sonic Blade, was replaced).
- `blender/af_sprites.py`: extracts the sprite frames from 2097's `FIGHTR*.AF` files, which were used as references.
- `blender/af_moves.py`: prints an `.AF` file's header and move table (inputs, animation strings, damage, stun, successors), as OpenOMF reads it; the source of the research in FINDINGS 741-746.
- `source/`: what the import reads:
  - `shadow.glb` and `shadow.blend`;
  - `ShadowColours.png` (the texture), `ShadowGlow.png` (what glows) and `ShadowDark.png` (the Stealth paint job);
  - `choose_creature.png`;
  - `ShadowGlow.png`: what glows;
  - `mapping.json`: what the import chose (15/15 bones by name and the Mixamo preset; mirrored and turned 180°).
- `renders/`: Blender turntable (`shadow_front`, `front34`, `side`, `back34`, `back`) and pose (`stance`, `stance_side`, `guard`, `kick`) renders.

## How the moves are built
`build.sh` imports `source/shadow.glb` onto Katana: the file's `Standing` replaces Katana's, its other animations become new sequences, `--import-ghost` poses LOD1 in each `Ghost*` sequence as a dark see-through afterimage under Katana's projectile actor, and `--har-script script/shadow.bgs` adds the moves, which `--compile` builds into VMC2, and `--har-help help.txt` adds his Help topics. Nothing in the engine is specific to Shadow.

## Rebuild
```sh
./build.sh                                # imports source/shadow.glb
./build.sh --model                        # prepares source/ and renders/ from the FBX in Blender first (set BLENDER if it isn't in /Applications)
TEXTURE=tripo.png ./build.sh --model      # the same, with the model's own texture instead of the generated one
```
`editor-har-import-tests <Data> --check source/shadow.glb Katana` prints the retargeting measures (for this model: limbs on Katana's, joints at most 0.57 units from where their parent bones carry them, 2.62 before FINDINGS 760; 198 of 6,309 foot-down samples out of reach, 6 of 1,846 planted samples sliding).

## Not done yet
- The Tripo texture is missing from the FBX (its material has a normal map node but no image), so the texture is generated. If it turns up, pass it with `TEXTURE=`.
- The ribs are painted on: the FBX's limbs are smooth tubes.
- His basics are still Katana's (walk, punches, kicks, throws); only the stance and the specials are his. Katana walks with her feet on one line, so his thick thighs pass close in her walk.
- His shoulder joints sit at the edge of the shoulder armour (0.48 m out, where the arm tube starts), so the upper arm is short and the arms swing from the plates; the claws reach the chest in the stance, not the belt as on the 2097 sprite.
- The projections are posed meshes that slide and switch poses, as 2097's held sprites do; they do not animate between poses, and they do not fade out.
- His move pages in Help have text only; the shipped ones show a picture of the move.
- 2097's aerial specials (the air Shadow Dive, `P632` in the air, and the enhanced air grab) and the Fire Pit destruction are not made.
