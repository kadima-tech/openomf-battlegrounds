**macOS:** download the ZIP for your Mac: **arm64** for Apple silicon or **x86_64**
for Intel. Requires **macOS 13 or newer**. Unzip and open `Battlegrounds.app`; no
compiler, Homebrew or system Python installation is required.

**Windows:** download the **windows-x64** ZIP. Requires **Windows 10 or 11** (64-bit;
Windows 11 on Arm runs it through emulation) and a graphics driver with OpenGL 4.1.
Extract all files and run `Battlegrounds\Battlegrounds.exe`; no installer,
administrator rights, Visual C++ runtime or Python installation is required.

**Linux:** download the `.tar.gz` for **x86_64** or **aarch64**. Requires a
desktop with GTK 3 and OpenGL 4.1 drivers on glibc 2.35 or newer (Ubuntu 22.04,
Debian 12, Fedora 36 or later). Unpack it anywhere and run
`Battlegrounds/Battlegrounds`; `--create-desktop-entry` adds it to your
application menu. No packages or system Python are installed.

You need your own English retail CD/ISO of *One Must Fall: Battlegrounds*.
Choose it in the launcher on first run. Setup works offline and needs at least
3 GB of free space. The cumulative update and setup tools are bundled; a complete
game installation is not included.

The macOS builds are ad-hoc signed and are **not Apple notarized**. macOS Gatekeeper
may block opening a downloaded app. If you trust the download, follow
[Apple's instructions](https://support.apple.com/en-us/102445) to allow it in
System Settings > Privacy & Security. The Windows build is **not Authenticode
signed**, so Microsoft Defender SmartScreen may warn about it; if you trust the
download, choose **More info** and then **Run anyway**.

Each download has a matching `.sha256` file. Verify it from a terminal in your
download folder with `shasum -a 256 -c Battlegrounds-*.zip.sha256` (macOS) or
`sha256sum -c Battlegrounds-*.tar.gz.sha256` (Linux). On Windows, compare the
file's hash with `Get-FileHash Battlegrounds-*-windows-x64.zip` in PowerShell.
