# Third-party notices

The [MIT license](https://github.com/kadima-tech/openomf-battlegrounds/blob/main/LICENSE)
covers this project's original code and documentation.
Third-party components retain their own copyrights and license terms. Preserve
their notices when redistributing source or binaries.

## Components in the macOS, Windows and Linux apps

The paths below are relative to `Battlegrounds.app/Contents/Resources/` (macOS),
`Battlegrounds\resources\` (Windows) or `Battlegrounds/resources/` (Linux). The full
upstream texts, rather than this index, define each component's terms.

| Component | Upstream / version | License and bundled notice |
| --- | --- | --- |
| openomf-battlegrounds | This repository | MIT; `licenses/openomf-battlegrounds.txt` |
| SDL2 | [libsdl-org/SDL](https://github.com/libsdl-org/SDL), commit `5d249570393f7a37e037abf22cd6012a4cc56a71` | zlib; `licenses/SDL2.txt` |
| unshield | [twogood/unshield](https://github.com/twogood/unshield), 1.5.1 | MIT; `licenses/unshield.txt` |
| GothicKit dmusic | [GothicKit/dmusic](https://github.com/GothicKit/dmusic), vendored revision `b73b270b` with local changes | Upstream MIT modern variant; `licenses/dmusic.txt`; provenance and modifications in `licenses/dmusic.md` |
| DirectMusic software synthesizer sample (DDKSynth) | Microsoft Windows Driver Kit 8.1 sample, ported to C in dmusic | MIT, Microsoft Corporation; `licenses/DDKSynth.txt` |
| zlib (Windows only) | [madler/zlib](https://github.com/madler/zlib), 1.3.1 | zlib; `licenses/zlib.txt` (macOS uses the system zlib) |
| openindeo | [kadima-tech/openindeo](https://github.com/kadima-tech/openindeo), commit `5452df0c8900f0892e701253481bf5c311a9a9d7` | MIT; `licenses/openindeo.txt` |
| ENet | [lsalzman/enet](https://github.com/lsalzman/enet), 1.3.18 (commit `2662c0de09e36f2a2030ccc2c528a3e4c9e8138a`) | MIT; `licenses/enet.txt` |
| Monocypher | [LoupVaillant/Monocypher](https://github.com/LoupVaillant/Monocypher), 4.0.2 (commit `0d85f98c9d9b0227e42cf795cb527dff372b40a4`), with its optional Ed25519 | BSD-2-Clause or CC0-1.0, at the user's choice; `licenses/Monocypher.txt` |
| CPython runtime and included libraries | Python 3.12.12 from [python-build-standalone](https://github.com/astral-sh/python-build-standalone) (macOS: installed by [uv](https://github.com/astral-sh/uv); Windows: release `20251028`, including its Visual C++ runtime DLLs) | Python and included third-party license terms; `licenses/Python.txt` and notices retained under `python/` |
| bsdiff4 | [ilanschnell/bsdiff4](https://github.com/ilanschnell/bsdiff4), 1.2.6 | BSD-style terms; full license retained in `python-packages/bsdiff4-1.2.6.dist-info/` |
| Tcl/Tk (Linux runtime only) | Included in the Linux CPython runtime ([python-build-standalone](https://github.com/astral-sh/python-build-standalone)), which also links X11 client libraries into Tk; the launcher does not use Tk | Tcl/Tk BSD-style license; `licenses/Tcl-Tk.txt`; X.Org libraries under MIT/X11-style terms |

The vendored dmusic source notices are also available in
[`engine/third_party/dmusic/`](https://github.com/kadima-tech/openomf-battlegrounds/tree/main/engine/third_party/dmusic).
Dependency versions
and integrity pins live in `launcher/cmake/CMakeLists.txt`, `engine/CMakeLists.txt` (ENet, Monocypher), `launcher/bundle-setup.sh`,
`launcher/windows/bundle-python.sh` and `launcher/requirements.txt`. uv is a build-time
tool and is not distributed in the finished app. macOS system frameworks and libraries
are supplied by macOS; on Windows, the system DLLs the programs use (including the
Universal C Runtime) are part of Windows 10 and 11. The Windows programs are built with
MinGW-w64 GCC, whose runtime libraries are linked in under their GCC Runtime Library
Exception and MinGW-w64 permissive terms. On Linux, glibc, zlib, OpenGL, GTK 3 and its
libraries, and the X11/Wayland and audio client libraries SDL2 opens at runtime are
supplied by the player's distribution and are not bundled. The SDL2 and C++ runtime code
linked into the Linux engine retains the notices above and GCC's runtime library exception.

## Components in the content editor (openbg-editor)

The editor is a development tool built from source and not part of the app downloads.

| Component | Upstream / version | License |
| --- | --- | --- |
| Dear ImGui (docking branch) | [ocornut/imgui](https://github.com/ocornut/imgui), `v1.92.5-docking`, fetched by CMake | MIT |
| cgltf | [jkuhlmann/cgltf](https://github.com/jkuhlmann/cgltf), `v1.15`, fetched by CMake (glTF reading) | MIT |
| stb_image | [nothings/stb](https://github.com/nothings/stb), commit `f58f558c120e9b32c217290b80bad1a0729fbb2c`, fetched by CMake (JPEG textures) | MIT or public domain (Unlicense), at the user's choice |
| Inter | [rsms/inter](https://github.com/rsms/inter), 4.1 (Regular, SemiBold); `engine/third_party/fonts/Inter-LICENSE.txt` | SIL Open Font License 1.1 |
| JetBrains Mono | [JetBrains/JetBrainsMono](https://github.com/JetBrains/JetBrainsMono), 2.304 (Regular); `engine/third_party/fonts/JetBrainsMono-LICENSE.txt` | SIL Open Font License 1.1 |
| Lucide icons | [lucide-icons/lucide](https://github.com/lucide-icons/lucide), lucide-static 0.469.0 (`lucide.ttf`); `engine/third_party/fonts/lucide-LICENSE.txt` | ISC (portions MIT, from Feather) |

## The lobby server (openbg-lobby)

The lobby server in `server/` is built separately (a static Go binary or a container image) and
is not part of the app downloads. It uses only the Go standard library and runtime (BSD-3-Clause,
the Go Authors), which a distributed binary carries.

## Original game and update data

*One Must Fall: Battlegrounds* was developed by Diversions Entertainment. The
original game's code, data, artwork, audio, names and trademarks belong to their
respective rights holders. This project is not affiliated with or endorsed by
them. Its MIT license grants no rights to those materials.

Players must supply their own supported English game disc or ISO. The bundled
`patches/english-2090-2123.obgpatch` contains cumulative update differences for
the original game; it is not licensed under this project's MIT license. Its
provenance and reproduction steps are documented in
[launcher/README.md](https://github.com/kadima-tech/openomf-battlegrounds/blob/main/launcher/README.md#cumulative-update-provenance)
and [PATCHES.md](https://github.com/kadima-tech/openomf-battlegrounds/blob/main/PATCHES.md).
A complete game installation and disc-derived DirectX tables are not bundled.
