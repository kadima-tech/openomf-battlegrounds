"""Regression: Intel helpers need an explicit signature under Resources/bin."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from signing import sign_native_code


@unittest.skipUnless(sys.platform == "darwin", "macOS signing tools required")
class SigningTests(unittest.TestCase):
    def test_unsigned_intel_helper_is_signed(self):
        with tempfile.TemporaryDirectory(prefix="OpenBG signing ") as temporary:
            root = Path(temporary)
            source = root / "helper.c"
            source.write_text("int main(void) { return 0; }\n")
            resources = root / "Resources"
            helper = resources / "bin/unshield"
            helper.parent.mkdir(parents=True)
            # Cross-compile on Apple silicon as well: no Intel execution or
            # Rosetta is needed to inspect/sign the Mach-O that failed CI.
            subprocess.run(["xcrun", "clang", "-arch", "x86_64", "-mmacosx-version-min=13.0",
                            str(source), "-o", str(helper)], check=True)
            if subprocess.run(["codesign", "--verify", str(helper)], capture_output=True).returncode == 0:
                subprocess.run(["codesign", "--remove-signature", str(helper)], check=True)
            unsigned = subprocess.run(["codesign", "--verify", str(helper)], capture_output=True, text=True)
            self.assertNotEqual(unsigned.returncode, 0, "Regression fixture must start unsigned")
            sign_native_code(resources)
            signed = subprocess.run(["codesign", "--verify", "--strict", str(helper)], capture_output=True, text=True)
            self.assertEqual(signed.returncode, 0, signed.stderr)


if __name__ == "__main__":
    unittest.main()
