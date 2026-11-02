"""Read-only ISO 9660 extraction for disc images, without bsdtar or mounting.

Linux desktops do not ship bsdtar by default, and loop-mounting an image needs
privileges. The Battlegrounds CD is a plain 2048-byte-sector ISO 9660 image with
Joliet names, so the installer copies the one directory it needs (Setup) with
this small reader. Only regular single-extent files are accepted; names are
checked so nothing can be written outside the destination.
"""
from __future__ import annotations

from pathlib import Path
import struct

SECTOR = 2048
JOLIET_ESCAPES = (b"%/@", b"%/C", b"%/E")
MAX_DEPTH = 16
MAX_ENTRIES = 100_000


class ImageError(ValueError):
    pass


class Entry:
    __slots__ = ("name", "extent", "size", "directory")

    def __init__(self, name: str, extent: int, size: int, directory: bool):
        self.name, self.extent, self.size, self.directory = name, extent, size, directory


class Image:
    def __init__(self, stream):
        self.stream = stream
        stream.seek(0, 2)
        self.length = stream.tell()
        primary = joliet = None
        for index in range(16, 16 + 64):
            descriptor = self.read(index * SECTOR, SECTOR)
            if len(descriptor) < SECTOR or descriptor[1:6] != b"CD001":
                break
            kind = descriptor[0]
            if kind == 255:
                break
            if kind == 1 and primary is None:
                primary = descriptor
            elif kind == 2 and descriptor[88:91] in JOLIET_ESCAPES and joliet is None:
                joliet = descriptor
        if primary is None:
            raise ImageError("This file is not an ISO 9660 disc image.")
        descriptor = joliet or primary
        self.joliet = joliet is not None
        if struct.unpack_from("<H", descriptor, 128)[0] != SECTOR:
            raise ImageError("This disc image uses an unsupported block size.")
        self.root = self.record(descriptor[156:190], root=True)
        self.entries = 0

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.length:
            raise ImageError("The disc image is truncated or damaged.")
        self.stream.seek(offset)
        return self.stream.read(size)

    def record(self, data: bytes, root: bool = False) -> Entry | None:
        if len(data) < 34 or data[0] < 34 or data[0] > len(data):
            raise ImageError("The disc image has a damaged directory.")
        extent, size = struct.unpack_from("<I", data, 2)[0], struct.unpack_from("<I", data, 10)[0]
        flags, unit, gap, length = data[25], data[26], data[27], data[32]
        raw = data[33:33 + length]
        if len(raw) != length:
            raise ImageError("The disc image has a damaged directory.")
        if root:
            if not flags & 0x02:
                raise ImageError("The disc image has a damaged directory.")
            return Entry("", extent, size, True)
        if raw in (b"\x00", b"\x01"):
            return None  # "." and ".."
        if flags & 0x80 or unit or gap:
            raise ImageError("The disc image uses multi-extent or interleaved files, which are not supported.")
        name = raw.decode("utf-16-be", "replace") if self.joliet else raw.decode("ascii", "replace")
        if not flags & 0x02:
            name = name.split(";", 1)[0]
            if not self.joliet:
                name = name.rstrip(".")
        if (not name or name in (".", "..") or "/" in name or "\\" in name or "\0" in name):
            raise ImageError("The disc image contains an invalid file name.")
        return Entry(name, extent, size, bool(flags & 0x02))

    def children(self, directory: Entry) -> list[Entry]:
        data = self.read(directory.extent * SECTOR, directory.size)
        result, offset = [], 0
        while offset < len(data):
            length = data[offset]
            if length == 0:
                # Records never span sectors; padding fills the rest of one.
                offset = (offset // SECTOR + 1) * SECTOR
                continue
            entry = self.record(data[offset:offset + length])
            offset += length
            if entry is None:
                continue
            self.entries += 1
            if self.entries > MAX_ENTRIES:
                raise ImageError("The disc image has too many files.")
            result.append(entry)
        names = [entry.name.casefold() for entry in result]
        if len(names) != len(set(names)):
            raise ImageError("The disc image contains conflicting file names.")
        return result

    def copy(self, entry: Entry, destination: Path, depth: int = 0) -> None:
        if depth > MAX_DEPTH:
            raise ImageError("The disc image's folders are nested too deeply.")
        if entry.directory:
            destination.mkdir()
            for child in self.children(entry):
                self.copy(child, destination / child.name, depth + 1)
            return
        start = entry.extent * SECTOR
        if start + entry.size > self.length:
            raise ImageError("The disc image is truncated or damaged.")
        self.stream.seek(start)
        remaining = entry.size
        with destination.open("xb") as output:
            while remaining:
                block = self.stream.read(min(remaining, 1024 * 1024))
                if not block:
                    raise ImageError("The disc image is truncated or damaged.")
                output.write(block)
                remaining -= len(block)


def extract_directory(image: Path, name: str, destination: Path) -> Path | None:
    """Copy the top-level directory `name` (matched case-insensitively) into
    `destination`. Returns its new path, or None if the image has no such folder."""
    with image.open("rb") as stream:
        reader = Image(stream)
        match = next((entry for entry in reader.children(reader.root)
                      if entry.directory and entry.name.casefold() == name.casefold()), None)
        if match is None:
            return None
        target = destination / match.name
        reader.copy(match, target)
        return target
