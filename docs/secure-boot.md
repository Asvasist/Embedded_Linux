# Zybo Z7-20: startup checks and authenticated boot

This repository has two build profiles. `zybo_z7_defconfig` keeps the original
SPL/extlinux/ext4 boot and adds Linux startup diagnostics. It does **not** enforce
authenticity. `zybo_z7_secure_defconfig` uses an AMD FSBL, signed U-Boot and a
signed FIT containing Linux, its device tree and the **entire** gzip-compressed
cpio root filesystem. Select the latter for this extension.

Hardware enforcement begins only after the board's trust anchor and RSA-enable
eFUSEs are correctly provisioned. Producing a signed BOOT.BIN does not provision
the board. Neither the build nor the Linux checker claims that eFUSEs are set.
No code in this repository writes eFUSEs.

## Implemented sequence

1. Zynq Boot ROM loads FSBL. A provisioned device authenticates it using the
   on-chip trust anchor. An unprovisioned device is only a development setup.
2. The board-specific FSBL initializes clocks, MIO and DDR and authenticates
   the U-Boot partition. The FSBL must be built with `RSA_SUPPORT`.
3. U-Boot requires a mandatory FIT configuration key in its own control DT.
   It tests 1 MiB of unused DDR at 0x08000000 with caches disabled, initializes
   SD, bounds image reads to 64 MiB, and authenticates `system.itb#conf-1`.
4. On a load, signature or pre-handoff boot failure, U-Boot tries
   `recovery.itb#conf-1` under the same required key. If both fail it stops and
   prints a UART error. There is no U-Boot prompt or unsigned alternate boot.
5. Linux unpacks the authenticated initial filesystem into RAM and runs
   Buildroot's `/init` to mount `devtmpfs` before starting `/sbin/init`. Its command
   line is compiled into the authenticated kernel. No external `extlinux.conf`,
   `boot.scr`, saved environment or separate SD root filesystem is used.
6. `S15zybo-health` loads the button driver and checks available RAM, a bounded
   4 MiB userspace allocation, the button character device, LED interface and
   XADC reading. Required failures block application startup.
7. If a bitstream is present inside the signed filesystem, `S20fpga` loads it
   and checks for `operating` state. Failure removes application readiness.
8. `S90zybo-monitor` starts only with `/run/zybo/ready` and checks that the
   launched process survives its first second. This is a liveness check,
   not a service protocol handshake or ongoing watchdog.

The report is printed on UART and stored at `/run/zybo/health.txt`. Results are
PASS, FAIL, SKIPPED or NOT_TESTED. It does not assert physical button/LED,
Ethernet traffic, HDMI, audio, USB or PMOD operation without suitable operator
actions, devices or fixtures. FPGA `operating` verifies programming status,
not the function of a custom logic design. Early DDR initialization failure
may occur before UART or U-Boot diagnostics can run.

## Inputs needed after creating the XSA

Use a Linux Buildroot host with its normal prerequisites, Python 3.11+ and OpenSSL.
Windows/MSYS can run the portable unit tests but is not a supported Buildroot
host. Keep the existing build directory separate from the secure build.

In Vivado/Vitis 2023.2, export the **actual Zybo Z7-20** hardware configuration
as an XSA and create the Zynq FSBL application for `ps7_cortexa9_0`. Configure
the Z7-20 DDR and MIO correctly. Enable `RSA_SUPPORT` in the FSBL compile
definitions and rebuild. Retain the ELF symbol table. The packager rejects
non-ARM ELFs and ELFs without `AuthenticatePartition`, but that check cannot
prove that the BSP matches your hardware. Validate its initialization on the
board before provisioning. The monitor itself does not need PL logic.

Create private development keys **outside this repository**:

```sh
python3 tools/secure_boot.py keygen --directory "$HOME/zybo-signing"
```

This creates a FIT key/certificate and separate Zynq primary/secondary RSA
keys. Existing directories are rejected to prevent accidental key replacement.
The key directory uses owner-only permissions on Linux. Back it up securely;
production signing keys should be managed separately. Private material is
never copied to the root filesystem or SD image.

From a Buildroot 2024.02.9 checkout:

```sh
export ZYBO_SIGNING_DIR="$HOME/zybo-signing"
export ZYBO_FSBL="/absolute/path/to/zybo-z7-20/fsbl.elf"
# Optional: override the Bootgen supplied by Buildroot.
# export ZYBO_BOOTGEN="/path/to/bootgen"

make BR2_EXTERNAL=/absolute/path/to/Embedded_Linux \
     O=/absolute/path/to/out-secure zybo_z7_secure_defconfig
make O=/absolute/path/to/out-secure
```

Use this repository's root as `BR2_EXTERNAL`.
The secure profile builds Bootgen, OpenSSL-backed U-Boot signing/verification
tools and DTC. The post-image step requires the FSBL and keys; it never silently
falls back to an unsigned image. The kernel and rootfs can be compiled before
you have the FSBL, but final image generation will stop with a missing-input
message. Resume `make` with the variables above after the FSBL is ready.

Artifacts in `out-secure/images/`:

| File | Purpose |
|---|---|
| `boot.bin` | RSA-authenticated FSBL and U-Boot, packed by Bootgen |
| `system.itb` | Signed configuration covering kernel, DT and complete rootfs |
| `recovery.itb` | Verified fallback FIT under the same key |
| `sdcard.img` | FAT boot partition containing these three files |
| `u-boot-keyed.dtb` | Public verification DT for independent host checks |
| `secure-manifest.json` | Artifact hashes, FSBL hash and provisioning caveat |

U-Boot's control DT is modified to embed the **public** FIT key, then combined
with its code and converted back to ELF before Bootgen signs it. The old
unkeyed U-Boot ELF is never used as the secure payload. Both FITs are checked
with `fit_check_sign` before publishing final artifacts. The packager also
audits the resolved kernel configuration for initramfs and `/init` support,
checks recovery-image architecture and load addresses, and invalidates stale
or partially published outputs when packaging fails.

The initial recovery FIT is a duplicate of the factory image, useful if one
file is corrupted. To use a distinct previously validated release, store it
outside the output directory and export `ZYBO_RECOVERY_FIT=/path/recovery.itb`.
It must use `conf-1`, the same kernel/fdt/ramdisk node names and a currently
trusted key. It is verified, not silently re-signed. Recovery does not repair
bad RAM, a corrupt `boot.bin`, an unreadable SD partition, or a Linux hang
after control has left U-Boot. Those need additional boot-media redundancy or
watchdog/boot-attempt state design.

## Development versus provisioned hardware

First test the signed images on an **unprovisioned** board. This validates the
software path but does not establish hardware-enforced authenticity: someone
could replace the first-stage loader and its keys.

Only after validating normal boot, negative tests and recovery:

1. Review AMD UG585's RSA/eFUSE provisioning procedure for the actual silicon
   revision. Use AMD's supported provisioning tools and verify key-hash inputs.
2. Independently verify that the intended primary public-key hash matches the
   production signing key and that the matching authenticated images boot.
3. Plan key backup, signed recovery and debug/boot-mode access policy.
4. Program and read back the appropriate trust-anchor/authentication eFUSEs
   as a separate, deliberate hardware operation. These settings are permanent.
5. Confirm that modified FSBL/U-Boot images are rejected by the board, and
   that modified FIT payloads cannot boot through any exposed path.

Do not infer hardware enforcement from the Linux status report or from a
successful signing command. Do not fuse a board on the strength of host tests.

## Scope and resource costs

- The secure profile disables root login and does not include Dropbear. It
  prints diagnostics on UART. A developer login must be deliberately enabled
  with a private password in the signed Buildroot configuration if needed.
- The signed initramfs protects the initial application and configuration
  bytes. It is writable in RAM after boot; changes disappear on reset. It is
  not runtime attestation, immutable RAM, or protection against compromised
  privileged software. No separately mounted storage is trusted for startup.
- The complete rootfs consumes RAM. Packaging bounds its uncompressed archive
  to 128 MiB and the entire FIT to 64 MiB; those are input limits, not runtime
  memory quotas. Measure actual peak memory before adding large applications.
  A verified disk filesystem is a future alternative for larger deployments.
- Signatures do not encrypt software, prevent rollback to older valid images,
  or fix vulnerabilities. Encryption, anti-rollback, ongoing watchdog recovery
  and per-service memory supervision are not implemented in this change.
- The pinned 2023.2 bootloader/kernel are retained for compatibility. Review
  security fixes and supported releases before treating this as a production
  security boundary.

## Validation

Portable tests:

```sh
python3 -m unittest discover -s tests -v
mkdir -p output
cc -std=c11 -Wall -Wextra -Werror -Isrc/zybo-health \
   tests/health_test.c src/zybo-health/health.c -o output/health-test
output/health-test
make -C src/zybo-health
```

On Linux, after building U-Boot, run real signature tests:

```sh
export ZYBO_UBOOT_TOOLS=/path/to/u-boot-build/tools
python3 -m unittest discover -s tests -v
```

These use disposable keys and require `openssl`, `dtc`, `fdtget`. Without these
tools the cryptographic tests are explicitly skipped. They test a valid image,
wrong signer, unsigned image, modified kernel, DT and rootfs, and a signed
recovery image with an incompatible load address. CI sets
`ZYBO_REQUIRE_FIT_TESTS=1` so missing tools fail the job instead of skipping
these tests. The workflow also builds the monitor and runs Linux regression
tests for invalid arguments and button-device EOF handling. A successful CI
run is required in addition to the portable tests before board acceptance.

Board acceptance tests still required: power-on UART trace; signed primary;
corrupt primary with valid recovery; both FITs invalid; missing key; unsigned
BOOT.BIN on a provisioned device; SD removal; required driver/sensor failure;
physical button/LED checks; boot time and peak RAM. Failure cases must never
start the monitor or expose a U-Boot prompt. Keep unsigned test builds and
production keys separate.

References: [AMD Zynq RSA authentication](https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/RSA-Authentication),
[AMD FSBL secure-boot support](https://docs.amd.com/r/en-US/ug821-zynq-7000-swdev/Secure-Boot-Support),
[U-Boot FIT signatures](https://docs.u-boot.org/en/latest/usage/fit/signature.html).
