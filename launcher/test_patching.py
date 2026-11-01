"""Cumulative update boundaries, including install cancellation and safe retry."""
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import bsdiff4
import install
import patching


def metadata(data):
    return dict(size=len(data), sha256=hashlib.sha256(data).hexdigest())


class CumulativePatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.resources = self.root / "resources"
        self.game = self.root / "staged"
        self.resources.mkdir()
        self.game.mkdir()
        self.originals = {"Data/OMFBGGUI.DEObjects": b"old menu", "Data/Fighter.DEObjects": b"old fighter",
                          "Engine/dbghelp.dll": b"unchanged"}
        self.finals = dict(self.originals, **{"Data/OMFBGGUI.DEObjects": b"new menu", "Data/Fighter.DEObjects": b"new fighter"})
        self.items, self.deltas = [], {}
        for index, (name, data) in enumerate(self.originals.items()):
            target = self.game / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            item = dict(path=name, **metadata(self.finals[name]), source_size=len(data),
                        source_sha256=hashlib.sha256(data).hexdigest())
            if data != self.finals[name]:
                delta = bsdiff4.diff(data, self.finals[name])
                member = f"deltas/{index:03d}.bsdiff"
                self.deltas[member] = delta
                item.update(delta=member, delta_size=len(delta), delta_sha256=hashlib.sha256(delta).hexdigest())
            self.items.append(item)
        self.target = dict(version="2.123", files=[dict(path=name, **metadata(data)) for name, data in self.finals.items()])
        self.manifest = dict(format=1, source_version="2.090", target_version="2.123", files=self.items)
        self.write_bundle()

    def write_bundle(self):
        (self.resources / "game-files.json").write_text(json.dumps(self.target))
        archive = self.resources / "patches" / patching.PATCH_NAME
        archive.parent.mkdir(exist_ok=True)
        with zipfile.ZipFile(archive, "w") as z:
            for name, delta in self.deltas.items():
                z.writestr(name, delta)
            z.writestr("manifest.json", json.dumps(self.manifest))
        (self.resources / "cumulative-patch.json").write_text(json.dumps(metadata(archive.read_bytes())))
        return archive

    def test_direct_patch_preserves_unchanged_files_and_matches_final_manifest(self):
        progress = []
        patching.apply_patch(self.game, self.resources, lambda done, total: progress.append((done, total)))
        install.verify(self.game, self.target)
        self.assertEqual(progress[-1], (3, 3))
        self.assertEqual((self.game / "Engine/dbghelp.dll").read_bytes(), b"unchanged")

    def test_wrong_disc_is_rejected_before_any_file_changes(self):
        (self.game / "Data/Fighter.DEObjects").write_bytes(b"bad fighter")
        with self.assertRaisesRegex(ValueError, "original English 2.090"):
            patching.apply_patch(self.game, self.resources)
        self.assertEqual((self.game / "Data/OMFBGGUI.DEObjects").read_bytes(), b"old menu")

    def test_corrupt_or_truncated_bundle_is_rejected(self):
        archive = self.write_bundle()
        for data in (archive.read_bytes()[:-1], b"x" * archive.stat().st_size):
            archive.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "incomplete|integrity"):
                patching.apply_patch(self.game, self.resources)
        self.assertEqual((self.game / "Data/OMFBGGUI.DEObjects").read_bytes(), b"old menu")

    def test_incomplete_file_list_is_rejected(self):
        self.manifest["files"] = self.items[:-1]
        self.write_bundle()
        with self.assertRaisesRegex(ValueError, "incomplete file list"):
            patching.apply_patch(self.game, self.resources)

    def test_delta_cannot_allocate_a_different_output_size(self):
        member = self.items[0]["delta"]
        delta = bytearray(self.deltas[member])
        delta[24:32] = (2**50).to_bytes(8, "little")
        self.deltas[member] = bytes(delta)
        self.items[0]["delta_sha256"] = hashlib.sha256(delta).hexdigest()
        self.write_bundle()
        with self.assertRaisesRegex(ValueError, "invalid delta header"):
            patching.apply_patch(self.game, self.resources)

    def test_bad_delta_with_valid_header_never_replaces_its_source(self):
        member = self.items[0]["delta"]
        self.deltas[member] = self.deltas[member][:32] + b"invalid bzip data"
        self.items[0].update(delta_size=len(self.deltas[member]), delta_sha256=hashlib.sha256(self.deltas[member]).hexdigest())
        self.write_bundle()
        with self.assertRaises((ValueError, OSError)):
            patching.apply_patch(self.game, self.resources)
        self.assertEqual((self.game / "Data/OMFBGGUI.DEObjects").read_bytes(), b"old menu")

    def test_symlink_cannot_escape_staging(self):
        original = self.game / "Data/Fighter.DEObjects"
        outside = self.root / "outside"
        outside.write_bytes(original.read_bytes())
        original.unlink()
        original.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "Unsupported disc file"):
            patching.apply_patch(self.game, self.resources)
        self.assertEqual(outside.read_bytes(), b"old fighter")

    def test_reconstructed_bytes_must_match_the_official_target(self):
        member = self.items[0]["delta"]
        self.deltas[member] = bsdiff4.diff(b"old menu", b"bad menu")
        self.items[0].update(delta_size=len(self.deltas[member]), delta_sha256=hashlib.sha256(self.deltas[member]).hexdigest())
        self.write_bundle()
        with self.assertRaisesRegex(ValueError, "could not reconstruct"):
            patching.apply_patch(self.game, self.resources)
        self.assertEqual((self.game / "Data/OMFBGGUI.DEObjects").read_bytes(), b"old menu")

    def test_cancellation_keeps_published_game_and_retry_is_fully_offline(self):
        disc = self.root / "disc/Setup"
        disc.mkdir(parents=True)
        (disc / "setup.ini").write_text("ProductGUID=" + install.PRODUCT_GUID)
        for name in ("data1.cab", "data1.hdr", "data2.cab"):
            (disc / name).touch()
        home = self.root / "home"
        (home / "game").mkdir(parents=True)
        (home / "game/previous").write_text("keep this install")

        def extract(command, _log):
            shutil.copytree(self.game, Path(command[2]) / "App_Executables")

        def cancel(stage, status, detail):
            if stage == "patches" and detail.startswith("Applying bundled"):
                raise KeyboardInterrupt

        with patch("install.run", side_effect=extract), patch("install.emit", side_effect=cancel):
            with self.assertRaises(KeyboardInterrupt):
                install.install(disc.parent, home, self.resources)
        self.assertEqual((home / "game/previous").read_text(), "keep this install")
        self.assertFalse(list(home.glob(".install-*")))
        with patch("install.run", side_effect=extract), patch("install.emit"), patch("socket.socket", side_effect=AssertionError("network access")):
            install.install(disc.parent, home, self.resources)
        install.verify(home / "game", self.target)
        self.assertTrue((home / "verified-files.json").is_file())
        self.assertFalse(list(home.glob(".install-*")))

    def test_versioned_real_bundle_matches_launcher_manifest(self):
        patching.validate_bundle(Path(__file__).parent)


if __name__ == "__main__":
    unittest.main()
