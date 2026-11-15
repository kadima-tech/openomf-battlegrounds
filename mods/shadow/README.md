# Shadow

Shadow, the One Must Fall: 2097 robot, made from the user's model `blender/shadow-model.fbx` and imported with the editor's robot import onto **Katana's moveset**. He has no special moves of his own yet.

## Files
- `blender/shadow-model.fbx`: the source model. It is a single AI-generated mesh (Tripo): 5,406 triangles, one UV layer, no texture, no rig, no animation; 1 m tall, in a T-pose, facing -Y.
- `blender/prepare_shadow.py`: turns the FBX into the importable model, in Blender (headless):
  - scale: 3.1 m tall (Katana stands about 3 m), feet on the floor, facing kept;
  - mesh: all 5,406 triangles of the FBX (Katana's LOD0 has 2,528; the import generates LOD1 and LOD2 at Katana's 1,262 and 748). `--tris N` decimates with symmetry instead;
  - armature: Mixamo bone names in the T-pose, at joints measured from the mesh (the crotch, knee pads, ankles, the waists at elbow and wrist, the neck);
  - weights: Blender's automatic weights. They fail on this mesh (holes and overlapping shells), so the script weights a watertight voxel remesh instead and transfers the weights back. The loose studs then follow the bone nearest them rigidly, and no limb takes weight from the other side;
  - texture: painted per texel in the model's own UVs: silver armour, black-and-white ribbed limbs and waist (as on the 2097 sprite), a dark face with a glowing visor, cyan studs, ear pods, claws and blade feet. `--texture file.png` uses a texture laid out in the same UVs instead;
  - materials: six, all sharing the one texture, each with a `de_paint` custom property and metallic for the reflection. Armour takes the pilot's first colour, limbs the second, hands the third; face, studs and blades keep their own colours;
  - the "Stealth" paint job (`ShadowDark.png`), the 512 × 128 Choose Creature picture (four staged poses) and turntable and pose renders.
- `blender/af_sprites.py`: extracts the sprite frames from 2097's `FIGHTR*.AF` files, which were used as references.
- `source/`: what the import reads:
  - `shadow.glb` and `shadow.blend`;
  - `ShadowColours.png` (the texture) and `ShadowDark.png` (the Stealth paint job);
  - `choose_creature.png`;
  - `mapping.json`: what the import chose (15/15 bones by name and the Mixamo preset; mirrored and turned 180°).
- `renders/`: Blender turntable (`shadow_front`, `front34`, `side`, `back34`, `back`) and pose (`stance`, `stance_side`, `guard`, `kick`) renders.

## Rebuild
```sh
./build.sh                                # imports source/shadow.glb
./build.sh --model                        # prepares source/ and renders/ from the FBX in Blender first (set BLENDER if it isn't in /Applications)
TEXTURE=tripo.png ./build.sh --model      # the same, with the model's own texture instead of the generated one
```
`editor-har-import-tests <Data> --check source/shadow.glb Katana` prints the retargeting measures (for this model: limbs on Katana's, worst edge stretch 1.21 of Katana's own, 4 of 1,865 planted foot samples sliding).

## Not done yet
- The Tripo texture is missing from the FBX (its material has a normal map node but no image), so the texture is generated. If it turns up, pass it with `TEXTURE=`.
- The cyan studs don't glow, because imported robots have no glow material yet.
- The ribs are painted on: the FBX's limbs are smooth tubes.
- No special moves: he fights with Katana's moves.
