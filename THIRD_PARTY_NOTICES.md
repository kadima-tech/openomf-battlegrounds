# Third-party notices

The [MIT license](https://github.com/kadima-tech/openomf-battlegrounds/blob/main/LICENSE)
covers this project's original code and documentation.
Third-party components retain their own copyrights and license terms. Preserve
their notices when redistributing source or binaries.

## Components in the macOS app

The paths below are relative to `Battlegrounds.app/Contents/Resources/`.
The full upstream texts, rather than this index, define each component's terms.

| Component | Upstream / version | License and bundled notice |
| --- | --- | --- |
| openomf-battlegrounds | This repository | MIT; `licenses/openomf-battlegrounds.txt` |
| SDL2 | [libsdl-org/SDL](https://github.com/libsdl-org/SDL), commit `5d249570393f7a37e037abf22cd6012a4cc56a71` | zlib; `licenses/SDL2.txt` |
| unshield | [twogood/unshield](https://github.com/twogood/unshield), 1.5.1 | MIT; `licenses/unshield.txt` |
| GothicKit dmusic | [GothicKit/dmusic](https://github.com/GothicKit/dmusic), vendored revision `b73b270b` with local changes | Upstream MIT modern variant; `licenses/dmusic.txt`; provenance and modifications in `licenses/dmusic.md` |
| DirectMusic software synthesizer sample (DDKSynth) | Microsoft Windows Driver Kit 8.1 sample, ported to C in dmusic | MIT, Microsoft Corporation; `licenses/DDKSynth.txt` |
| openindeo | [kadima-tech/openindeo](https://github.com/kadima-tech/openindeo), commit `5452df0c8900f0892e701253481bf5c311a9a9d7` | MIT; `licenses/openindeo.txt` |
| CPython runtime and included libraries | Python 3.12.12, managed by [uv](https://github.com/astral-sh/uv) | Python and included third-party license terms; `licenses/Python.txt` and notices retained under `python/` |
| bsdiff4 | [ilanschnell/bsdiff4](https://github.com/ilanschnell/bsdiff4), 1.2.6 | BSD-style terms; full license retained in `python-packages/bsdiff4-1.2.6.dist-info/` |

The vendored dmusic source notices are also available in
[`engine/third_party/dmusic/`](https://github.com/kadima-tech/openomf-battlegrounds/tree/main/engine/third_party/dmusic).
Dependency versions
and integrity pins live in `launcher/cmake/CMakeLists.txt`, `launcher/bundle-setup.sh`
and `launcher/requirements.txt`. uv is a build-time tool and is not distributed
in the finished app. macOS system frameworks and libraries are supplied by macOS.

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
