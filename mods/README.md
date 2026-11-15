# Mods

Add-ons made with the content editor (`openbg-editor`). Each folder holds everything needed to rebuild its mod, plus a `build.sh` that does it with the editor in batch mode.

| Mod | What it adds |
|---|---|
| [kreissack](kreissack/) | Major Kreissack, the villain of One Must Fall: 2097, as a pilot, plus a one-match tournament ("WAR Finale") that unlocks him |
| [shadow](shadow/) | Shadow, the 2097 robot, from the user's model prepared and animated in Blender, imported on Katana's moveset, with his 2097 shadow projections as specials |

**Build and play**
```sh
mods/kreissack/build.sh          # writes mods/kreissack/build/Kreissack.DEObjects
mods/shadow/build.sh             # writes mods/shadow/build/Shadow.DEObjects
openbg <Data> --addon mods/kreissack/build/Kreissack.DEObjects --addon mods/shadow/build/Shadow.DEObjects
```
- To play them from the launcher, open its mod screen (`./configure.command`, or `configure.cmd` on Windows) and tick them: in a checkout it lists every `mods/*/build/*.DEObjects`. They stay where `build.sh` wrote them, so a rebuild is picked up the next launch. `openbg` started directly then loads them too ([launcher/README.md](../launcher/README.md#mods)); an explicit `--addon` of the same file name replaces the listed copy.
- The scripts expect the patched game data in `source/patched/v2123/Data` (set `OPENBG_DATA` to change it) and a built `engine/build/openbg-editor` (`OPENBG_EDITOR`).
- They run the editor with a temporary HOME, so your settings are untouched.

**Not in git**
- `build/`: the add-ons contain copies of the base game's objects, so they are built locally and never committed.
- `references/`: art and text from One Must Fall: 2097, used while making the mods. These stay local.
