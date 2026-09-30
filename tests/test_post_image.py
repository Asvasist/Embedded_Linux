"""Ensure early post-image errors cannot leave a stale deployable SD image."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipIf(os.name == "nt", "requires POSIX shell and executable symlinks")
class PostImageTests(unittest.TestCase):
    def test_missing_inputs_invalidate_old_release(self):
        for missing in ("ZYBO_SIGNING_DIR", "ZYBO_FSBL"):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                host = root / "host/bin"
                host.mkdir(parents=True)
                (host / "python3").symlink_to(sys.executable)
                images = root / "images"
                images.mkdir()
                (images / "sdcard.img").write_bytes(b"old release")
                env = dict(os.environ, HOST_DIR=str(root / "host"), BINARIES_DIR=str(images),
                           ZYBO_SIGNING_DIR="unused", ZYBO_FSBL="unused")
                env.pop(missing)
                result = subprocess.run(["/bin/sh", ROOT / "board/zybo-z7/secure/post-image.sh"],
                                        env=env, capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(missing.encode(), result.stderr)
                self.assertFalse((images / "sdcard.img").exists())


if __name__ == "__main__":
    unittest.main()
