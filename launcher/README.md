# Battlegrounds launcher

The launcher is a native SwiftUI macOS app (macOS 13+, Apple silicon or Intel).
Its service-bay design uses graphite, amber and an original robot schematic. The
artwork was generated with the built-in imagegen tool; no disc assets are needed
to display setup.

## Run from this checkout

Double-click `launch.command`, or run:

```sh
./launch.command
```

The first source build needs Xcode command-line tools, CMake 3.20+ and Python 3.
It fetches pinned SDL2, unshield and openindeo sources into `launcher/build/`,
compiles them and creates `dist/Battlegrounds.app`. SDL2 and unshield are linked
statically; Homebrew libraries are not used by the finished app. After changing
launcher or engine code, rebuild with `launcher/build-app.sh`.

## First run

1. Choose an ISO image, drag one onto the ISO panel, or insert your original
   English 2.090 game CD. Matching mounted discs are detected at startup and on
   macOS mount notifications, and start setup automatically. Unrelated discs are
   ignored; an already mounted ISO works as a disc too.
2. The launcher downloads a private Python runtime and the hash-locked patching
   packages. It extracts your disc, downloads and verifies the eight official
   patch executables, and applies them using the existing RTPatch CPU emulator.
   Windows executables are never run natively. Locally supplied patches in
   `source/patches/` are reused only when their checksums match.
3. All 76 original game data/module files are checked against the recorded
   English 2.123 SHA-256 manifest. Only then is the completed installation
   published and the native engine started.

On subsequent launches the app checks the actual files and starts the game
without displaying setup or needing Python/network access. Existing complete
`source/patched/v2123` installs are also recognized. A missing, truncated,
corrupted or different-version installation opens setup again. No first-run
boolean can make an incomplete installation count as ready. After full checksum
verification, a local readiness cache records each file's size, inode and macOS
modification/change timestamps. Unchanged verified files need no reread; changed
files are hashed again. The cache also checks the expected digest from the app's
manifest. It is a startup optimization, not protection against deliberate local
tampering or silent storage corruption that leaves all file metadata unchanged.
The explicit `--verify` command below always reads and hashes every file.

Closing during installation stops the whole setup process group. Downloads and
tools can be reused on retry; incomplete staging files never count as ready.
Both the native worker and Python installer lock setup to prevent overlapping
installs. Publishing uses a same-volume rename with rollback; the original ISO,
CD and any existing legacy installation remain untouched.

## Files and isolation

In a source checkout, everything setup installs lives in `source/launcher/`:

```text
game/          Verified Data and Engine folders
patches/       SHA-256 checked official patch downloads
runtime/       Private Python, virtual environment, packages and download cache
install.log    Extraction and patch diagnostics
bootstrap.log  Private runtime installation diagnostics
game.log       Native engine output
```

For a standalone app in a writable folder, files go into the adjacent
`Battlegrounds Files/` folder. In `/Applications` or a read-only location they go
into the user's `~/Library/Application Support/OpenBG/` folder. Settings shows
the exact location. No shell profiles, system Python environments, Homebrew
packages or administrator privileges are involved. The engine continues to keep
its preferences and recordings in its existing Application Support location.

`OPENBG_HOME=/absolute/path` overrides the install location;
`OPENBG_PROJECT_ROOT=/absolute/path` enables legacy-data/local-patch discovery.
The app's `--preview` switch shows setup without installing or launching. For
visual verification, `--screenshot /absolute/path.png` renders the actual native
setup view and exits. `OPENBG_GAME_ARGS` supplies space-separated engine arguments
for development smoke tests (for example `--headless 10`).

First-time setup needs an internet connection and at least 3 GB free space.
The supported source is the English 2.090 retail disc; other localizations and
later disc editions fail validation rather than mixing incompatible game data.

## Package for players

```sh
launcher/build-app.sh
```

Distribute `dist/Battlegrounds.app` as a zip. The build script includes the
engine, static extractor, pinned standalone uv and third-party license notices.
It rejects non-system dynamic library dependencies. Python and patch packages
are downloaded into the player's game folder only when setup is needed.
No game files, patches or DirectX-derived tables are included in the app.

Build on each supported architecture for separate Apple silicon/Intel releases.
The script ad-hoc signs for local use; a public release still needs Developer ID
signing and Apple notarization. This workflow does not publish a release.

## Validation

```sh
python3 -m unittest discover -s launcher -p 'test_*.py' -v
xcrun swiftc -parse-as-library launcher/Readiness.swift launcher/test-readiness.swift \
  -o launcher/build/test-readiness
launcher/build/test-readiness
python3 launcher/install.py --home /path/to/private-state \
  --verify /path/to/game --resources launcher
```

The installer can be exercised without the GUI using the bundled tools:

```sh
dist/Battlegrounds.app/Contents/Resources/bin/worker \
  dist/Battlegrounds.app/Contents/Resources/bootstrap.sh \
  /absolute/path/to/private-state --source /absolute/path/to/Battlegrounds.iso
```

The backend emits JSON progress events. Unit tests cover checksum readiness,
disc identity, corrupt/oversized downloads, atomic publication and rollback.
Native tests cover cached readiness, same-size edits, changed expected patches,
missing game files and malformed caches.
Real-disc end-to-end testing needs your own game data; none is committed.

Patch URLs and checksums are in `patches.json`, from the
[LoneBullet patch mirror](https://www.lonebullet.com/).
`game-files.json` contains filenames, sizes and hashes only, derived from the
verified official patch chain documented in the repository's `PATCHES.md`.
