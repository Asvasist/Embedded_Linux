// SPDX-License-Identifier: MIT
#include "health.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int memvalue(const char *text, unsigned long *value)
{
    char *end;
    while (isspace((unsigned char)*text))
        text++;
    if (!isdigit((unsigned char)*text))
        return -1;
    errno = 0;
    *value = strtoul(text, &end, 10);
    if (errno || !isspace((unsigned char)*end))
        return -1;
    while (isspace((unsigned char)*end))
        end++;
    if (strncmp(end, "kB", 2))
        return -1;
    end += 2;
    while (isspace((unsigned char)*end))
        end++;
    return *end ? -1 : 0;
}

int health_meminfo(FILE *file, unsigned long *total, unsigned long *available)
{
    char line[256];
    unsigned long value;
    int have_total = 0, have_available = 0;

    *total = *available = 0;
    while (fgets(line, sizeof(line), file)) {
        if (!strncmp(line, "MemTotal:", 9)) {
            if (have_total || memvalue(line + 9, &value) || !value)
                return -1;
            *total = value;
            have_total = 1;
        } else if (!strncmp(line, "MemAvailable:", 13)) {
            if (have_available || memvalue(line + 13, &value))
                return -1;
            *available = value;
            have_available = 1;
        }
    }
    return !ferror(file) && have_total && have_available && *available <= *total ? 0 : -1;
}

int health_ram_test(volatile uint32_t *words, size_t count)
{
    const uint32_t patterns[] = {0x00000000, 0xffffffff, 0xaaaaaaaa, 0x55555555};
    size_t i, p;

    if (!words || !count)
        return -1;
    for (p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
        for (i = 0; i < count; i++)
            words[i] = patterns[p] ^ (uint32_t)i;
        for (i = 0; i < count; i++)
            if (words[i] != (patterns[p] ^ (uint32_t)i))
                return -1;
    }
    return 0;
}
