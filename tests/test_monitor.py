"""Linux daemon regression tests; set ZYBO_MONITOR_BINARY to the built program."""
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time
import unittest

BINARY = Path(os.environ.get("ZYBO_MONITOR_BINARY", "/nonexistent")).resolve()


@unittest.skipUnless(BINARY.is_file(), "requires a Linux zybo-monitor binary")
class MonitorTests(unittest.TestCase):
    def test_invalid_options_fail_before_hardware_access(self):
        for args in (("-i", "-1"), ("-i", "60x"), ("-w", "nan"), ("-w", "inf"), ("extra",)):
            with self.subTest(args=args):
                result = subprocess.run([BINARY, *args], capture_output=True, timeout=3)
                self.assertEqual(result.returncode, 1)
                self.assertTrue(result.stderr)

    def test_eof_button_device_is_disabled_once(self):
        # A regular empty file is always poll-readable. Treating EOF as success
        # would leave the daemon spinning instead of sleeping on its timer.
        with tempfile.NamedTemporaryFile() as device:
            process = subprocess.Popen([BINARY, "-v", "-d", device.name], stderr=subprocess.PIPE,
                                       env=dict(os.environ, LC_ALL="C"))
            try:
                # Missing XADC hardware also reports ENODEV on a host PC.
                # Observe this device's read error before testing for repeats.
                expected = f"read {device.name}: No such device".encode()
                error = b""
                deadline = time.monotonic() + 5
                while expected not in error:
                    remaining = deadline - time.monotonic()
                    self.assertGreater(remaining, 0, "button EOF was not handled")
                    readable, _, _ = select.select([process.stderr], [], [], remaining)
                    self.assertTrue(readable, "button EOF was not handled")
                    chunk = os.read(process.stderr.fileno(), 4096)
                    self.assertTrue(chunk, "monitor exited before handling button EOF")
                    error += chunk
                time.sleep(0.2)
                process.terminate()
                _, remaining_error = process.communicate(timeout=3)
                error += remaining_error
                self.assertEqual(process.returncode, 0)
                self.assertEqual(error.count(expected), 1)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    unittest.main()
