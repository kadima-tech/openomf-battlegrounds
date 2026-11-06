# deobjects-edit

A command-line editor for *One Must Fall: Battlegrounds* `.DEObjects` files. It unpacks a file
into a folder of readable JSON, which you edit with any text editor, and packs the folder back
into a `.DEObjects` file.

- **Lossless.** Unpacking and packing an unchanged folder gives back the original file byte for
  byte. This holds for every shipped file: the 65 files of 2.123, of the 2.090 disc and of each
  patch in between, and the two `core` files. Every unpack checks it and says so. Anything the
  editor does not understand is kept as hex and written back untouched, and objects you do not
  edit stay byte-identical when you edit others.
- **Decodes what is known.** Fields of documented classes (descriptions, values, menu layout,
  images, cameras) get names; references between objects show the target's index and name. The
  format is described in [docs/DEOBJECTS_FORMAT.md](../../docs/DEOBJECTS_FORMAT.md).
- **Standalone.** C++20 and CMake, no other libraries. It builds on Windows (Visual Studio or
  MinGW), macOS and Linux.

## Building

You need CMake 3.20 or newer and a C++20 compiler.

**macOS** (Xcode command-line tools: `xcode-select --install`; CMake from cmake.org or Homebrew):

```bash
cmake -S tools/deobjects-editor -B tools/deobjects-editor/build
```

```bash
cmake --build tools/deobjects-editor/build
```

The program is `tools/deobjects-editor/build/deobjects-edit`.

**Windows** with Visual Studio 2022 (with its "Desktop development with C++" workload, which
includes CMake), from a *Developer PowerShell for VS 2022*:

```powershell
cmake -S tools/deobjects-editor -B tools/deobjects-editor/build
cmake --build tools/deobjects-editor/build --config Release
```

The program is `tools\deobjects-editor\build\Release\deobjects-edit.exe`. With MSYS2/MinGW (the
toolchain the engine uses, see the main [README](../../README.md#building-on-windows)), add
`-G Ninja` to the first command; the program is then `build\deobjects-edit.exe`, statically linked.

Run the tests with `ctest --test-dir tools/deobjects-editor/build -C Release`. To round-trip real
game files as well, configure with `-DDEOBJECTS_SAMPLES=<folder with .DEObjects files>`. The
engine's build has two more tests on the game's `Data` folder (`engine/tests/deobjects_tests.cpp`):
`deobjects-round-trip` packs every file back, and `deobjects-edit-reload` edits a pilot's stat, a
description's name and a compressed object, then checks the result with the engine's reader.

## Using it

```bash
deobjects-edit list   ArenaDescriptions.DEObjects            # the objects in the file
deobjects-edit tree   ArenaDescriptions.DEObjects            # the parent/child hierarchy
deobjects-edit show   ArenaDescriptions.DEObjects "The Desert"   # one object as JSON (or #4)
deobjects-edit unpack ArenaDescriptions.DEObjects arenas     # write the folder "arenas"
deobjects-edit pack   arenas ArenaDescriptions.DEObjects     # build the file from it
deobjects-edit verify Data/*.DEObjects                       # check files round-trip exactly
deobjects-edit schemas                                       # the class layouts it decodes
```

Add `--data <the game's Data folder>` to `show` or `unpack` to name the objects in *other* files
that references point at.

**Work on copies.** The launcher checks every installed game file against the official 2.123
checksums and treats a changed file as needing repair. Try edits on a copy of the `Data` folder,
run by the engine directly (`engine/build/openbg <Data folder>`) or by the original game.

### The unpacked folder

```
arenas/
  deobjects.json             the file header: the agent, the GUID pool, the object order
  objects/0004 The Desert.json   one file per object
  blobs/0003.ex1.bin         large binary payloads (image pixels, script bytecode, ...)
```

An object file looks like this (shortened):

```json
{
  "index": 4,
  "name": "The Desert",
  "class": "Description\\Environment",
  "guid": "f37b5220-f68d-11d3-a3dc-004033de174c",
  "info": { "version": 4, "flag": 0, "value84": 17, "flags": 0 },
  "contents": [
    {
      "class": "Description",
      "version": 3,
      "fields": {
        "name": "The Desert",
        "listed": true,
        "locked_by_default": false,
        ...
      },
      "rest": "000000000000000000000000000000000000000000000000000000000000"
    },
    {
      "class": "EnvironmentDescription",
      "version": 1,
      "fields": { "min_recommended_players": 3, "max_recommended_players": 10 },
      "rest": "0000000000000000000000000000000000000000"
    }
  ],
  "references": [
    { "version": 2 },
    {
      "class": "Description",
      "version": 6,
      "fields": {
        "casts": ["^0 96d4773c-91d0-4043-8259-05c8a00b0280", ...],
        "icon": null,
        "medium_representation": "#5 DesertSelectioImage",
        ...
      }
    }
  ],
  "hierarchy": [ { "class": "Object", "version": 1, "fields": { "children": ["#5 DesertSelectioImage"] } } ],
  "ex": [ { "key": "OBJSavedTimeStamp", "flag": 0, "text": "1004199821" } ]
}
```

- `contents`, `references` and `hierarchy` are lists of **chunks**, one per class level. A chunk
  is either decoded (`fields`, plus any undecoded `rest` as hex), a list of reference lists
  (`lists`), or raw `hex`. `lists` is a guess for chunks without a schema, shown only when the
  lists write back to exactly the same bytes; some chunks (a texture manager's, a faction's) are
  not lists and stay hex. A block that is not a clean run of chunks is a single `{"hex": ...}`.
- **References** are strings: `"#5 DesertSelectioImage"` is object 5 of this file (the number
  counts; the name is a reminder and you get a warning if it no longer matches), and
  `"^0 96d4773c-..."` is entry 0 of the GUID pool in `deobjects.json`, an object in another
  file. `null` is no reference. The game writes an index of this file up to 204 as one byte and
  from 205 on as three; a reference stored in a wider form than that keeps it with a suffix after
  the index, `"#0/u16 ..."` (or `/tag`; `^n/u8`, `/u16`, `/u32` for pool entries). You can add a
  suffix yourself to force a form; without one, the game's own choice is written.
- **Text** is a JSON string. The game stores 8-bit Latin-1 text, so characters beyond that (such
  as `€` or emoji) are refused. Text that is not plain (embedded NULs, no terminator) is shown as
  `{"hex": ...}`.
- **Floats** are numbers. NaN and infinity are written as their bit pattern, e.g. `"0x7fc00000"`.
- **Extended data** payloads are inline `text` or `hex` when small, otherwise a `file` in `blobs/`.
  Replace a blob file to replace the payload.

### What you can change

- Any decoded field: names, flags, numbers, text, references. Fields are written in the class's
  order whatever their order in the JSON. A field may be removed only together with every field
  after it (that is how an older chunk version looks); unknown field names are refused.
- Hex (`rest`, `hex`, blobs): freely, as long as the game can still read the result.
- Objects: copy an object file under `objects/`, give it a new `guid`, and add its path to the
  `objects` list in `deobjects.json` (its `index` field is informational). New objects go at the
  **end** of the list: references are by index, so inserting in the middle shifts every `#n`
  after it. To place it in the tree, add it to its parent's `children`.
- The GUID pool: add entries at the end, then point `^n` references at them.

Packing re-computes every offset, so blocks may grow or shrink. A compressed object keeps its
original compressed bytes (`packed_file` in `blobs/`) while its contents are unchanged. Edited
contents are stored uncompressed (method 0) with the object still flagged compressed, which both
the engine and the original game read (`DE_CContents::Decompress` case 0; FINDINGS 421).

## How it is built

| File | Role |
|---|---|
| `engine/src/de/objects_model.*` | Reads and writes the container, keeping every byte; shared with the engine's `write_objects_file` |
| `src/schema.*` | The table of known class layouts. **Add classes here.** |
| `src/convert.*` | The model to and from JSON: field decoding, references, payloads |
| `src/json.*` | A small ordered JSON reader/writer |
| `src/main.cpp` | The command line |
| `tests/tests.cpp` | Round-trip, decoding, editing and error tests on a synthetic file |

The container model and decompression are the engine's `engine/src/de/{objects_model,contents,lzrw3a,md5}.cpp`.

A schema entry names the fields of one chunk of one class. A wrong guess cannot damage a file: a
chunk is shown decoded only when its decoded fields encode back to exactly the original bytes;
otherwise it stays hex.
