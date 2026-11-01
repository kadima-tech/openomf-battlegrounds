Download the ZIP for your Mac: **arm64** for Apple silicon or **x86_64** for Intel.
Requires **macOS 13 or newer**. Unzip and open `Battlegrounds.app`; no compiler,
Homebrew or system Python installation is required.

You need your own English retail CD/ISO of *One Must Fall: Battlegrounds*.
Choose it in the launcher on first run. Setup works offline and needs at least
3 GB of free space. The cumulative update and setup tools are bundled; a complete
game installation is not included.

These builds are ad-hoc signed and are **not Apple notarized**. macOS Gatekeeper
may block opening a downloaded app. If you trust the download, follow
[Apple's instructions](https://support.apple.com/en-us/102445) to allow it in
System Settings > Privacy & Security.

Each ZIP has a matching `.sha256` file. Verify it from Terminal with
`shasum -a 256 -c Battlegrounds-*.zip.sha256` in your download folder.
