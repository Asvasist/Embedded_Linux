# Embedded Linux on the Zybo Z7

A Buildroot external tree for the **Digilent Zybo Z7-20**, combining a custom
button driver, board monitoring, automatic startup diagnostics, and an
authenticated boot build profile.

The monitoring application runs on the Zynq processing system (PS): it uses
MIO buttons, the MIO LED, and the on-chip XADC. A programmable-logic (PL)
bitstream is optional.

> **Project status:** source implementation with host regression tests.
> A complete secure image still requires a board-specific FSBL generated from
> your XSA. Full Buildroot builds, cryptographic integration tests and board
> acceptance tests must pass before deployment. Hardware secure boot is not
> enabled by generating signed files; eFUSE provisioning is a separate step.

## Capabilities

| Component | Function |
|---|---|
| Buildroot integration | Builds the Linux image, custom packages and SD-card layout |
| `zybo_btn` kernel module | Interrupt-driven, debounced BTN4/BTN5 events through `/dev/zybo_btn` |
| `zybo-health` | Startup checks for memory, required device interfaces and XADC temperature |
| `zybo-monitor` | Button actions, LED control, temperature alerts and system status logging |
| Authenticated boot profile | RSA-signed FSBL/U-Boot and signed FIT configuration covering Linux, DT and initial filesystem |
| Recovery policy | Tries a signed recovery FIT when the primary image fails before kernel handoff |
| Validation | Portable policy/C tests, Linux daemon tests and FIT tampering tests |

The event queue and startup memory tests have fixed bounds. **Per-service
memory limits, a runtime supervisor and watchdog recovery are not implemented.**
They remain possible extensions.

## Build profiles

| | Development | Authenticated boot |
|---|---|---|
| Defconfig | `zybo_z7_defconfig` | `zybo_z7_secure_defconfig` |
| First-stage loader | U-Boot SPL | Board-specific AMD FSBL with `RSA_SUPPORT` |
| Linux boot | extlinux, `zImage`, DTB | Signed `system.itb` |
| Root filesystem | Writable SD ext4 partition | Complete signed initramfs, unpacked into RAM |
| Startup diagnostics | Enabled | Enabled |
| Login | `root` / `zybo`; Dropbear enabled | Root login disabled; no Dropbear |
| Additional inputs | None | FSBL ELF and private signing keys |
| Hardware trust anchor | None | Requires separately validated eFUSE provisioning |

Use the development credentials only on an isolated development network and
change them before enabling access from other systems. The authenticated
profile prints UART diagnostics; enabling an interactive developer login is
an explicit Buildroot configuration change.

## Boot architecture

```text
Development:
Boot ROM -> U-Boot SPL -> U-Boot/extlinux -> Linux + SD rootfs

Authenticated profile:
Boot ROM -> authenticated FSBL -> authenticated U-Boot
                                       |
                            DDR quick test + SD access
                                       |
                            verify signed system.itb
                              |                 |
                            valid            rejected
                              |                 |
                              |        verify signed recovery.itb
                              |          |                |
                              |        valid           rejected -> stop
                              v          v
                          Linux -> /init -> BusyBox init
                                       |
                           S15: required health checks
                                       |
                           S20: optional FPGA programming
                                       |
                           S90: start board monitor
```

The authenticated U-Boot path does not load an external boot script, saved
environment or extlinux configuration, and does not expose an interactive
U-Boot prompt after failure. Linux enters Buildroot's `/init` wrapper so that
`devtmpfs` is mounted before `/sbin/init` starts.

See [the secure-boot guide](docs/secure-boot.md) for signing, the trust chain,
memory layout, recovery boundaries and hardware provisioning prerequisites.

## Repository layout

```text
configs/                     Development and authenticated Buildroot profiles
board/zybo-z7/               Device tree, kernel fragment and SD-image scripts
board/zybo-z7/rootfs-overlay/ Board startup scripts and monitor defaults
board/zybo-z7/secure/         U-Boot policy, patch, configuration and packaging
package/                     Buildroot package definitions and init scripts
src/zybo-btn/                Linux platform driver and shared event ABI
src/zybo-health/             Startup diagnostics in userspace C
src/zybo-monitor/            Event-driven board daemon in userspace C
tools/secure_boot.py          Signing, verification and artifact packaging
tests/                       C, policy and integration regression tests
docs/secure-boot.md           Detailed authenticated-boot setup
.github/workflows/            Linux CI configuration
```

All build commands use this repository's root as `BR2_EXTERNAL`.

## Prerequisites

- Digilent Zybo Z7-20, microSD card, USB UART connection and suitable power.
- A Linux build host with the
  [Buildroot prerequisites](https://buildroot.org/downloads/manual/manual.html#requirement).
  Native Windows/MSYS is suitable for portable tests, not a full Buildroot build.
- For signing: Python 3.11+, OpenSSL and an FSBL matching the board's hardware
  export. Buildroot supplies DTC, Bootgen and U-Boot signing tools.

| Dependency | Pinned version |
|---|---|
| Buildroot | `2024.02.9` |
| Linux | Xilinx `xilinx-v2023.2`, 6.1 series |
| U-Boot | Xilinx `xilinx-v2023.2` |

These versions are retained for project compatibility, not presented as a
current production-security baseline. Review upstream fixes before deployment.
The original Zybo and other board variants have not been validated by this
extension.

## Build the development image

Run from this repository on Linux. Keep separate output directories for the
two profiles.

```sh
export PROJECT_DIR="$(pwd)"
git clone --depth 1 --branch 2024.02.9 \
    https://gitlab.com/buildroot.org/buildroot.git ../buildroot

make -C ../buildroot BR2_EXTERNAL="$PROJECT_DIR" \
    O="$PROJECT_DIR/../out-dev" zybo_z7_defconfig
make -C ../out-dev
```

The output directory contains `images/sdcard.img`, `boot.bin`, `u-boot.img`,
`zImage`, `zybo-z7.dtb` and `rootfs.ext4`.

To rebuild project packages and regenerate the image:

```sh
make -C ../out-dev zybo-btn-rebuild zybo-health-rebuild zybo-monitor-rebuild
make -C ../out-dev
```

## Build the authenticated image

First generate the Zybo Z7-20 XSA and build an FSBL with `RSA_SUPPORT` using
Vivado/Vitis. Preserve the ELF symbols. Detailed requirements are in the
[FSBL and signing instructions](docs/secure-boot.md#inputs-needed-after-creating-the-xsa).

Create signing keys outside the repository, then configure the secure build:

```sh
export PROJECT_DIR="$(pwd)"
python3 tools/secure_boot.py keygen --directory "$HOME/zybo-signing"

export ZYBO_SIGNING_DIR="$HOME/zybo-signing"
export ZYBO_FSBL="/absolute/path/to/zybo-z7-20/fsbl.elf"

make -C ../buildroot BR2_EXTERNAL="$PROJECT_DIR" \
    O="$PROJECT_DIR/../out-secure" zybo_z7_secure_defconfig
make -C ../out-secure
```

Without the FSBL, you can prepare the software components first:

```sh
make -C ../out-secure linux uboot zybo-btn zybo-health zybo-monitor
```

Final packaging deliberately fails if required inputs, resolved kernel/U-Boot
settings or image verification checks are invalid. Failed packaging removes
stale deployable outputs. Private keys are not included on the SD card.

| Secure artifact | Contents |
|---|---|
| `sdcard.img` | Deployable SD image, after a successful complete build |
| `boot.bin` | Signed FSBL and U-Boot containing the FIT public key |
| `system.itb` | Signed Linux kernel, device tree and complete initial filesystem |
| `recovery.itb` | Signed fallback; initially a duplicate of the factory image |
| `u-boot-keyed.dtb` | Public control DT for host verification |
| `secure-manifest.json` | Artifact hashes and build/provisioning notes |

To supply a different known-good recovery release, set `ZYBO_RECOVERY_FIT`
to a compatible signed FIT **outside the build output directory** before
running the final build.

## Boot and inspect

1. Write the chosen `images/sdcard.img` to the microSD card. Confirm the target
   disk carefully; flashing replaces its existing contents.
2. Select SD boot using JP5 and connect the PROG/UART USB port.
3. Open the serial console at **115200 baud, 8 data bits, no parity, 1 stop bit**.
4. Power on and inspect the boot and health-check output.

On the development image:

```sh
cat /run/zybo/health.txt
test -f /run/zybo/ready && echo "Startup checks passed"
tail -f /var/log/messages
```

`/run/zybo/ready` gates application startup. It is not continuous health
attestation. A failed required check leaves the monitor stopped. An installed
FPGA bitstream that fails programming also blocks the monitor.

| Result | Meaning |
|---|---|
| `PASS` | The stated check passed, within its reported scope |
| `FAIL` | A required check failed; application startup is blocked |
| `SKIPPED` | An optional component or check is not applicable |
| `NOT_TESTED` | Additional equipment, operator input or tests are needed |

Interface detection does not prove physical button/LED behavior or complete
Ethernet, USB, HDMI, audio or PMOD operation. The secure U-Boot DDR test covers
a 1 MiB scratch region; the userspace RAM test covers its own 4 MiB allocation.

## Application and driver interfaces

- **BTN4:** cycle LD4 through heartbeat, on and off.
- **BTN5:** log temperature, uptime, load, free memory and observed presses.
- **Temperature alert:** fast LED blink above the configured threshold; clear
  after temperature falls 5 C below the threshold.
- **Configuration:** `/etc/default/zybo-monitor`, default `-i 60 -w 70`.
- **Options:** `-i` accepts 1–86400 seconds; `-w` accepts finite values from
  -40 to 125 C. `-v` also logs to stderr.

The daemon uses `poll()`, `timerfd` and `signalfd`. It sleeps between events.
Device disconnects disable button handling without a busy loop; fatal
timer/signal descriptor failures return a nonzero exit status.

The driver maintains a 32-event FIFO and emits this shared ABI:

```c
struct zybo_btn_event {
    __u64 timestamp_ns; /* monotonic timestamp after debounce */
    __u32 button;       /* 0: BTN4; 1: BTN5 */
    __u32 pressed;      /* 1: pressed; 0: released */
};
```

Reads block until an event is available, or use `O_NONBLOCK`/`poll()`.
Multiple readers share the queue; events are consumed, not broadcast.
Platform-device sysfs attributes expose `debounce_ms` (1–1000), `presses` and
`dropped`. The platform device is normally named `buttons`.

For optional PL programming, include a Bootgen-converted binary at
`/lib/firmware/zybo_top.bit.bin`. `S20fpga` uses the pinned Xilinx FPGA-manager
interface and checks its `operating` state. In the secure profile, include
the bitstream before signing the complete root filesystem.

## Validation and CI

Portable tests, from the repository root:

```sh
python3 -m unittest discover -s tests -v
mkdir -p output
cc -std=c11 -Wall -Wextra -Werror -Isrc/zybo-health \
    tests/health_test.c src/zybo-health/health.c -o output/health-test
output/health-test
cc -std=c11 -Wall -Wextra -Werror -Isrc/zybo-monitor \
    tests/options_test.c src/zybo-monitor/options.c -o output/options-test
output/options-test
```

On Linux, build the daemon and run integration tests with the pinned U-Boot tools:

```sh
make -C src/zybo-health
make -C src/zybo-monitor
export ZYBO_UBOOT_TOOLS=/absolute/path/to/u-boot-build/tools
export ZYBO_MONITOR_BINARY="$PWD/src/zybo-monitor/zybo-monitor"
ZYBO_REQUIRE_FIT_TESTS=1 python3 -m unittest discover -s tests -v
```

FIT tests require `openssl`, `dtc` and `fdtget`. They use disposable keys and
check valid signatures, wrong signers, unsigned images and modified payloads.
Tests requiring unavailable Linux tools are explicitly skipped in portable
runs; CI requires FIT tools and fails if they are missing.

The [CI workflow](.github/workflows/boot-checks.yml) builds the userspace programs
and patched U-Boot, audits its resolved configuration and runs regression
tests. It does not replace an end-to-end Buildroot build or tests on the board.
See the [board acceptance checklist](docs/secure-boot.md#validation).

## Troubleshooting

| Symptom | Check |
|---|---|
| Signing stops before producing an image | Key/FSBL paths, private-key permissions and the reported configuration error |
| `required FIT verification key missing` | Use the packaged `boot.bin`, not an earlier unkeyed U-Boot binary |
| Both FITs rejected | Signing key, image integrity, `conf-1` layout and recovery compatibility |
| Monitor reports `BLOCKED` | UART output, `/run/zybo/health.txt` and FPGA programming result |
| `/dev/zybo_btn` absent | Module installation, device-tree GPIO node and `/dev` mount |
| No interactive login in secure profile | Expected default; diagnostics are printed on UART |

## Security boundaries and future work

Authentication protects the initial software bytes; it does not encrypt them,
prevent rollback to an older valid image, or guarantee safety after a runtime
compromise. The initramfs is writable in RAM and changes disappear on reset.
No eFUSEs are programmed by this repository's tools.

Future work includes measured service memory budgets and cgroups, ongoing
service supervision, watchdog-assisted recovery, verified disk storage for
larger applications, and peripheral-specific functional tests.

## Licensing

The button driver and U-Boot integration carry GPL-2.0-family SPDX identifiers;
the event ABI header includes the Linux syscall exception. Userspace C carries
MIT identifiers. Refer to individual files and upstream components for their
applicable licenses.
