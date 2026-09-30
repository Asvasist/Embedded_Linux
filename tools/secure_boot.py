#!/usr/bin/env python3
"""Build the Zybo signed FIT + RSA BOOT.BIN. Never programs hardware/eFUSEs."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

FIT_LIMIT = 64 * 1024 * 1024
ROOTFS_LIMIT = 128 * 1024 * 1024
TEXT_BASE = 0x04000000
KEY_NAMES = ("zybo.key", "zybo.crt", "boot-primary.pem", "boot-secondary.pem")
OUTPUT_NAMES = ("sdcard.img", "boot.vfat", "boot.bin", "system.itb", "recovery.itb",
                "u-boot-keyed.dtb", "secure-manifest.json")


def run(*args, cwd=None, capture=False):
    return subprocess.run([str(a) for a in args], cwd=cwd, check=True,
                          text=True, stdout=subprocess.PIPE if capture else None).stdout


def config_values(path):
    values = {}
    for line in Path(path).read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


def audit_config(path):
    cfg = config_values(path)
    for symbol in ("ZYBO_VERIFIED_BOOT", "FIT", "FIT_SIGNATURE", "FIT_FULL_CHECK",
                   "RSA", "RSA_VERIFY", "OF_SEPARATE", "ENV_IS_NOWHERE",
                   "CMD_BOOTM", "CMD_FAT", "CMD_MMC", "CMD_MEMTEST"):
        if cfg.get("CONFIG_" + symbol) != "y":
            raise ValueError(f"U-Boot is missing required CONFIG_{symbol}=y")
    for symbol in ("SPL", "OF_BOARD", "LEGACY_IMAGE_FORMAT", "USE_PREBOOT",
                   "DISTRO_DEFAULTS", "NETCONSOLE", "UPDATE_TFTP"):
        if cfg.get("CONFIG_" + symbol) == "y":
            raise ValueError(f"Forbidden U-Boot setting CONFIG_{symbol}")
    for symbol, value in cfg.items():
        if symbol.startswith("CONFIG_ENV_IS_IN_") and value == "y":
            raise ValueError(f"Persistent environment forbidden: {symbol}")
    if int(cfg.get("CONFIG_TEXT_BASE", "0"), 0) != TEXT_BASE:
        raise ValueError("U-Boot text base must be 0x04000000")
    if int(cfg.get("CONFIG_FIT_SIGNATURE_MAX_SIZE", "0"), 0) != FIT_LIMIT:
        raise ValueError("U-Boot FIT limit must match the 64 MiB load region")


def audit_linux_config(path):
    cfg = config_values(path)
    for symbol in ("BLK_DEV_INITRD", "RD_GZIP", "DEVTMPFS", "CMDLINE_FORCE"):
        if cfg.get("CONFIG_" + symbol) != "y":
            raise ValueError(f"Linux is missing required CONFIG_{symbol}=y")
    command_line = cfg.get("CONFIG_CMDLINE", "").strip('"').split()
    if [word for word in command_line if word.startswith("rdinit=")] != ["rdinit=/init"]:
        raise ValueError("Linux must enter Buildroot /init to mount devtmpfs")


def invalidate_images(images):
    images = Path(images).resolve()
    if not images.is_dir() or images == images.parent:
        raise ValueError("Expected a build images directory")
    for name in OUTPUT_NAMES:
        path = images / name
        if path.exists() or path.is_symlink():
            path.unlink()


def validate_inputs(images, keys, fsbl):
    images, keys, fsbl = Path(images), Path(keys), Path(fsbl)
    audit_config(images / "secure-inputs/uboot.config")
    audit_linux_config(images / "secure-inputs/linux.config")
    for name in KEY_NAMES:
        path = keys / name
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Missing signing material: {name}")
        if os.name == "posix" and name != "zybo.crt" and path.stat().st_mode & 0o077:
            raise ValueError(f"Private key must be owner-only (chmod 600): {name}")
    with fsbl.open("rb") as stream:
        header = stream.read(52)
    if len(header) != 52 or header[:7] != b"\x7fELF\x01\x01\x01" or struct.unpack_from("<H", header, 18)[0] != 40:
        raise ValueError("FSBL must be a little-endian 32-bit ARM ELF")
    for name, limit in (("zImage", 24 * 1024 * 1024), ("zybo-z7.dtb", 1024 * 1024),
                        ("rootfs.cpio.gz", 48 * 1024 * 1024)):
        if not 0 < (images / name).stat().st_size <= limit:
            raise ValueError(f"{name} exceeds the fixed boot memory layout")
    # Limit expansion as well as compressed size. Read in bounded chunks.
    count = 0
    with gzip.open(images / "rootfs.cpio.gz", "rb") as stream:
        prefix = stream.read(6)
        if prefix not in (b"070701", b"070702"):
            raise ValueError("Expected a newc initramfs")
        count += len(prefix)
        while chunk := stream.read(1024 * 1024):
            count += len(chunk)
            if count > ROOTFS_LIMIT:
                raise ValueError("Unpacked rootfs exceeds 128 MiB policy")


def fit_source():
    return '''/dts-v1/;
/ {
    description = "Zybo Z7-20 authenticated Linux system";
    #address-cells = <1>;
    images {
        kernel-1 {
            description = "Linux";
            data = /incbin/("zImage");
            type = "kernel"; arch = "arm"; os = "linux";
            compression = "none"; load = <0x02000000>; entry = <0x02000000>;
            hash-1 { algo = "sha256"; };
        };
        fdt-1 {
            description = "Zybo Z7 device tree";
            data = /incbin/("zybo-z7.dtb");
            type = "flat_dt"; arch = "arm"; compression = "none";
            hash-1 { algo = "sha256"; };
        };
        ramdisk-1 {
            description = "Complete authenticated root filesystem";
            data = /incbin/("rootfs.cpio.gz");
            type = "ramdisk"; arch = "arm"; os = "linux";
            compression = "none";
            hash-1 { algo = "sha256"; };
        };
    };
    configurations {
        default = "conf-1";
        conf-1 {
            kernel = "kernel-1";
            fdt = "fdt-1";
            ramdisk = "ramdisk-1";
            signature-1 {
                algo = "sha256,rsa2048";
                key-name-hint = "zybo";
                sign-images = "kernel", "fdt", "ramdisk";
            };
        };
    };
};
'''


def keygen(args):
    directory = args.directory.resolve()
    # Refuse partial replacement of existing key sets.
    directory.mkdir(mode=0o700, parents=True, exist_ok=False)
    previous = os.umask(0o077)
    try:
        for name in ("zybo.key", "boot-primary.pem", "boot-secondary.pem"):
            run(args.openssl, "genrsa", "-out", directory / name, "2048")
        run(args.openssl, "req", "-batch", "-new", "-x509", "-sha256", "-days", "3650",
            "-key", directory / "zybo.key", "-out", directory / "zybo.crt",
            "-subj", "/CN=Zybo FIT signing/")
    finally:
        os.umask(previous)
    print(f"Created private signing directory: {directory}")
    print("Back it up securely. No device or eFUSE was programmed.")


def sha256(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check_fit(tool, fdtget, image, control):
    if not 0 < image.stat().st_size <= FIT_LIMIT:
        raise ValueError("FIT exceeds 64 MiB policy")
    with image.open("rb") as stream:
        header = stream.read(8)
    if len(header) != 8 or struct.unpack(">II", header) != (0xd00dfeed, image.stat().st_size):
        raise ValueError("Expected a complete embedded-data FIT, matching the target boot policy")
    required = run(fdtget, "-t", "s", control, "/signature/key-zybo", "required", capture=True).strip()
    if required != "conf":
        raise ValueError("U-Boot public key is not mandatory for configurations")
    for prop, expected in (("kernel", "kernel-1"), ("fdt", "fdt-1"), ("ramdisk", "ramdisk-1")):
        actual = run(fdtget, "-t", "s", image, "/configurations/conf-1", prop, capture=True).strip()
        if actual != expected:
            raise ValueError(f"Unexpected FIT {prop} selection")
    run(tool, "-f", image, "-k", control, "-c", "conf-1")
    # A correctly signed recovery image must also fit this board's boot ABI.
    for node, image_type in (("kernel-1", "kernel"), ("fdt-1", "flat_dt"), ("ramdisk-1", "ramdisk")):
        path = "/images/" + node
        for prop, expected in (("type", image_type), ("arch", "arm"), ("compression", "none")):
            actual = run(fdtget, "-t", "s", image, path, prop, capture=True).strip()
            if actual != expected:
                raise ValueError(f"Incompatible FIT {node} {prop}: {actual}")
        properties = run(fdtget, "-p", image, path, capture=True).split()
        if "data" not in properties or any(prop in properties for prop in ("data-offset", "data-position")):
            raise ValueError(f"FIT {node} must embed its payload")
    for prop in ("load", "entry"):
        value = run(fdtget, "-t", "x", image, "/images/kernel-1", prop, capture=True).strip()
        if value != "2000000":
            raise ValueError(f"FIT kernel {prop} must be 0x02000000")


def assemble(args):
    images, keys, fsbl = args.images.resolve(), args.keys.resolve(), args.fsbl.resolve()
    if args.recovery and args.recovery.resolve().is_relative_to(images):
        raise ValueError("Keep the known-good recovery image outside the output images directory")
    invalidate_images(images)
    try:
        assemble_checked(args, images, keys, fsbl)
    except BaseException:
        # Also remove partially published outputs on error or interruption.
        invalidate_images(images)
        raise


def assemble_checked(args, images, keys, fsbl):
    validate_inputs(images, keys, fsbl)
    inputs = images / "secure-inputs"
    symbols = run(inputs / "arm-nm", "--defined-only", fsbl, capture=True)
    if not any(line.split()[-1:] == ["AuthenticatePartition"] for line in symbols.splitlines()):
        raise ValueError("FSBL must retain symbols and contain AuthenticatePartition (build with RSA_SUPPORT)")
    mkimage, checker = inputs / "mkimage", inputs / "fit_check_sign"
    with tempfile.TemporaryDirectory(prefix="zybo-sign-", dir=images) as directory:
        work = Path(directory)
        for name in ("zImage", "zybo-z7.dtb", "rootfs.cpio.gz"):
            shutil.copyfile(images / name, work / name)
        shutil.copyfile(inputs / "u-boot.dtb", work / "control.dtb")
        (work / "system.its").write_text(fit_source())
        # mkimage embeds ONLY the public key into the U-Boot control DT.
        run(mkimage, "-f", "system.its", "-k", keys, "-K", "control.dtb", "-r", "system.itb", cwd=work)
        check_fit(checker, args.fdtget, work / "system.itb", work / "control.dtb")
        # A distinct known-good image can be supplied; the first factory build
        # duplicates its authenticated system as a fallback for file corruption.
        shutil.copyfile(args.recovery.resolve() if args.recovery else work / "system.itb", work / "recovery.itb")
        check_fit(checker, args.fdtget, work / "recovery.itb", work / "control.dtb")

        # Rebuild the executable *after* public-key injection. Signing the old
        # u-boot.elf would silently omit the required verification key.
        (work / "u-boot.bin").write_bytes((inputs / "u-boot-nodtb.bin").read_bytes() +
                                         (work / "control.dtb").read_bytes())
        run(inputs / "arm-objcopy", "-I", "binary", "-O", "elf32-littlearm", "-B", "arm",
            "--rename-section", ".data=.text,alloc,load,readonly,code,contents",
            "u-boot.bin", "u-boot.o", cwd=work)
        run(inputs / "arm-ld", "-EL", "-Ttext=0x04000000", "--entry=0x04000000",
            "-o", "u-boot-keyed.elf", "u-boot.o", cwd=work)
        # Copy only public inputs; private-key paths stay in this temporary BIF.
        shutil.copyfile(fsbl, work / "fsbl.elf")
        primary, secondary = keys / "boot-primary.pem", keys / "boot-secondary.pem"
        for path in (primary, secondary):
            if any(char in str(path) for char in ('"', '\n', '\r')):
                raise ValueError("Unsupported signing directory characters")
        bif = ('the_ROM_image:\n{\n'
               f'    [pskfile] "{primary.as_posix()}"\n'
               f'    [sskfile] "{secondary.as_posix()}"\n'
               '    [bootloader, authentication=rsa] fsbl.elf\n'
               '    [authentication=rsa] u-boot-keyed.elf\n}\n')
        (work / "boot.bif").write_text(bif)
        run(args.bootgen, "-arch", "zynq", "-image", "boot.bif", "-o", "boot.bin", "-w", "on", cwd=work)
        if not (work / "boot.bin").stat().st_size:
            raise ValueError("Bootgen produced an empty image")
        manifest = {
            "profile": "zybo-z7-20-rsa-fit-initramfs",
            "hardware_authentication": "requires separately verified eFUSE provisioning",
            "fsbl_requirement": "Zybo Z7-20 hardware initialization and RSA_SUPPORT enabled",
            "fsbl_sha256": sha256(fsbl),
            "recovery": "supplied signed image" if args.recovery else "factory duplicate",
            "sha256": {name: sha256(work / name) for name in ("boot.bin", "system.itb", "recovery.itb")},
        }
        for name in ("boot.bin", "system.itb", "recovery.itb"):
            os.replace(work / name, images / name)
        # Public DT is useful for independent verification; never on the SD boot path.
        os.replace(work / "control.dtb", images / "u-boot-keyed.dtb")
        (work / "secure-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        os.replace(work / "secure-manifest.json", images / "secure-manifest.json")
    print("Signed images verified and packaged. Hardware enforcement is NOT asserted by this build.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    generate = commands.add_parser("keygen", help="create a new private RSA key set")
    generate.add_argument("--directory", required=True, type=Path)
    generate.add_argument("--openssl", default="openssl")
    generate.set_defaults(func=keygen)
    invalidate = commands.add_parser("invalidate", help="remove stale deployable outputs before a build attempt")
    invalidate.add_argument("--images", type=Path, required=True)
    invalidate.set_defaults(func=lambda args: invalidate_images(args.images))
    build = commands.add_parser("assemble", help="sign and verify build outputs; no hardware access")
    for name in ("images", "keys", "fsbl"):
        build.add_argument("--" + name, type=Path, required=True)
    build.add_argument("--bootgen", required=True)
    build.add_argument("--fdtget", default="fdtget")
    build.add_argument("--recovery", type=Path)
    build.set_defaults(func=assemble)
    args = parser.parse_args()
    try:
        args.func(args)
    except (OSError, EOFError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"secure-boot: {error}\n")


if __name__ == "__main__":
    main()
