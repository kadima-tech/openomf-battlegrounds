"""Read one directory out of an ISO 9660 disc image, without mounting it or external tools.

Windows has no bsdtar at a fixed path, so setup reads the image itself. Only what a 2003
retail CD uses is supported: the primary volume descriptor, or a Joliet supplementary one
for its original file names, with single-extent, non-interleaved files. Anything else is
rejected rather than guessed at. Names are compared case-insensitively.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import struct

SECTOR = 2048
JOLIET_ESCAPES = (b"%/@", b"%/C", b"%/E")
MAX_ENTRIES = 10_000
MAX_DEPTH = 8


@dataclass
class Entry:
    name: str
    extent: int
    size: int
    directory: bool


class ImageReader:
    def __init__(self, stream, image_size: int):
        self.stream = stream
        self.image_size = image_size
        self.block = SECTOR
        self.joliet = False
        self.root = self._volume_root()

    def _read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.image_size:
            raise ValueError("The disc image is truncated or damaged.")
        self.stream.seek(offset)
        data = self.stream.read(size)
        if len(data) != size:
            raise ValueError("The disc image is truncated or damaged.")
        return data

    def _volume_root(self) -> Entry:
        primary = joliet = None
        for sector in range(16, 16 + 64):
            descriptor = self._read(sector * SECTOR, SECTOR)
            if descriptor[1:6] != b"CD001":
                break
            kind = descriptor[0]
            if kind == 255:
                break
            if kind == 1 and primary is None:
                primary = descriptor
            elif kind == 2 and descriptor[88:91] in JOLIET_ESCAPES and joliet is None:
                joliet = descriptor
        descriptor = joliet or primary
        if descriptor is None:
            raise ValueError("This file is not an ISO 9660 disc image.")
        self.joliet = descriptor is joliet
        self.block = struct.unpack_from("<H", descriptor, 128)[0]
        if self.block not in (512, 1024, 2048):
            raise ValueError("The disc image uses an unsupported block size.")
        root = self._record(descriptor[156:190], root=True)
        if not root.directory:
            raise ValueError("The disc image has no root directory.")
        return root

    def _record(self, data: bytes, root: bool = False) -> Entry | None:
        if len(data) < 34 or data[0] < 34 or 33 + data[32] > min(data[0], len(data)):
            raise ValueError("The disc image has a damaged directory.")
        extent, size = struct.unpack_from("<I4xI", data, 2)
        flags, unit, gap, name_length = data[25], data[26], data[27], data[32]
        if flags & 0x80 or unit or gap:
            raise ValueError("The disc image uses multi-extent or interleaved files, which are not supported.")
        raw = data[33:33 + name_length]
        if root:
            return Entry("", extent, size, bool(flags & 0x02))
        if raw in (b"\x00", b"\x01"):
            return None
        name = raw.decode("utf-16-be" if self.joliet else "latin-1", errors="replace")
        name = name.split(";", 1)[0]
        if not flags & 0x02:
            name = name.rstrip(".")
        return Entry(name, extent, size, bool(flags & 0x02))

    def entries(self, directory: Entry) -> list[Entry]:
        data = self._read(directory.extent * self.block, directory.size)
        result, offset = [], 0
        while offset < len(data):
            length = data[offset]
            if length == 0:
                # Records never cross a sector; the rest of this one is padding.
                offset = (offset // SECTOR + 1) * SECTOR
                continue
            if offset + length > len(data):
                raise ValueError("The disc image has a damaged directory.")
            entry = self._record(data[offset:offset + length])
            if entry is not None:
                result.append(entry)
                if len(result) > MAX_ENTRIES:
                    raise ValueError("The disc image has too many files.")
            offset += length
        return result

    def child(self, directory: Entry, name: str) -> Entry | None:
        return next((e for e in self.entries(directory) if e.name.casefold() == name.casefold()), None)

    def extract(self, directory: Entry, destination: Path, depth: int = 0) -> None:
        if depth > MAX_DEPTH:
            raise ValueError("The disc image's folders are nested too deeply.")
        destination.mkdir(parents=True, exist_ok=True)
        for entry in self.entries(directory):
            if (not entry.name or entry.name in (".", "..") or any(c in entry.name for c in '/\\:*?"<>|\0')):
                raise ValueError(f"The disc image contains an invalid file name: {entry.name!r}")
            target = destination / entry.name
            if entry.directory:
                self.extract(entry, target, depth + 1)
                continue
            start = entry.extent * self.block
            if start + entry.size > self.image_size:
                raise ValueError("The disc image is truncated or damaged.")
            with target.open("wb") as out:
                remaining = entry.size
                self.stream.seek(start)
                while remaining:
                    chunk = self.stream.read(min(remaining, 1024 * 1024))
                    if not chunk:
                        raise ValueError("The disc image is truncated or damaged.")
                    out.write(chunk)
                    remaining -= len(chunk)


def extract_directory(image: Path, name: str, destination: Path) -> Path:
    """Copy the top-level folder `name` (any case) and its contents to destination/<its name>."""
    with image.open("rb") as stream:
        reader = ImageReader(stream, image.stat().st_size)
        folder = reader.child(reader.root, name)
        if folder is None or not folder.directory:
            raise ValueError("This image does not contain the Battlegrounds installer.")
        target = destination / folder.name
        reader.extract(folder, target)
        return target
