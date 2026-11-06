# The .DEObjects file format

*One Must Fall: Battlegrounds* keeps all of its game data (robots, pilots, arenas, menus, scripts,
sounds, music, images) in 65 `.DEObjects` files, written by Diversions Entertainment's object
system ("DE Core", `Core.ModuleDLL`). This document describes the format as far as it is known.
It is the reference for [`tools/deobjects-editor`](../tools/deobjects-editor), and is meant to be
checked against the original engine source and the original editor, then extended.

Where a statement comes from the original binaries, the function and its address in
`Core.ModuleDLL` (2.123) are given. [re/FINDINGS.md](../re/FINDINGS.md) has the full
reverse-engineering notes. The engine's reader is
[engine/src/de/objects_file.cpp](../engine/src/de/objects_file.cpp).

Confidence markers used below:

- **verified**: reading and rewriting every byte of shipped files reproduces them exactly.
- **from code**: read from the original engine's code; consistent with shipped files.
- **observed**: seen in shipped files, meaning not confirmed.
- **unknown**: present in files, meaning not known yet. These are listed under
  [Open questions](#open-questions).

## Primitive types

Everything is little-endian, as on the original x86 build.

| Name | Encoding |
|---|---|
| `u8`, `u16`, `u32`, `i32`, `f32` | 1, 2, 4, 4, 4 bytes; `f32` is an IEEE single |
| `BOOL` | one byte, 0 or 1 (`DE_CContents::ExtractBOOL` 0x70019b00 reads a byte) |
| `block` | `u32` length, then that many bytes (`DE_CDataBlock`) |
| `string` | a `block` whose bytes include the terminating NUL. The empty string is a zero-length block (`DE_CDataBlock::Set(char*)` 0x70029140). Text is 8-bit (Windows code page 1252 / Latin-1) |
| `GUID` | 16 bytes, Windows `GUID` layout (the first three groups little-endian) |
| `squeezed` | a byte `< 0xFE` is the value; `0xFE` is followed by a `u16`; `0xFF` by a `u32`. The writer (`DE_CContents::AddSqueezed` 0x70019100) uses the `u16` form only below `0xFFFF`, so `0xFFFF` itself takes the `u32` form (FINDINGS 422) |
| `chunk` | `u8` version, a size (a `u8`, or `0xFF` then a `u32`), then that many bytes. The size counts the bytes after the size field |

**Chunks** are how the object system versions its data: each class writes its fields inside one
chunk tagged with its own version, and readers always skip to the chunk's end. A newer class can
add fields at the end of its chunk without breaking older readers. Most readers check the version
and read optional trailing fields only while there are bytes left in the chunk. (verified)

## Container layout

```
string   signature        "Diversions Entertainment Objects File2"
u8       format           2 in every shipped file
u32      agent EX count   0 in every shipped file (the agent's own extended-data entries)
block    header           the object table (below)
...      object data      one record per object, in table order
...      EX payloads      extended data, in object order, then entry order
```

`DE_CTransferAgentWinFile::LoadHeader` (0x70051bb0) reads the start of the file and
`DE_CTransferAgentWinFile::SaveToFile` (0x700514b0) writes it. Files that start with the older
signature `"Diversions Entertainment Objects File"` (no `2`) have no format byte. None ship with
the game, and the editor does not read them. (verified on `ArenaDescriptions`; the engine parses all 65 files)

### Header

The header block holds one chunk, version 9 (`DE_CTransferAgent::ProcessHeader` 0x70055a70):

```
chunk v9 {
    object info          the transfer agent itself (class "TransferAgent\Win\File")
    chunk v1 {           the agent's own contents (DE_CTransferAgent::ObjProcessContents 0x700545b0)
        block            agent +0xd0, empty in shipped files
        GUID             all zero in shipped files
        u8               1 = a GUID pool follows
        u16              GUID pool size
        GUID[n]          the GUID pool: objects in other files that this file refers to
        BOOL             agent +0x108, 1 in shipped files
    }
    i32                  object count
    object info[count]   the object table
}
```

Versions 1–8 of the header exist in the reader but are not used by shipped data.

### Object info (one table entry)

`DE_CTransferObjectInfo::ProcessHeader` (0x700557c0) and the WinFile extension (0x7004fdd0):

```
chunk v4 {
    string   name            e.g. "The Desert"
    string   class path      the factory path, e.g. "Description\Environment"
    GUID     object GUID     (v2+) unique across all files; other files refer to the object by it
    BOOL     create eagerly  (v3+) the original calls New() for it as soon as the file loads
    i32      info +0x84      (v3+) unknown; see open questions
    u32      flags           (v4+) bit 1 (value 2): the contents block is compressed
}
size-only chunk {            format 2: a size field but no version byte
    u32      data offset     file offset of the object's data record
}
```

The **class path** selects the C++ class through the object factory (`re/proto/factory_map.py`
lists them). Each `\` adds a level of the class hierarchy: `Description\Environment` is an
environment description (`DE_CEnvironmentDescription`), derived from `DE_CDescription`.

### Object data record

At each object's data offset (`RetrieveObjectContents` 0x7004fbb0, `ReadEXDataHeader` 0x700511f0):

```
block    contents         the object's fields (ObjProcessContents); compressed when flags & 2
block    references       its links to other objects (ObjProcessReferences)
block    hierarchy        its child objects (ObjProcessHierarchy)
u32      EX entry count
{ block key; u32 payload offset }[count]
```

Each **extended data** (EX) payload sits at its offset: a `u8` flag, then a `block`. The engine
fetches payloads only when it asks for them (`EXRequestData` 0x70050130). This is where bulky
data lives: image pixels, compiled script bytecode (`VMC2`), the importer's source paths, save
timestamps. Keys are either NUL-terminated text (`"VMC2"`, `"ImportPath"`, `"OBJImportFilename"`,
`"OBJSavedTimeStamp"`) or, for an image's pixels, two binary bytes `{0, sub-image index}`.
The payload flag is 0, 1 or 2 in shipped files and its meaning is unknown.

Shipped files are written in exactly this order, with no gaps: header, all data records, then all
payloads in object order. Rewriting a shipped file in that order gives identical bytes. (verified)

### Compression

When an object's info flags have bit 1 set, its contents block is a compressed stream
(`DE_CContents::Decompress`, case table at 0x7001a3b0): a `squeezed` method id, then:

| Method | Payload |
|---|---|
| 0 | stored: a `block` with the plain bytes. The reader refuses only a negative length or one longer than the rest of the stream |
| 5 | "DZip". The original engine itself refuses it as obsolete |
| 6 | `squeezed` plain size, then LZRW3-A data |
| 7 | MD5 of the plain bytes (16), `squeezed` plain size, LZRW3-A data |

1–3 (an LZSS tree coder) and 4 are not used by shipped data. Image pixel payloads use the same
stream inside their EX block.

`SaveToFile` sets flag bit 1 for objects whose class asks for compression and always compresses
them with method 7: `DE_CContents::Compress()` (0x7001a8f0) reads the registry value
`CompressionType` but then calls `Compress(7)` regardless. It never clears the flag. An object
whose flag is set and whose contents are stored with method 0 therefore loads in the original
game (`DE_CTransferAgent::New` 0x70055140 runs `Decompress`, whose case 0 is the stored block)
as in the engine. The editor keeps each object's original compressed stream while its contents
are unchanged and writes changed contents with method 0, flag left set (FINDINGS 421). (from code)

## Inside an object

All three blocks are a run of chunks: **one chunk per class level, base class first**. For a
`Description\Environment` the contents are `[Description chunk][EnvironmentDescription chunk]`.
Some classes have a chunk for `DE_CObject` itself and some do not, so the chunk index of a class
level depends on the block.

### References

A reference to another object is written by `DE_CTransferAgent::InsertReference` and read by
`ExtractReference` (0x70057690):

| Tag byte | Meaning |
|---|---|
| `0x00` | null |
| `0x01` + `u16` | object in this file, by table index |
| `0x02` + `u8`, `0x03` + `u16`, `0x04` + `u32` | object in another file, by GUID pool index |
| `0x32`–`0xFF` | object in this file, table index = tag − 0x32 (indexes 0–205) |

The writer (`DE_CTransferAgent::AddReference` 0x700574b0) picks the form from the index: a local
index up to 204 (0xCC) as the single tag byte, 205 and above as `0x01` + `u16`, so tag `0xFF` is
never written; a GUID pool index in the smallest of `0x02`/`0x03`/`0x04`. Every reference the
editor decodes in the shipped files follows this rule (FINDINGS 420). A reader that re-encodes
references must use exactly this rule (index 205 is the trap); the editor also keeps any other
form it reads (`"#0/u16 ..."`, see its README) so such a file still round-trips. (verified)

Inside references chunks, references nearly always come as **lists**: each entry is
`BOOL 1, reference`, and the list ends with `BOOL 0`. A single optional reference is `BOOL 0`, or
`BOOL 1, reference`.

Not every references chunk is made of such lists, although many decode as lists by accident:
`DE_CTextureManager::ObjGenerateReferences` (0x70131de0) writes a v2 chunk with an `i32` count
followed by that many bare references, and `DE_CFaction::ObjGenerateReferences` (OMF 0x35051e10)
a v1 chunk with an optional reference (the photo), the colour's `CAdvancedReference` references (a
v1 chunk: `BOOL`, reference) and 0x14 filler bytes. The editor shows a chunk without a schema as
lists only when the lists re-encode to exactly the same bytes with AddReference's tags. (from code)

A **passive reference** (`DE_CTransferAgent::CPassiveReference`), used inside contents, names an
object by GUID without loading it: `BOOL set`, then (when set) a size-only chunk of 32 bytes:
the target file's agent GUID and the object's GUID.

### Hierarchy

Every object's hierarchy block is a v1 chunk holding one reference list: its children, always in
the same file (`DE_CObject::ObjProcessHierarchy` 0x7003c110). The tree they form is what
`deobjects-edit tree` prints. (verified)

## Known class layouts

These are the layouts the editor decodes (run `deobjects-edit schemas` for the live list). Fields
are listed in file order; "rest" is whatever follows in the chunk and is kept as raw bytes.

### Description (contents chunk 0)

`DE_CDescription::ObjProcessContents` (0x701f14e0). Descriptions are what the menus list:
arenas, robots, pilots, tournaments and game types.

| Field | Type | From | Meaning |
|---|---|---|---|
| name | string | | display name |
| resource | passive reference | | what the description stands for (for an arena: its environment) |
| listed | BOOL | v2 | shown in menus (+0xbc) |
| unknown_d4 | f32 | v3 | +0xd4, unknown |
| locked_by_default | BOOL | v3 | +0xc4 |
| visible_when_locked | BOOL | v3 | +0xc8 |
| default_model | BOOL | v3 | +0xd8: a fighter model description's default model |
| unknown_cc | BOOL | v3 | +0xcc, unknown |
| unknown_d0 | BOOL | v3 | +0xd0, unknown |

In `ArenaDescriptions` every v3 chunk ends with 30 zero bytes after these fields.

### Description (references chunk 1, v6)

`DE_CDescription::ObjProcessReferences` (0x701f17a0): `casts` (list), `icon`, `large_representation`
(a ControlCreator: the panel a chooser shows), `components` (list), `replacements` (list),
`medium_representation` (an image), then 0x28 zero bytes. Chunk 0 is an empty `DE_CObject` chunk.
(verified on all 12 arena descriptions: the decoded fields re-encode to the same bytes and leave
exactly the 0x28 filler)

### Description\Environment (contents chunk 1, v1)

`DE_CEnvironmentDescription::ObjProcessContents` (Creature 0x1500aa20): `i32` minimum and maximum
recommended players, then 20 zero bytes in `ArenaDescriptions`.

### Description\FighterModelDescription (contents chunk 1, v1)

A `GUID`: the fighter description the model belongs to.

### ControlCreator (contents chunk 0, v1)

`DE_CControlCreator::ObjProcessContents` (0x7021c270): name, `i32` layout, `i32` alignment,
`i32` text height (virtual pixels), `f32` image scale, then 48 zero bytes in `ArenaDescriptions`.
Its references chunk 1 lists the items it lays out in order: `Value\String` objects become text,
images become pictures, ControlCreators nest. See
[engine/src/game/descriptions.h](../engine/src/game/descriptions.h) for the layout rules.

### Values

`Value\*` contents start with a v1 chunk holding the name. Chunk 1 holds the value:
`Value\String` a string (v2), `Value\FLOAT` an `f32` (v1), `Value\RGBA` four `f32` (v1).

### Image (contents chunk 0)

`DE_CImage::ObjProcessContents` (0x7018d850): name, `i32` type, `i32` width, `i32` height,
`BOOL` mipmap (+0x4c), (v4+) `f32` unknown and `BOOL` adjust mipmap level (+0x5c). More fields
follow in v6 (unknown). `Image\Standard` adds a v7 chunk (unknown). The pixels are EX payloads
keyed `{0, index}`. `DE_CStandardImageCompressor::Load` (0x7018b4b0) reads them: a `u8`
version (above 1, the rest is a compressed stream), then repeated `{i32 channel, width × height
bytes}` until a negative channel. Channels are planar 8-bit: 0–2 colour, 3 alpha, 4 luminance.

### Camera (contents chunk 0)

`DE_CCamera::ObjProcessContents` (0x70187a70): name, (v3+) `f32` field of view, (v4+) `f32` near
and far.

### Other classes

Many more classes are decoded by the engine (meshes, lights, scripts, sounds, music, particles,
physics, fighters). Their layouts are described next to the code that reads them under
`engine/src` and in `re/FINDINGS.md`, and can be added to the editor's table
(`tools/deobjects-editor/src/schema.cpp`) as they are confirmed.

## Writing files back

`deobjects-edit` (unpack to JSON, pack again, no edits) reproduces every shipped file byte for byte:
the 65 files of 2.123, of the 2.090 disc, of the 2.095, 2.096, 2.097, 2.101 and 2.112 patches, and
the two `core` files (457 in all). What it takes, besides writing the layout above in order:

- Keep every chunk's size form: a size written as `0xFF` + `u32` although it would fit in a byte is
  written back the same way.
- Keep every byte after the known fields of a chunk (object info, agent contents, class chunks).
- Keep `value84`, the "create eagerly" BOOL and the agent's uninitialised fields as read (they
  hold debug-heap `0xCD` fill in some files).
- Keep each compressed object's stream unless its contents changed; LZRW3-A output is not
  reproduced by recompressing.
- Decode a field or list only when re-encoding it gives back the same bytes, and write references
  with AddReference's rule (above).

Untouched objects stay byte-identical when other objects in the file are edited: their contents
(compressed stream included), references, hierarchy and payloads are unchanged, and only file
offsets move (the object table's data offsets and the EX entries' payload offsets). The engine's `write_objects_file` (recordings) uses the same
writer (`engine/src/de/objects_model.cpp`). (verified)

## Open questions

Shipped data does not explain these. The original source or editor could answer them:

1. **Object info +0x84** (`value84`): small integers per object (0–36 in `ArenaDescriptions`).
   A count of something, or an edit or revision counter? FINDINGS 173 notes that in some files
   it holds the debug heap's 0xcd fill, so it may never be set on purpose.
2. **Object info "create eagerly" BOOL**: 0 for every object seen. Is it set anywhere in the
   game's data, and when did the editor set it?
3. **The agent's own contents**: block +0xd0, the GUID (all zero) and BOOL +0x108.
4. **EX payload flag** (the `u8` before each payload): 0, 1 or 2. Is it a type, a compression
   method, or an "imported" marker?
5. **Description** +0xd4 (`f32`), +0xcc and +0xd0 (`BOOL`), and the 30 trailing bytes of the v3
   chunk: reserved fields, or data from a later version?
6. **Image v6 fields after the mipmap BOOL** (starting `07 00 00 00` in arena images) and the
   `Image\Standard` v7 chunk.
7. **The 0x28 zero bytes** at the end of a description's references, and the zero runs after
   ControlCreator and EnvironmentDescription fields: reserved space?
8. **Class paths**: the full list of factory paths and the C++ class behind each, to make sure the
   editor's class levels line up with the chunks.
