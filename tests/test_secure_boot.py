"""Host policy tests. Cryptographic integration tests additionally need U-Boot tools."""
import gzip
from argparse import Namespace
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("secure_boot", ROOT / "tools/secure_boot.py")
secure_boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(secure_boot)


class SigningPolicyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.images = self.root / "images"
        (self.images / "secure-inputs").mkdir(parents=True)
        self.config = self.images / "secure-inputs/uboot.config"
        self.config.write_text((ROOT / "board/zybo-z7/secure/uboot.fragment").read_text())
        self.linux = self.images / "secure-inputs/linux.config"
        self.linux.write_text((ROOT / "board/zybo-z7/linux.fragment").read_text() + "\n" +
                              (ROOT / "board/zybo-z7/secure/linux.fragment").read_text())
        self.keys = self.root / "keys"
        self.keys.mkdir()
        for name in secure_boot.KEY_NAMES:
            (self.keys / name).write_text("fixture-only; not cryptographic key material")
            (self.keys / name).chmod(0o600)
        self.fsbl = self.root / "fsbl.elf"
        elf = bytearray(52)
        elf[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<H", elf, 18, 40)
        self.fsbl.write_bytes(elf)
        (self.images / "zImage").write_bytes(b"kernel fixture")
        (self.images / "zybo-z7.dtb").write_bytes(b"dtb fixture")
        (self.images / "rootfs.cpio.gz").write_bytes(gzip.compress(b"070701fixture"))

    def test_baseline_policy_accepts(self):
        secure_boot.validate_inputs(self.images, self.keys, self.fsbl)

    def test_persistent_environment_rejected(self):
        with self.config.open("a") as stream:
            stream.write("\nCONFIG_ENV_IS_IN_FAT=y\n")
        with self.assertRaisesRegex(ValueError, "Persistent environment"):
            secure_boot.audit_config(self.config)

    def test_missing_enforcement_rejected(self):
        self.config.write_text(self.config.read_text().replace("CONFIG_ZYBO_VERIFIED_BOOT=y", ""))
        with self.assertRaisesRegex(ValueError, "ZYBO_VERIFIED_BOOT"):
            secure_boot.audit_config(self.config)

    def test_initramfs_cannot_bypass_devtmpfs_setup(self):
        self.linux.write_text(self.linux.read_text().replace("rdinit=/init", "rdinit=/sbin/init"))
        with self.assertRaisesRegex(ValueError, "Buildroot /init"):
            secure_boot.validate_inputs(self.images, self.keys, self.fsbl)

    def test_missing_devtmpfs_rejected(self):
        self.linux.write_text(self.linux.read_text().replace("CONFIG_DEVTMPFS=y", ""))
        with self.assertRaisesRegex(ValueError, "CONFIG_DEVTMPFS"):
            secure_boot.validate_inputs(self.images, self.keys, self.fsbl)

    def test_mismatched_target_fit_limit_rejected(self):
        self.config.write_text(self.config.read_text().replace("0x04000000\nCONFIG_RSA", "0x08000000\nCONFIG_RSA"))
        with self.assertRaisesRegex(ValueError, "FIT limit"):
            secure_boot.audit_config(self.config)

    def test_unsigned_legacy_images_rejected(self):
        with self.config.open("a") as stream:
            stream.write("\nCONFIG_LEGACY_IMAGE_FORMAT=y\n")
        with self.assertRaisesRegex(ValueError, "LEGACY_IMAGE_FORMAT"):
            secure_boot.audit_config(self.config)

    def test_missing_private_key_rejected(self):
        (self.keys / "zybo.key").unlink()
        with self.assertRaisesRegex(ValueError, "Missing signing material"):
            secure_boot.validate_inputs(self.images, self.keys, self.fsbl)

    def test_wrong_fsbl_architecture_rejected(self):
        elf = bytearray(self.fsbl.read_bytes())
        struct.pack_into("<H", elf, 18, 183)  # AArch64, not Zynq-7000
        self.fsbl.write_bytes(elf)
        with self.assertRaisesRegex(ValueError, "32-bit ARM"):
            secure_boot.validate_inputs(self.images, self.keys, self.fsbl)

    def test_wrong_rootfs_format_rejected(self):
        (self.images / "rootfs.cpio.gz").write_bytes(gzip.compress(b"ext4 is not cpio"))
        with self.assertRaisesRegex(ValueError, "newc"):
            secure_boot.validate_inputs(self.images, self.keys, self.fsbl)

    def test_expansion_limit_rejected(self):
        old = secure_boot.ROOTFS_LIMIT
        secure_boot.ROOTFS_LIMIT = 20
        self.addCleanup(setattr, secure_boot, "ROOTFS_LIMIT", old)
        (self.images / "rootfs.cpio.gz").write_bytes(gzip.compress(b"070701" + b"x" * 100))
        with self.assertRaisesRegex(ValueError, "Unpacked rootfs"):
            secure_boot.validate_inputs(self.images, self.keys, self.fsbl)

    def test_keygen_never_overwrites_existing_directory(self):
        marker = self.keys / "zybo.key"
        original = marker.read_bytes()
        with self.assertRaises(FileExistsError):
            secure_boot.keygen(Namespace(directory=self.keys, openssl="not-invoked"))
        self.assertEqual(marker.read_bytes(), original)

    def test_failed_build_invalidates_previous_deployable_outputs(self):
        old_image = self.images / "sdcard.img"
        old_image.write_bytes(b"previous image must not look like a new release")
        (self.keys / "zybo.key").unlink()
        with self.assertRaisesRegex(ValueError, "Missing signing material"):
            secure_boot.assemble(Namespace(images=self.images, keys=self.keys, fsbl=self.fsbl, recovery=None))
        self.assertFalse(old_image.exists())

    def test_interrupted_publication_removes_partial_release(self):
        def interrupted(*args):
            (self.images / "system.itb").write_bytes(b"partial release")
            raise KeyboardInterrupt
        with patch.object(secure_boot, "assemble_checked", side_effect=interrupted):
            with self.assertRaises(KeyboardInterrupt):
                secure_boot.assemble(Namespace(images=self.images, keys=self.keys, fsbl=self.fsbl, recovery=None))
        self.assertFalse((self.images / "system.itb").exists())

    def test_truncated_fit_rejected_before_external_tools(self):
        fit = self.images / "truncated.itb"
        fit.write_bytes(struct.pack(">II", 0xd00dfeed, 4096))
        with self.assertRaisesRegex(ValueError, "complete embedded-data FIT"):
            secure_boot.check_fit("not-invoked", "not-invoked", fit, self.images / "key.dtb")


if __name__ == "__main__":
    unittest.main()
