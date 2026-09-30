// SPDX-License-Identifier: MIT
/* Startup diagnostics, not a certification of every physical peripheral. */
#include "health.h"
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_BYTES (4U * 1024U * 1024U)
static int failures;

static void result(const char *name, const char *state, const char *detail)
{
    printf("%-24s %-10s %s\n", name, state, detail);
    if (!strcmp(state, "FAIL"))
        failures++;
}

static int number(const char *dir, const char *attribute, double *out)
{
    char path[512], extra;
    FILE *f;
    int n;
    if (snprintf(path, sizeof(path), "%s/%s", dir, attribute) >= (int)sizeof(path))
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    n = fscanf(f, "%lf %c", out, &extra);
    fclose(f);
    return n == 1 && isfinite(*out) ? 0 : -1;
}

static void temperature(void)
{
    DIR *d = opendir("/sys/bus/iio/devices");
    struct dirent *de;
    int found = 0;
    if (d) {
        while ((de = readdir(d))) {
            char dir[384], path[512], name[32], detail[80];
            double raw, offset, scale, celsius;
            FILE *f;
            if (strncmp(de->d_name, "iio:device", 10))
                continue;
            snprintf(dir, sizeof(dir), "/sys/bus/iio/devices/%s", de->d_name);
            snprintf(path, sizeof(path), "%s/name", dir);
            f = fopen(path, "r");
            if (!f)
                continue;
            name[0] = 0;
            (void)fscanf(f, "%31s", name);
            fclose(f);
            if (strcmp(name, "xadc"))
                continue;
            found = 1;
            if (number(dir, "in_temp0_raw", &raw) ||
                number(dir, "in_temp0_offset", &offset) ||
                number(dir, "in_temp0_scale", &scale)) {
                result("XADC", "FAIL", "cannot read temperature");
                break;
            }
            celsius = (raw + offset) * scale / 1000.0;
            snprintf(detail, sizeof(detail), "%.1f C; startup range -40..85 C", celsius);
            result("XADC", isfinite(celsius) && celsius >= -40 && celsius < 85 ? "PASS" : "FAIL", detail);
            break;
        }
        closedir(d);
    }
    if (!found)
        result("XADC", "FAIL", "required monitor sensor unavailable");
}

int main(void)
{
    unsigned long total, available;
    FILE *f = fopen("/proc/meminfo", "r");
    char detail[128];
    struct stat st;
    int fd;
    double carrier;
    uint32_t *memory;

    puts("Zybo Z7 startup checks (driver readiness and bounded diagnostics)");
    if (!f || health_meminfo(f, &total, &available)) {
        result("Linux memory", "FAIL", "invalid or missing /proc/meminfo");
    } else {
        snprintf(detail, sizeof(detail), "total %lu KiB; available %lu KiB", total, available);
        result("Linux memory", total >= 256UL * 1024 && available >= 32UL * 1024 ? "PASS" : "FAIL", detail);
    }
    if (f)
        fclose(f);
    /* Never overwrite physical RAM owned by Linux or another process. */
    memory = failures ? NULL : malloc(TEST_BYTES);
    if (!memory) {
        result("RAM allocation test", "FAIL", "cannot safely allocate 4 MiB");
    } else {
        result("RAM allocation test", health_ram_test(memory, TEST_BYTES / sizeof(*memory)) ? "FAIL" : "PASS",
               "4 MiB only; cached userspace test, not full DDR coverage");
        free(memory);
    }
    /* misc_open does not wait for an event; no read is attempted here. */
    fd = open("/dev/zybo_btn", O_RDONLY);
    result("Button interface", fd >= 0 && !fstat(fd, &st) && S_ISCHR(st.st_mode) ? "PASS" : "FAIL",
           "required /dev/zybo_btn character device");
    if (fd >= 0)
        close(fd);
    result("Physical buttons", "NOT_TESTED", "requires operator input");
    fd = open("/sys/class/leds/zynq-zybo-z7:green:ld4/brightness", O_WRONLY);
    result("LED interface", fd >= 0 ? "PASS" : "FAIL", "required LD4 output interface");
    if (fd >= 0)
        close(fd);
    result("Physical LED", "NOT_TESTED", "requires visual observation");
    temperature();
    if (access("/sys/class/net/eth0", F_OK))
        result("Ethernet", "SKIPPED", "no interface; optional for board monitoring");
    else if (number("/sys/class/net/eth0", "carrier", &carrier) || carrier != 1)
        result("Ethernet link", "SKIPPED", "no active link yet; network starts later");
    else
        result("Ethernet link", "PASS", "link only; remote communication not tested");
    result("USB/HDMI/audio/PMOD", "NOT_TESTED", "needs attached devices and application-specific tests");
    result("FPGA", "SKIPPED", "optional programming and state check run in S20fpga");
    result("Boot authentication", "NOT_TESTED", "enforced by boot stages, not attested by this Linux program");
    puts(failures ? "STARTUP FAIL" : "STARTUP PASS (see skipped and untested items above)");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
