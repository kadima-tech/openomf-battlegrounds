"""Exercise launch.command with a fake builder; no game or downloads needed."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class LaunchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="launcher test ")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        source = Path(__file__).resolve().parent.parent
        for name in ("launch.command", "launcher/build_state.py", "LICENSE", "THIRD_PARTY_NOTICES.md",
                     "re/scripts/directx_tables.py"):
            target = self.repo / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source / name, target)
        (self.repo / "engine").mkdir()
        self.source = self.repo / "engine/source.cpp"
        self.source.write_text("original source")
        builder = self.repo / "launcher/build-app.sh"
        builder.write_text('''#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
echo build >> "$repo/builds.log"
if [ -f "$repo/fail-build" ]; then exit 1; fi
mkdir -p "$repo/dist/Battlegrounds.app/Contents/MacOS"
printf '#!/bin/bash\\necho "Launched: $*"\\n' > "$repo/dist/Battlegrounds.app/Contents/MacOS/Battlegrounds"
chmod +x "$repo/dist/Battlegrounds.app/Contents/MacOS/Battlegrounds"
python3 "$repo/launcher/build_state.py" > "$repo/dist/.launcher-build-state"
''')
        builder.chmod(0o755)

    def launch(self, success=True):
        result = subprocess.run([str(self.repo / "launch.command"), "--preview"],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result.stdout

    def count_builds(self):
        return len((self.repo / "builds.log").read_text().splitlines())

    def test_build_once_then_reuse_and_forward_arguments(self):
        self.assertIn("Launched: --preview", self.launch())
        self.launch()
        self.assertEqual(self.count_builds(), 1)
        # Build outputs and Python bytecode must not invalidate the app.
        for name in ("launcher/build/output", "engine/build-debug/output", "launcher/__pycache__/code.pyc"):
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        self.launch()
        self.assertEqual(self.count_builds(), 1)

    def test_edit_addition_and_deletion_rebuild_even_with_old_timestamps(self):
        self.launch()
        self.source.write_text("updated source")
        os.utime(self.source, (1, 1))
        self.launch()
        asset = self.repo / "launcher/new-asset"
        asset.write_bytes(b"new")
        self.launch()
        asset.unlink()
        self.launch()
        self.assertEqual(self.count_builds(), 4)

    def test_failed_rebuild_never_launches_stale_app(self):
        self.launch()
        self.source.write_text("updated source")
        (self.repo / "fail-build").touch()
        self.assertNotIn("Launched:", self.launch(success=False))

    def test_legacy_app_without_state_rebuilds(self):
        self.launch()
        (self.repo / "dist/.launcher-build-state").unlink()
        self.launch()
        self.assertEqual(self.count_builds(), 2)

    def test_missing_executable_rebuilds_even_with_matching_state(self):
        self.launch()
        (self.repo / "dist/Battlegrounds.app/Contents/MacOS/Battlegrounds").unlink()
        self.launch()
        self.assertEqual(self.count_builds(), 2)
