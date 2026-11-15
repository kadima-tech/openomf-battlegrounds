# Major Kreissack

Major Kreissack, president of WAR and the villain of One Must Fall: 2097, as a Battlegrounds pilot. Also "WAR Finale", a one-match tournament against him that unlocks him.

## Spec

His values come from 2097 and are mapped onto Battlegrounds' pilot model.

| Field | Value | Source |
|---|---|---|
| Power / Agility / Endurance / Focus | 70 / 65 / 70 / 45 | 2097: 16 / 15 / 16 on a 1–25 scale (OpenOMF `pilots.c`). His total of 47 against 30 for every other pilot is scaled to 250, matching BG's strongest boss, Mizuno-4. |
| AI | Specials and grapples Massive; supers and pre-emptive attacks Huge; no mercy or fear | OpenOMF `ai_controller.c` personality 10. These are OpenOMF's own tuning, not original game data. |
| Colours | purple / navy / white | Approximate |
| Locked | Locked, visible when locked; unlocked by winning WAR Finale | |
| Bio and quotes | Quotes from 2097's text strings 747 and 980–990 | 2097's own bio is a placeholder, so his BG bio is new text based on his 2097 ending |
| Robot | Warlord, in WAR Finale | His 2097 robot, Nova, isn't in Battlegrounds |

All fields are in `scripts/pilot_args.txt` (`--pilot-set`) and `scripts/tournament_args.txt` (`--tournament-set`).

## Files
- `source/kreissack_source.jpeg`: the portrait as supplied.
- `source/kreissack_cutout.png`: the portrait with its background removed and the beret's crown extended. This is what the build imports; the editor resizes it to the game's 256×256.
- `source/kreissack_256.png`: the 256×256 pilot photo.
- `scripts/cutout.py`: removes the background with rembg. Needs `pip install rembg`; `birefnet-portrait` gave the cleanest edges.
- `scripts/make_photo.py`: extends the beret, then frames and scales the photo.
- `previews/`: the cut-out against different backgrounds, and in the game.

## Rebuild
```sh
python3 scripts/cutout.py source/kreissack_source.jpeg /tmp/cut birefnet-portrait
python3 scripts/make_photo.py /tmp/cut/cut_birefnet-portrait.png source/kreissack_cutout.png source/kreissack_256.png
./build.sh
```
