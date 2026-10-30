# openomf-battlegrounds

![A fight in the City arena](docs/screenshot.png)

**openomf-battlegrounds** is an open-source reimplementation of the engine behind
*One Must Fall: Battlegrounds* (Diversions Entertainment, 2003), the 3D sequel to *One Must Fall: 2097*.
In the spirit of [OpenOMF](https://github.com/omf2097/openomf) and ScummVM, it runs the original
game's own data (the robots, pilots, arenas, menus, scripts, sound and music) on a new, native
engine, so the game can be played on modern machines.

> **Status: alpha.** Single-player tournaments, Practice and Training run end to end: the original
> menus, robot and pilot selection, every robot and arena, the computer opponents, the HUD, sound,
> music and replays. Expect bugs and rough edges. Online and LAN play are not supported yet.
> [PLACEHOLDERS.md](PLACEHOLDERS.md) lists everything that is still missing or approximated.

The port follows the game's final official patch, **2.123**.

## You need the game

No game data is included in this repository. You need your own copy of *One Must Fall:
Battlegrounds*, either the Windows CD or an ISO image of it, and the official patches up to 2.123.

## Building

openomf-battlegrounds currently builds on **macOS** (Apple silicon or Intel). You need CMake 3.20 or
newer, a C++20 compiler (Xcode's command-line tools), and a few packages from Homebrew:

```sh
brew install cmake sdl2 unshield
```

Then build:

```sh
git clone https://github.com/kadima-tech/openomf-battlegrounds.git
cd openomf-battlegrounds
cmake -S engine -B engine/build
cmake --build engine/build -j
```

The Indeo video decoder, [openindeo](https://github.com/kadima-tech/openindeo), is fetched by CMake
automatically. Run `ctest --test-dir engine/build` to check that your game data parses.

## Installing the game data

**1. Unpack the disc.** Copy your ISO into `source/` and extract its installer:

```sh
mkdir -p source/iso
cp /path/to/Battlegrounds.iso source/
bsdtar -xf source/Battlegrounds.iso -C source/iso Setup
unshield -d source/extracted x source/iso/Setup/data1.cab
```

This gives you the disc version of the game (2.090) in `source/extracted/App_Executables`.

**2. Apply the official patches.** Put the official patch files in `source/patches/` under their
original names (see [PATCHES.md](PATCHES.md) for the list), then run:

```sh
pip3 install unicorn pefile
re/scripts/rtpatch_chain.sh /tmp/omfbg-patching
```

This applies each patch in turn to a copy of the disc files, inside an emulator (no patch program
runs natively), and writes the result to `source/patched/v2123`. You can also patch an installed
copy of the game on Windows and copy its `Data` and `Engine` folders into `source/patched/v2123`.

The `source/` folder is ignored by git, so your game files never end up in a commit.

## Playing

```sh
engine/build/openbg source/patched/v2123/Data
```

This starts the game at its main menu. Choose **Single Player** and pick a tournament, Practice or
Training.

**Fight controls:**

| Key | Action |
|---|---|
| Arrow keys | Walk (up/down) and turn (left/right) |
| A / S | Left / right punch |
| Z / X | Left / right kick |
| Space | Jump (add a direction to jump that way) |
| Left Shift or C | Duck and evade (add left/right to sidestep) |
| Esc | In-game menu |
| V | Replay controls (during a replay) |

To jump straight into a single fight against the computer:

```sh
engine/build/openbg-fight source/patched/v2123/Data Jaguar --opponent Katana --arena Desert
```

Robots: Jaguar, Katana, Gargoyle, Chronos, Mantis, Pyros, Warlord, Force. Arenas: Desert, BladePit,
Canyon, City, FirePit, Iceberg, PowerPlant, SpaceStation, Volcano.

## How it works

The original game runs on Diversions Entertainment's "DE" engine: a set of module DLLs plus a large
amount of game logic written in the engine's own scripting language and shipped as compiled
bytecode inside the data files. openomf-battlegrounds reimplements the engine (the data formats,
the script virtual machine and its native functions, scenes, physics, AI, rendering, sound and
music) and runs the game's own scripts on top of it. The behaviour is reverse engineered from the
game's modules; [re/FINDINGS.md](re/FINDINGS.md) records what each part does and where it was
read from. The copy-protected game executable is never decrypted.

## Contributing

Contributions and bug reports are welcome. [DEVELOPING.md](DEVELOPING.md) describes the code, the
tools and the debugging switches; [PLACEHOLDERS.md](PLACEHOLDERS.md) is the list of open work;
[PATCHES.md](PATCHES.md) documents what the official patches changed.

## Credits

*One Must Fall: Battlegrounds* was developed by Diversions Entertainment and belongs to its rights
holders. This project is not affiliated with or endorsed by them, and contains none of the game's
data.

Third-party code included in this repository keeps its own licence: GothicKit's
[dmusic](https://github.com/GothicKit/dmusic) (MIT, vendored in `engine/third_party/dmusic`) and a C
port of Microsoft's DirectMusic software synthesizer sample (MIT). See
[engine/third_party/dmusic/VENDORED.md](engine/third_party/dmusic/VENDORED.md).
