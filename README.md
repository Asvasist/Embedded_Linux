# Embedded Linux on the Zybo Z7

Buildroot based Linux for the Digilent Zybo Z7 (Zynq-7010/7020), with a small
kernel driver and a userspace daemon on top. The PL is not needed for any of
this, everything runs on the PS side (MIO buttons, MIO LED, XADC). A bitstream
can still be loaded from Linux if you have one.

What's in here:

- Buildroot external tree (`BR2_EXTERNAL`) with a defconfig for the board
- U-Boot SPL -> U-Boot -> Linux boot from SD card (extlinux, no FSBL/Vivado needed)
- `zybo_btn` kernel module: platform driver for BTN4/BTN5, IRQ + debounce,
  events on a misc char device, sysfs attributes
- `zybo-monitor` daemon: reads button events, drives LD4 through the LED
  class, reads the die temperature from the XADC (IIO)
- Init scripts, kernel config fragment, DT, genimage SD card layout

## Layout

```
configs/zybo_z7_defconfig        Buildroot defconfig
board/zybo-z7/
    zybo-z7.dts                  upstream board DT + button node
    linux.fragment               kernel config additions
    extlinux.conf                boot entry read by U-Boot distro boot
    genimage.cfg, post-image.sh  sdcard.img generation
    rootfs-overlay/              /etc/default, S20fpga init script
package/zybo-btn/                Buildroot package for the kernel module
package/zybo-monitor/            Buildroot package for the daemon + init script
src/zybo-btn/                    kernel module sources
src/zybo-monitor/                daemon sources
```

## Versions

| Component | Version |
|-----------|---------|
| Buildroot | 2024.02.x (LTS) |
| Linux     | linux-xlnx `xilinx-v2023.2` (6.1) |
| U-Boot    | u-boot-xlnx `xilinx-v2023.2` |

## Build

Buildroot needs the usual host packages (see the Buildroot manual,
"System requirements").

```sh
git clone https://gitlab.com/buildroot.org/buildroot.git -b 2024.02.9
git clone https://github.com/Asvasist/Embedded_Linux.git

cd buildroot
make BR2_EXTERNAL=../Embedded_Linux O=../out zybo_z7_defconfig
cd ../out
make
```

First build takes a while (toolchain, kernel, U-Boot). Output ends up in
`out/images/`:

```
boot.bin        U-Boot SPL (with ps7_init for the Zybo Z7)
u-boot.img
zImage
zybo-z7.dtb
rootfs.ext4
sdcard.img      everything above in a 2 partition image
```

Rebuilding only the project packages after a change:

```sh
make zybo-btn-rebuild zybo-monitor-rebuild
make            # regenerate the images
```

## Flash and boot

```sh
sudo dd if=images/sdcard.img of=/dev/sdX bs=4M conv=fsync
```

- Set JP5 to SD
- Serial console on the PROG/UART micro USB, 115200 8N1 (`/dev/ttyUSB1` usually)
- Login `root` / `zybo`
- eth0 comes up with DHCP, dropbear is running for ssh

## zybo_btn driver

DT node (in `board/zybo-z7/zybo-z7.dts`):

```dts
buttons {
    compatible = "asv,zybo-btn";
    button-gpios = <&gpio0 50 GPIO_ACTIVE_HIGH>,   /* BTN4 */
                   <&gpio0 51 GPIO_ACTIVE_HIGH>;   /* BTN5 */
    debounce-interval = <20>;
};
```

Both edges of each GPIO trigger an interrupt. The ISR only (re)arms a delayed
work, the work reads the level once it has been stable for `debounce_ms` and
pushes an event into a kfifo. Readers block on a wait queue or use poll().

`read()` on `/dev/zybo_btn` returns one or more of these (see
`src/zybo-btn/zybo_btn.h`):

```c
struct zybo_btn_event {
    __u64 timestamp_ns;   /* CLOCK_MONOTONIC */
    __u32 button;         /* 0 = BTN4, 1 = BTN5 */
    __u32 pressed;        /* 1 pressed, 0 released */
};
```

sysfs, under `/sys/bus/platform/devices/buttons/`:

```
debounce_ms   rw   1..1000
presses       ro   press count per button
dropped       ro   events dropped because nobody was reading
```

Quick check on the target:

```sh
modprobe zybo_btn
dmesg | tail -1            # zybo-btn buttons: 2 buttons, debounce 20 ms
dd if=/dev/zybo_btn bs=16 count=1 2>/dev/null | hexdump -C   # blocks until a press
cat /sys/bus/platform/devices/buttons/presses
```

Building it outside Buildroot against a configured kernel tree:

```sh
make -C src/zybo-btn KDIR=/path/to/linux ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
```

## zybo-monitor

Started by `/etc/init.d/S90zybo-monitor` (which also does the `modprobe`).
Options go in `/etc/default/zybo-monitor`.

- BTN4: LD4 mode heartbeat -> on -> off
- BTN5: writes a status line to syslog (temperature, uptime, load, free mem)
- every `-i` seconds the XADC temperature is logged; above `-w` degrees LD4
  switches to a fast blink until it is 5 degrees below the limit again

It is a single poll() loop over the button device, a timerfd and a signalfd,
so there are no signal handlers or threads. Logs go to syslog:

```sh
tail -f /var/log/messages | grep zybo-monitor
```

Missing pieces (driver not loaded, no XADC) are logged and skipped, the rest
keeps working. The daemon also builds on a PC (`make -C src/zybo-monitor`)
which is handy for checking option parsing and error paths.

## Loading a bitstream (optional)

The Zynq FPGA manager is enabled in the kernel. Convert the bitstream with
bootgen and copy it to the target:

```sh
# system.bif:  all: { system.bit }
bootgen -image system.bif -arch zynq -process_bitstream bin
scp system.bit.bin root@zybo:/lib/firmware/zybo_top.bit.bin
```

`/etc/init.d/S20fpga` programs it at boot. By hand:

```sh
echo 0 > /sys/class/fpga_manager/fpga0/flags
echo zybo_top.bit.bin > /sys/class/fpga_manager/fpga0/firmware
cat /sys/class/fpga_manager/fpga0/state      # operating
```

The `firmware` attribute is a linux-xlnx addition, mainline kernels need a DT
overlay for this instead.

## Notes

- `zybo-z7.dts` includes `zynq-zybo-z7.dts` from the kernel. For kernels 6.5+
  the dts files moved, the include becomes `xilinx/zynq-zybo-z7.dts`.
- There is no mdev/udev coldplug, so modules are loaded from the init scripts.
- The Zybo Z7-10 and Z7-20 use the same DT and U-Boot config.

## License

Kernel module: GPL-2.0. Everything else: MIT.
