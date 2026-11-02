"""Startup readiness (readiness.py, used by the Linux launcher); mirrors test-readiness.swift."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import install
import readiness

GOOD = b"patched"


class ReadinessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="readiness test ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.file = self.root / "game-file"
        self.cache = self.root / "verified.json"
        self.manifest = {"version": "2.123", "files": [
            dict(path="game-file", size=len(GOOD), sha256=hashlib.sha256(GOOD).hexdigest())]}
        self.file.write_bytes(GOOD)

    def test_verification_cache_and_invalidation(self):
        self.assertTrue(readiness.verify(self.root, self.manifest, self.cache))
        self.assertTrue(self.cache.is_file(), "successful verification saves its cache")
        with patch("readiness.digest", side_effect=AssertionError("unchanged files must not be reread")):
            self.assertTrue(readiness.verify(self.root, self.manifest, self.cache))
        self.file.write_bytes(b"damaged")  # Same size.
        self.assertFalse(readiness.verify(self.root, self.manifest, self.cache))
        self.file.write_bytes(GOOD)
        self.assertTrue(readiness.verify(self.root, self.manifest, self.cache))

    def test_python_installer_cache_is_shared(self):
        native = readiness.verify(self.root, self.manifest, self.cache) and json.loads(self.cache.read_text())
        install.record_verified(self.root, self.manifest, self.cache)
        written = json.loads(self.cache.read_text())
        self.assertEqual(native["root"], written["root"])
        self.assertEqual(native["files"], [{key: entry[key] for key in readiness.STAMP_KEYS} for entry in written["files"]])
        with patch("readiness.digest", side_effect=AssertionError("installer cache must be accepted")):
            self.assertTrue(readiness.verify(self.root, self.manifest, self.cache))

    def test_expected_digest_change_corrupt_cache_and_missing_files(self):
        self.assertTrue(readiness.verify(self.root, self.manifest, self.cache))
        different = {"files": [dict(self.manifest["files"][0], sha256="0" * 64)]}
        self.assertFalse(readiness.verify(self.root, different, self.cache))
        self.cache.write_text("invalid json")
        self.assertTrue(readiness.verify(self.root, self.manifest, self.cache))
        self.cache.write_text(json.dumps({"root": str(self.root.resolve()), "files": [{"path": 1}]}))
        self.assertTrue(readiness.verify(self.root, self.manifest, self.cache))
        self.file.unlink()
        self.assertFalse(readiness.verify(self.root, self.manifest, self.cache))
        self.assertFalse(readiness.verify(self.root, {"files": []}, self.cache), "an empty manifest is never ready")

    def test_command_line_reports_first_ready_installation(self):
        resources, home = self.root / "resources", self.root / "home"
        resources.mkdir()
        (resources / "game-files.json").write_text(json.dumps(self.manifest))
        script = Path(readiness.__file__)
        command = [sys.executable, "-I", "-B", str(script), str(resources), str(home)]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
        (home / "game").mkdir(parents=True)
        (home / "game/game-file").write_bytes(GOOD)
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual((result.returncode, result.stdout.strip()), (0, str(home / "game")))
        legacy = self.root / "project/source/patched/v2123"
        legacy.mkdir(parents=True)
        (legacy / "game-file").write_bytes(GOOD)
        os.remove(home / "game/game-file")
        result = subprocess.run(command + [str(self.root / "project")], capture_output=True, text=True)
        self.assertEqual((result.returncode, result.stdout.strip()), (0, str(legacy)))


if __name__ == "__main__":
    unittest.main()
