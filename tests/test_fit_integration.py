"""Real FIT signature and payload tamper tests, using the pinned U-Boot tools.

ZYBO_UBOOT_TOOLS=/path/to/u-boot/tools python3 -m unittest discover -s tests -v
Requires openssl, dtc and fdtget in PATH. No board or production keys are used.
"""
import gzip
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("secure_boot", ROOT / "tools/secure_boot.py")
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)
TOOLS = Path(os.environ.get("ZYBO_UBOOT_TOOLS", "/nonexistent"))
AVAILABLE = all(shutil.which(name) for name in ("openssl", "dtc", "fdtget")) and all(
    (TOOLS / name).is_file() for name in ("mkimage", "fit_check_sign"))
if os.environ.get("ZYBO_REQUIRE_FIT_TESTS") == "1" and not AVAILABLE:
    raise RuntimeError("Required FIT tools are missing; refusing to silently skip cryptographic tests")


@unittest.skipUnless(AVAILABLE, "requires Linux dtc/fdtget and ZYBO_UBOOT_TOOLS")
class FitSignatureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.tool = (TOOLS / "fit_check_sign").resolve()
        cls.mkimage = (TOOLS / "mkimage").resolve()
        for name in ("trusted", "untrusted"):
            directory = cls.root / name
            directory.mkdir()
            boot.run("openssl", "genrsa", "-out", directory / "zybo.key", "2048")
            boot.run("openssl", "req", "-new", "-x509", "-batch", "-key", directory / "zybo.key",
                     "-out", directory / "zybo.crt", "-subj", "/CN=Disposable integration test/")
        (cls.root / "zImage").write_bytes(b"unique-kernel-payload-for-tamper-test")
        (cls.root / "board.dts").write_text('/dts-v1/; / { model = "fixture"; };\n')
        boot.run("dtc", "-I", "dts", "-O", "dtb", "-o", cls.root / "zybo-z7.dtb", cls.root / "board.dts")
        (cls.root / "rootfs.cpio.gz").write_bytes(gzip.compress(b"070701unique-rootfs-payload"))
        (cls.root / "system.its").write_text(boot.fit_source())
        for name in ("trusted", "untrusted"):
            boot.run("dtc", "-I", "dts", "-O", "dtb", "-p", "8192", "-o", cls.root / f"{name}.dtb",
                     cls.root / "board.dts")
            boot.run(cls.mkimage, "-f", "system.its", "-k", name, "-K", f"{name}.dtb", "-r",
                     f"{name}.itb", cwd=cls.root)

    def check(self, image):
        boot.check_fit(self.tool, "fdtget", image, self.root / "trusted.dtb")

    def test_valid_image(self):
        self.check(self.root / "trusted.itb")

    def test_wrong_signer_rejected(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.check(self.root / "untrusted.itb")

    def tamper(self, payload):
        data = bytearray((self.root / "trusted.itb").read_bytes())
        location = data.find(payload)
        self.assertGreaterEqual(location, 0)
        data[location] ^= 1
        image = self.root / "tampered.itb"
        image.write_bytes(data)
        with self.assertRaises(subprocess.CalledProcessError):
            self.check(image)

    def test_modified_kernel_rejected(self):
        self.tamper((self.root / "zImage").read_bytes())

    def test_modified_rootfs_rejected(self):
        self.tamper((self.root / "rootfs.cpio.gz").read_bytes())

    def test_modified_device_tree_rejected(self):
        self.tamper((self.root / "zybo-z7.dtb").read_bytes())

    def test_unsigned_image_rejected(self):
        boot.run(self.mkimage, "-f", "system.its", "unsigned.itb", cwd=self.root)
        with self.assertRaises(subprocess.CalledProcessError):
            self.check(self.root / "unsigned.itb")

    def test_signed_recovery_with_wrong_load_address_rejected(self):
        (self.root / "wrong-address.its").write_text(boot.fit_source().replace("0x02000000", "0x3f000000"))
        boot.run(self.mkimage, "-f", "wrong-address.its", "-k", "trusted", "wrong-address.itb", cwd=self.root)
        with self.assertRaisesRegex(ValueError, "kernel load"):
            self.check(self.root / "wrong-address.itb")


if __name__ == "__main__":
    unittest.main()
