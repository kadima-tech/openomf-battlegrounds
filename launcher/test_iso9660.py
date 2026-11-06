"""ISO 9660 reading used by Linux and Windows setup; synthetic images, no game disc needed."""
from pathlib import Path, PurePosixPath, PureWindowsPath
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import install
from iso9660 import SECTOR, extract_directory


def both16(value):
    return struct.pack("<H", value) + struct.pack(">H", value)


def both32(value):
    return struct.pack("<I", value) + struct.pack(">I", value)


def record(name: bytes, extent: int, size: int, directory: bool) -> bytes:
    length = 33 + len(name) + (len(name) + 1) % 2
    body = bytes([length, 0]) + both32(extent) + both32(size) + bytes(7) + bytes([2 if directory else 0, 0, 0])
    body += both16(1) + bytes([len(name)]) + name
    return body.ljust(length, b"\0")


class ImageBuilder:
    """Lays out a tiny image: descriptors, then one extent per directory and file."""

    def __init__(self, joliet=False):
        self.joliet = joliet
        self.sectors = {}
        self.next = 20

    def name(self, text, directory):
        if self.joliet:
            return text.encode("utf-16-be")
        return text.upper().encode() + (b"" if directory else b";1")

    def file(self, data: bytes):
        extent = self.next
        self.sectors[extent] = data
        self.next += max(1, -(-len(data) // SECTOR))
        return extent, len(data)

    def directory(self, children, parent=None):
        """children: {name: bytes | dict}. Returns (extent, size) of the directory."""
        records = []
        for name, value in children.items():
            if isinstance(value, dict):
                extent, size = self.directory(value)
                records.append(record(self.name(name, True), extent, size, True))
            else:
                extent, size = self.file(value)
                records.append(record(self.name(name, False), extent, size, False))
        # Self/parent records first; records never cross a sector boundary.
        extent = self.next
        blocks, current = [], record(b"\0", extent, 0, True) + record(b"\1", extent, 0, True)
        for item in records:
            if len(current) + len(item) > SECTOR:
                blocks.append(current.ljust(SECTOR, b"\0"))
                current = b""
            current += item
        blocks.append(current.ljust(SECTOR, b"\0"))
        data = b"".join(blocks)
        size = len(data)
        # Patch the self record's extent and size now that both are known.
        data = record(b"\0", extent, size, True) + data[34:]
        self.sectors[extent] = data
        self.next += len(blocks)
        return extent, size

    def build(self, tree, truncate=0) -> bytes:
        root_extent, root_size = self.directory(tree)
        descriptor = bytearray(SECTOR)
        descriptor[0] = 2 if self.joliet else 1
        descriptor[1:7] = b"CD001\x01"
        if self.joliet:
            descriptor[88:91] = b"%/E"
        descriptor[128:132] = both16(SECTOR)
        descriptor[156:190] = record(b"\0", root_extent, root_size, True)
        terminator = bytearray(SECTOR)
        terminator[0] = 255
        terminator[1:7] = b"CD001\x01"
        image = bytearray(self.next * SECTOR)
        image[16 * SECTOR:17 * SECTOR] = descriptor
        image[17 * SECTOR:18 * SECTOR] = terminator
        for extent, data in self.sectors.items():
            image[extent * SECTOR:extent * SECTOR + len(data)] = data
        return bytes(image[:len(image) - truncate])


def disc_tree(**extra):
    setup = {"setup.ini": b"ProductGUID=" + install.PRODUCT_GUID.encode(),
             "data1.cab": b"cab one", "data1.hdr": b"header", "data2.cab": b"x" * 5000}
    setup.update(extra)
    return {"Setup": setup, "Autorun.inf": b"[autorun]"}


class ISO9660Tests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="iso test ")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def image(self, data: bytes) -> Path:
        path = self.root / "Battlegrounds.iso"
        path.write_bytes(data)
        return path

    def test_primary_volume_extracts_setup_folder(self):
        image = self.image(ImageBuilder().build(disc_tree()))
        folder = extract_directory(image, "Setup", self.root / "out")
        self.assertEqual(folder.name, "SETUP")
        self.assertEqual((folder / "DATA2.CAB").read_bytes(), b"x" * 5000)
        self.assertEqual(install.disc_setup(self.root / "out"), folder)

    def test_joliet_names_are_preferred(self):
        image = self.image(ImageBuilder(joliet=True).build(disc_tree()))
        folder = extract_directory(image, "SETUP", self.root / "out")
        self.assertEqual(folder.name, "Setup")
        self.assertEqual((folder / "data1.hdr").read_bytes(), b"header")

    def test_directories_spanning_sectors_and_subfolders(self):
        many = {f"file{index:03}.bin": bytes([index]) for index in range(120)}
        image = self.image(ImageBuilder().build(disc_tree(Extras={"nested.txt": b"nested"}, **many)))
        folder = extract_directory(image, "setup", self.root / "out")
        self.assertEqual((folder / "FILE119.BIN").read_bytes(), bytes([119]))
        self.assertEqual((folder / "EXTRAS/NESTED.TXT").read_bytes(), b"nested")

    def test_missing_installer_and_non_iso_files_are_rejected(self):
        image = self.image(ImageBuilder().build({"Other": {"a.txt": b"a"}}))
        with self.assertRaisesRegex(ValueError, "does not contain the Battlegrounds installer"):
            extract_directory(image, "Setup", self.root / "out")
        with self.assertRaisesRegex(ValueError, "not an ISO 9660"):
            extract_directory(self.image(bytes(40 * SECTOR)), "Setup", self.root / "out")

    def test_truncated_image_is_rejected(self):
        image = self.image(ImageBuilder().build(disc_tree(), truncate=SECTOR))
        with self.assertRaisesRegex(ValueError, "truncated"):
            extract_directory(image, "Setup", self.root / "out")

    def test_unsafe_names_are_rejected(self):
        image = self.image(ImageBuilder(joliet=True).build({"Setup": {"..\\evil.dll": b"x"}}))
        with self.assertRaisesRegex(ValueError, "invalid file name"):
            extract_directory(image, "Setup", self.root / "out")
        self.assertFalse((self.root / "evil.dll").exists())

    def test_windows_setup_reads_the_image_without_bsdtar(self):
        image = self.image(ImageBuilder().build(disc_tree()))
        work = self.root / "work"
        work.mkdir()
        with open(self.root / "install.log", "w") as log, patch.object(install, "WINDOWS", True), \
                patch.object(install.subprocess, "check_output", side_effect=AssertionError("bsdtar used")):
            disc = install.extract_setup(image, work, log)
        self.assertEqual(install.disc_setup(disc).name, "SETUP")


    def test_linux_setup_reads_the_image_without_bsdtar(self):
        image = self.image(ImageBuilder(joliet=True).build(disc_tree()))
        work = self.root / "work"
        work.mkdir()
        with open(self.root / "install.log", "w") as log, patch.object(install.sys, "platform", "linux"), \
                patch.object(install.subprocess, "check_output", side_effect=AssertionError("bsdtar used")):
            disc = install.extract_setup(image, work, log)
        self.assertEqual(install.disc_setup(disc).name, "Setup")

    @unittest.skipUnless(shutil.which("xorriso"), "xorriso is not installed")
    def test_image_written_by_xorriso(self):
        tree = self.root / "tree/Setup"
        tree.mkdir(parents=True)
        (tree / "setup.ini").write_text("ProductGUID=" + install.PRODUCT_GUID)
        for name in ("data1.cab", "data1.hdr", "data2.cab"):
            (tree / name).write_bytes(name.encode() * 5000)
        image = self.root / "xorriso.iso"
        subprocess.run(["xorriso", "-as", "mkisofs", "-J", "-quiet", "-o", str(image), str(tree.parent)], check=True)
        setup = extract_directory(image, "Setup", self.root / "out")
        self.assertEqual((setup / "data2.cab").read_bytes(), b"data2.cab" * 5000)
        self.assertEqual(install.disc_setup(self.root / "out"), setup)


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
                self.skipTest("names that differ only by case are the same file here (macOS, Windows)")
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

class LockTests(unittest.TestCase):
    def test_second_installer_is_refused_until_the_first_finishes(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "install.lock"
            with install.SetupLock(path):
                with self.assertRaisesRegex(ValueError, "already running"):
                    with install.SetupLock(path):
                        pass
            with install.SetupLock(path):
                pass

    def test_unshield_gets_forward_slashes_on_windows(self):
        # unshield finds data1.hdr and data2.cab by splitting the cabinet path at the last "/".
        cabinet = PureWindowsPath(r"C:\Users\player\Battlegrounds Files\.install-1a\disc\Setup\data1.cab")
        with patch.object(install, "WINDOWS", True):
            self.assertEqual(install.unshield_path(cabinet),
                             "C:/Users/player/Battlegrounds Files/.install-1a/disc/Setup/data1.cab")
        with patch.object(install, "WINDOWS", False):
            self.assertEqual(install.unshield_path(PurePosixPath("/tmp/disc/Setup/data1.cab")), "/tmp/disc/Setup/data1.cab")

    def test_unshield_creates_relative_folders_on_windows(self):
        # unshield creates folders from the start of the path; a bare "C:" cannot be created.
        work = PureWindowsPath(r"C:\Users\player\Battlegrounds Files\.install-1a")
        cabinet = PureWindowsPath(r"D:\Setup\data1.cab")  # a real CD
        with patch.object(install, "WINDOWS", True):
            command, folder = install.unshield_command(PureWindowsPath("res"), work, work / "unpacked", cabinet)
        self.assertEqual(command[1:], ["-d", "unpacked", "x", "D:/Setup/data1.cab"])
        self.assertEqual(folder, work)
        with patch.object(install, "WINDOWS", False):
            command, folder = install.unshield_command(PurePosixPath("res"), PurePosixPath("/w"), PurePosixPath("/w/unpacked"),
                                                      PurePosixPath("/cd/Setup/data1.cab"))
        self.assertEqual(command[1:], ["-d", "/w/unpacked", "x", "/cd/Setup/data1.cab"])
        self.assertIsNone(folder)

    def test_windows_tools_have_exe_names(self):
        with patch.object(install, "WINDOWS", True):
            self.assertEqual(install.tool(Path("res"), "unshield"), Path("res/bin/unshield.exe"))
        with patch.object(install, "WINDOWS", False):
            self.assertEqual(install.tool(Path("res"), "unshield"), Path("res/bin/unshield"))


if __name__ == "__main__":
    unittest.main()
