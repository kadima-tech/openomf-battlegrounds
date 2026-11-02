"""ISO 9660 extraction used by Linux setup; synthetic images, no game disc needed."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

import install
import iso9660

SECTOR = iso9660.SECTOR


def both(value, size):
    """ISO 9660 stores numbers in both byte orders."""
    return value.to_bytes(size, "little") + value.to_bytes(size, "big")


def record(name: bytes, extent: int, size: int, directory: bool, flags: int = 0) -> bytes:
    body = bytes([0]) + both(extent, 4) + both(size, 4) + bytes(7)
    body += bytes([flags | (0x02 if directory else 0), 0, 0]) + both(1, 2) + bytes([len(name)]) + name
    body += b"\0" if len(body) % 2 == 0 else b""  # The length byte counts the padding.
    return bytes([len(body) + 1]) + body


class Builder:
    """Lay out a tiny image: descriptors, then each directory and file in its own sectors.

    `tree` maps names to bytes (files) or nested dicts (directories). Joliet
    images keep the given case; primary-only images use upper case and ";1".
    """

    def __init__(self, tree, joliet=True, file_flags=0):
        self.joliet, self.file_flags = joliet, file_flags
        self.sectors: dict[int, bytes] = {}
        self.next = 20
        root_extent, root_size = self.directory(tree, None)
        root = record(b"\0", root_extent, root_size, True)
        pvd = bytearray(SECTOR)
        pvd[0:7] = b"\x01CD001\x01"
        pvd[128:132] = both(SECTOR, 2)
        pvd[156:156 + len(root)] = root
        self.sectors[16] = bytes(pvd)
        terminator = 17
        if joliet:
            svd = bytearray(pvd)
            svd[0] = 2
            svd[88:91] = b"%/E"
            self.sectors[17] = bytes(svd)
            terminator = 18
        self.sectors[terminator] = b"\xffCD001\x01" + bytes(SECTOR - 7)

    def name(self, name: str, directory: bool) -> bytes:
        if self.joliet:
            return (name if directory else name + ";1").encode("utf-16-be")
        return (name.upper() if directory else name.upper() + ";1").encode()

    def allocate(self, data: bytes) -> int:
        extent = self.next
        self.next += max(1, -(-len(data) // SECTOR))
        self.sectors[extent] = data
        return extent

    def directory(self, tree, parent):
        records = []
        for name, value in tree.items():
            if isinstance(value, dict):
                records.append((name, *self.directory(value, None), True))
            else:
                records.append((name, self.allocate(value), len(value), False))
        size = SECTOR
        extent = self.next
        self.next += 1
        data = record(b"\0", extent, size, True) + record(b"\1", extent, size, True)
        for name, child_extent, child_size, directory in records:
            data += record(self.name(name, directory), child_extent, child_size, directory,
                           0 if directory else self.file_flags)
        assert len(data) <= SECTOR
        self.sectors[extent] = data
        return extent, size

    def write(self, path: Path) -> Path:
        image = bytearray(self.next * SECTOR)
        for index, data in self.sectors.items():
            image[index * SECTOR:index * SECTOR + len(data)] = data
        path.write_bytes(bytes(image))
        return path


DISC = {
    "Setup": {"setup.ini": b"ProductGUID=" + install.PRODUCT_GUID.encode(), "data1.cab": b"cab" * 1000,
              "data1.hdr": b"hdr", "data2.cab": b"x" * (SECTOR + 7), "Sub": {"nested.txt": b"nested"}},
    "Autorun.inf": b"[autorun]",
}


class ISO9660Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="iso test ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.out = self.root / "out"
        self.out.mkdir()

    def test_joliet_names_and_contents_are_extracted(self):
        image = Builder(DISC).write(self.root / "disc.iso")
        setup = iso9660.extract_directory(image, "setup", self.out)
        self.assertEqual(setup, self.out / "Setup")
        self.assertEqual((setup / "data2.cab").read_bytes(), DISC["Setup"]["data2.cab"])
        self.assertEqual((setup / "Sub/nested.txt").read_bytes(), b"nested")
        self.assertFalse((self.out / "Autorun.inf").exists())
        self.assertEqual(install.disc_setup(self.out), setup)

    def test_primary_volume_names_without_joliet(self):
        image = Builder(DISC, joliet=False).write(self.root / "plain.iso")
        setup = iso9660.extract_directory(image, "Setup", self.out)
        self.assertEqual(setup.name, "SETUP")
        self.assertEqual((setup / "DATA1.HDR").read_bytes(), b"hdr")
        self.assertEqual(install.disc_setup(self.out), setup)

    def test_missing_directory_returns_none(self):
        image = Builder({"Other": {"file": b"x"}}).write(self.root / "other.iso")
        self.assertIsNone(iso9660.extract_directory(image, "Setup", self.out))

    def test_non_iso_and_truncated_images_are_rejected(self):
        junk = self.root / "junk.iso"
        junk.write_bytes(b"not a disc" * 10000)
        with self.assertRaisesRegex(ValueError, "not an ISO 9660"):
            iso9660.extract_directory(junk, "Setup", self.out)
        image = Builder(DISC).write(self.root / "cut.iso")
        image.write_bytes(image.read_bytes()[:-SECTOR])
        with self.assertRaisesRegex(ValueError, "truncated"):
            iso9660.extract_directory(image, "Setup", self.out)

    def test_unsafe_names_cannot_escape_destination(self):
        for name in ("..", "a/b", "a\\b"):
            with self.subTest(name=name):
                image = Builder({"Setup": {name: b"evil"}}).write(self.root / "evil.iso")
                with self.assertRaisesRegex(ValueError, "invalid file name"):
                    iso9660.extract_directory(image, "Setup", self.out)
                self.assertFalse((self.root / "b").exists())
                shutil.rmtree(self.out)
                self.out.mkdir()

    def test_multi_extent_files_are_rejected(self):
        image = Builder(DISC, file_flags=0x80).write(self.root / "multi.iso")
        with self.assertRaisesRegex(ValueError, "multi-extent"):
            iso9660.extract_directory(image, "Setup", self.out)

    @unittest.skipUnless(shutil.which("xorriso"), "xorriso is not installed")
    def test_image_written_by_xorriso(self):
        tree = self.root / "tree/Setup"
        tree.mkdir(parents=True)
        (tree / "setup.ini").write_text("ProductGUID=" + install.PRODUCT_GUID)
        for name in ("data1.cab", "data1.hdr", "data2.cab"):
            (tree / name).write_bytes(name.encode() * 5000)
        image = self.root / "xorriso.iso"
        subprocess.run(["xorriso", "-as", "mkisofs", "-J", "-quiet", "-o", str(image), str(tree.parent)], check=True)
        setup = iso9660.extract_directory(image, "Setup", self.out)
        self.assertEqual((setup / "data2.cab").read_bytes(), b"data2.cab" * 5000)
        self.assertEqual(install.disc_setup(self.out), setup)


def case_sensitive(folder: Path) -> bool:
    probe = folder / "Case-Probe"
    probe.touch()
    return not (folder / "case-probe").exists()


class ManifestCaseTests(unittest.TestCase):
    def test_extracted_names_are_renamed_to_manifest_case(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "game"
            root.mkdir()
            if not case_sensitive(Path(temporary)):
                self.skipTest("names that differ only by case are the same file here (macOS)")
            (root / "DATA").mkdir()
            (root / "DATA/omfbggui.deobjects").write_bytes(b"menu")
            (root / "Engine").mkdir()
            (root / "Engine/Core.ModuleDLL").write_bytes(b"core")
            manifest = {"files": [{"path": "Data/OMFBGGUI.DEObjects"}, {"path": "Engine/Core.ModuleDLL"},
                                  {"path": "Data/Missing.DEObjects"}]}
            install.match_manifest_case(root, manifest)
            self.assertEqual(sorted(p.name for p in root.iterdir()), ["Data", "Engine"])
            self.assertEqual([p.name for p in (root / "Data").iterdir()], ["OMFBGGUI.DEObjects"])
            self.assertEqual((root / "Data/OMFBGGUI.DEObjects").read_bytes(), b"menu")


if __name__ == "__main__":
    unittest.main()
