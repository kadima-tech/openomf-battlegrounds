# Tools

Modding and data tools for *One Must Fall: Battlegrounds*, separate from the engine. Each one
builds on its own with CMake on Windows, macOS and Linux.

| Tool | What it does |
|---|---|
| [deobjects-editor](deobjects-editor) | `deobjects-edit`: unpack a `.DEObjects` file to editable JSON and pack it back, losslessly |

The `.DEObjects` format itself is documented in [docs/DEOBJECTS_FORMAT.md](../docs/DEOBJECTS_FORMAT.md).
The engine's own inspection tool, `deobj-dump` (`engine/tools/deobj_dump.cpp`), can also export
images, scripts and bytecode; see [DEVELOPING.md](../DEVELOPING.md).
