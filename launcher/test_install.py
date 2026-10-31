"""Installer failure boundaries; no original game data is needed for these tests."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import install


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_readiness_checks_content_not_only_directory_or_size(self):
        data = self.root / "Data"
        data.mkdir()
        file = data / "OMFBGGUI.DEObjects"
        file.write_bytes(b"patched")
        manifest = {"files": [dict(path="Data/OMFBGGUI.DEObjects", size=7,
                                   sha256=hashlib.sha256(b"patched").hexdigest())]}
        install.verify(self.root, manifest)
        file.write_bytes(b"damaged")  # Same size must still fail.
        with self.assertRaisesRegex(ValueError, "repair"):
            install.verify(self.root, manifest)
        file.unlink()
        with self.assertRaisesRegex(ValueError, "missing"):
            install.verify(self.root, manifest)

    def test_empty_manifest_cannot_count_as_ready(self):
        with self.assertRaisesRegex(ValueError, "empty"):
            install.verify(self.root, {"files": []})

    def test_verified_cache_records_native_file_identity(self):
        file = self.root / "game-file"
        file.write_bytes(b"patched")
        item = dict(path=file.name, size=7, sha256=hashlib.sha256(b"patched").hexdigest())
        manifest = {"files": [item]}
        install.verify(self.root, manifest)
        cache = self.root / "verified.json"
        install.record_verified(self.root, manifest, cache)
        result = json.loads(cache.read_text())
        self.assertEqual(result["root"], str(self.root.resolve()))
        self.assertEqual(result["files"][0]["inode"], file.stat().st_ino)
        self.assertEqual(result["files"][0]["sha256"], item["sha256"])
        self.assertEqual(result["files"][0]["modified"], f"{file.stat().st_mtime_ns // 10**9}:{file.stat().st_mtime_ns % 10**9}")

    def test_disc_detection_is_case_insensitive_and_checks_identity(self):
        setup = self.root / "SETUP"
        setup.mkdir()
        (setup / "SETUP.INI").write_text("ProductGUID=" + install.PRODUCT_GUID.upper())
        for name in ["DATA1.CAB", "DATA1.HDR", "DATA2.CAB"]:
            (setup / name).touch()
        self.assertEqual(install.disc_setup(self.root), setup)
        (setup / "SETUP.INI").write_text("AppName=An unrelated game")
        with self.assertRaisesRegex(ValueError, "not a One Must Fall"):
            install.disc_setup(self.root)

    def test_publish_rolls_back_if_rename_fails(self):
        old = self.root / "game"
        old.mkdir()
        (old / "save").write_text("old install")
        original_replace = Path.replace
        staged = self.root / "staged"
        staged.mkdir()
        def replace(source, destination):
            if source == staged:
                raise OSError("disk failure")
            return original_replace(source, destination)
        with patch.object(Path, "replace", replace), self.assertRaises(OSError):
            install.publish(staged, old)
        self.assertEqual((old / "save").read_text(), "old install")

    def test_publish_recovers_interrupted_swap(self):
        backup = self.root / "game.previous"
        backup.mkdir()
        (backup / "original").touch()
        staged = self.root / "new"
        staged.mkdir()
        (staged / "ready").touch()
        install.publish(staged, self.root / "game")
        self.assertTrue((self.root / "game/ready").exists())
        self.assertFalse(backup.exists())



if __name__ == "__main__":
    unittest.main()
