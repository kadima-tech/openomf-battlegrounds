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
2. The launcher extracts your disc and applies one bundled cumulative update
   directly from 2.090 to 2.123. The 17.5 MiB update, private Python runtime and
   hash-locked binary-delta decoder ship inside the app. Setup works completely
   offline: no mirrors, patch executables, CPU emulator or package downloads.
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

Closing during installation stops the whole setup process group. A retry starts
with fresh staging files; incomplete staging files never count as ready.
Both the native worker and Python installer lock setup to prevent overlapping
installs. Publishing uses a same-volume rename with rollback; the original ISO,
CD and any existing legacy installation remain untouched.

## Files and isolation

In a source checkout, everything setup installs lives in `source/launcher/`:

```text
game/          Verified Data and Engine folders
install.log    Extraction and patch diagnostics
bootstrap.log  Bundled setup tool diagnostics
game.log       Native engine output
```

For a standalone app in a writable folder, files go into the adjacent
`Battlegrounds Files/` folder. In `/Applications` or a read-only location they go
into the user's `~/Library/Application Support/OpenBG/` folder. Settings shows
the exact location. No shell profiles, system Python environments, Homebrew
packages or administrator privileges are involved. The engine continues to keep
its preferences and recordings in its existing Application Support location.

`OPENBG_HOME=/absolute/path` overrides the install location;
`OPENBG_PROJECT_ROOT=/absolute/path` enables legacy-data discovery.
The app's `--preview` switch shows setup without installing or launching. For
visual verification, `--screenshot /absolute/path.png` renders the actual native
setup view and exits. `OPENBG_GAME_ARGS` supplies space-separated engine arguments
for development smoke tests (for example `--headless 10`).

First-time setup works offline and needs at least 3 GB free space.
The supported source is the English 2.090 retail disc; other localizations and
later disc editions fail validation rather than mixing incompatible game data.

## Package for players

```sh
launcher/build-app.sh
```

Distribute `dist/Battlegrounds.app` as a zip. The build script includes the
engine, static extractor, cumulative update, relocatable CPython 3.12.12 and
bsdiff4 1.2.6, together with the project's MIT license, third-party notice index
and upstream license files in `Contents/Resources/licenses/`. Additional runtime
and package notices remain inside `python/` and `python-packages/`. See
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) for the component list and the
separate status of original game/update data. Python and its decoder are
private to the app; no Homebrew packages, system Python or player-side downloads
are used. uv 0.9.7 is a pinned build-time tool and is not shipped in the app.
The build machine needs network access to fetch pinned dependencies. A complete
game installation and disc-derived DirectX tables are not included.

Build on each supported architecture for separate Apple silicon/Intel releases.
The script ad-hoc signs the app. Developer ID signing and Apple notarization
are not configured, so macOS Gatekeeper may block a downloaded build. Players
who trust the download can allow it in System Settings > Privacy & Security,
following [Apple's instructions](https://support.apple.com/en-us/102445).

### CI builds and releases

GitHub Actions builds the same standalone app on native Apple silicon and Intel
macOS runners. Each download is a `Battlegrounds-<label>-macos-<arch>.zip` with
a matching `.zip.sha256` checksum. Both architectures must build and pass the
installer/native readiness tests, offline relocation checks, signature and architecture checks before a
release is published. The engine's data-dependent CTest suite requires the
original disc files and is not run in CI.

- **Dev:** pushes to `main` and manual runs of **Dev build and prerelease**
  publish a new prerelease with a unique `dev-<run-id>-<attempt>` tag pointing
  to the exact built commit. These never replace the latest formal release.
  Pull requests targeting `main` run the same builds and tests and upload
  Actions artifacts, without publishing a release.
- **Release:** push a stable `vMAJOR.MINOR.PATCH` tag, such as `v0.1.0`, to run
  **Tagged release**. It builds the tagged code and creates a formal GitHub
  release with both ZIPs, checksums and generated release notes. Other `v*`
  tags fail validation. An existing published release is never overwritten.

For example, after committing the version you want to release:

```sh
git tag v0.1.0
git push origin v0.1.0
```

The workflows use the built-in `GITHUB_TOKEN`; no extra publishing secrets are
needed. Keep GitHub Actions enabled for the repository and allow the publishing
jobs' `contents: write` permission. Actions artifacts expire after 14 days;
release assets remain available on the repository's Releases page.

The shared `build-macos.yml` workflow calls `launcher/build-app.sh`. It stamps
`CFBundleShortVersionString` from the release tag (`0.0.0` for dev builds) and
`CFBundleVersion` from the workflow run number and attempt before signing.
Local builds retain `0.1.0` / `1` by default; override them with
`OPENBG_VERSION` and `OPENBG_BUILD_NUMBER` when needed.

## Validation

```sh
python3 -m pip install --require-hashes --only-binary=:all: -r launcher/requirements.txt
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
disc identity, corrupt updates, wrong versions, delta output bounds, cancellation,
offline retry, atomic publication and rollback.
Native tests cover cached readiness, same-size edits, changed expected patches,
missing game files and malformed caches.
CI runs `launcher/check-bundle.py` on both architectures after building. It moves
the bundled tools to a path with spaces, starts them with a fresh home and
untrusted Python environment variables, and denies network access with the macOS
sandbox. Real-disc end-to-end testing needs your own game disc.

## Cumulative update provenance

`patches/english-2090-2123.obgpatch` is versioned so CI packages the same update
without downloading game patches. `cumulative-patch.json` pins its size and SHA-256.
The archive contains 64 direct BSDIFF40 deltas and a manifest for all 76 original
Data/Engine files: the other 12 are unchanged. Each source file is checked before
any file is modified, and every final file is checked against `game-files.json`
before the installation is published. The archive contains update differences,
not a complete copy of the game; players still need their own English disc.

Rebuild it from your local disc and byte-exact official final tree:

```sh
python3 re/scripts/build_cumulative_patch.py \
  source/extracted/App_Executables source/patched/v2123 --jobs 2
python3 launcher/patching.py launcher
```

The builder fixes ZIP metadata and JSON formatting for reproducible output, checks
all final file hashes, and round-trips every delta. It requires the pinned
`launcher/requirements.txt` dependency. No disc files or original updater executables
are needed on CI. The historical tools in `re/scripts/rtpatch_chain.sh` and
`rtpatch_emu.py` remain for auditing; original patch URLs/checksums are recorded in
`re/scripts/official-patches.json`. The chain and its changes are documented in
`PATCHES.md`.
