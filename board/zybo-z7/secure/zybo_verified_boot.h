/* SPDX-License-Identifier: GPL-2.0+ */
/* Included only by common/main.c in the dedicated Zybo secure profile. */
#include <asm/cache.h>
#include <asm/global_data.h>
#include <cpu_func.h>
#include <linux/delay.h>
#include <linux/libfdt.h>

DECLARE_GLOBAL_DATA_PTR;

static void __noreturn zybo_boot_stop(const char *reason)
{
    printf("\nZybo boot FAIL: %s\nPower off and use an authenticated recovery image.\n", reason);
    /* No interactive prompt, alternate unsigned boot, or reboot loop. */
    for (;;)
        mdelay(1000);
}

static void zybo_boot_fit(const char *filename)
{
    char command[160];
    const char *value;
    unsigned long size, loaded;
    const void *fit = (const void *)0x10000000;

    env_set("filesize", NULL);
    snprintf(command, sizeof(command), "fatsize mmc 0:1 %s", filename);
    if (run_command(command, 0)) {
        printf("%s: unavailable\n", filename);
        return;
    }
    value = env_get("filesize");
    if (!value || strict_strtoul(value, 16, &size) || size < 40 || size > 0x04000000) {
        printf("%s: invalid size (limit 64 MiB)\n", filename);
        return;
    }
    snprintf(command, sizeof(command), "fatload mmc 0:1 0x10000000 %s 0x%lx", filename, size);
    if (run_command(command, 0))
        return;
    value = env_get("filesize");
    if (!value || strict_strtoul(value, 16, &loaded) || loaded != size ||
        fdt_check_header(fit) || fdt_totalsize(fit) != size) {
        printf("%s: incomplete or external-data FIT rejected\n", filename);
        return;
    }
    /* bootm verifies required configuration signatures and all image hashes. */
    printf("Authenticating %s (kernel, device tree and complete root filesystem)...\n", filename);
    run_command("bootm 0x10000000#conf-1", 0);
    printf("%s: rejected or failed to boot\n", filename);
}

static void __noreturn zybo_verified_boot(void)
{
    int node, length, cached, rc;
    const char *required;

    disable_ctrlc(1);
    puts("Zybo Z7-20: fixed authenticated SD boot policy\n");
    node = fdt_path_offset(gd->fdt_blob, "/signature/key-zybo");
    required = node < 0 ? NULL : fdt_getprop(gd->fdt_blob, node, "required", &length);
    if (!required || length != 5 || memcmp(required, "conf", 5))
        zybo_boot_stop("required FIT verification key missing");
    if (gd->ram_size < 0x20000000 || gd->relocaddr < 0x18000000)
        zybo_boot_stop("unexpected DDR layout");

    /* Fixed Z7-20 layout: 128..129 MiB scratch, before loading any FIT.
     * U-Boot starts at 64 MiB and relocates near the top of DDR.
     * Flush/disable caches so the test reaches DDR rather than only cache.
     */
    cached = dcache_status();
    if (cached) {
        flush_dcache_all();
        dcache_disable();
    }
    rc = run_command("mtest 0x08000000 0x08100000 0x55aa55aa 1", 0);
    if (cached)
        dcache_enable();
    if (rc)
        zybo_boot_stop("DDR quick test");
    puts("DDR quick test PASS (1 MiB scratch region only)\n");

    env_set("verify", "yes");
    /* Buildroot's /init mounts devtmpfs before handing off to /sbin/init. */
    env_set("bootargs", "console=ttyPS0,115200 rdinit=/init panic=0");
    env_set("bootm_low", "0");
    env_set("bootm_size", "0x20000000");
    if (run_command("mmc dev 0", 0) || run_command("mmc rescan", 0))
        zybo_boot_stop("SD card initialization");
    zybo_boot_fit("system.itb");
    zybo_boot_fit("recovery.itb");
    zybo_boot_stop("no valid signed image");
}
