/* SPDX-License-Identifier: MIT */
#include "health.h"
#include <assert.h>
#include <string.h>

static int parse(const char *text, unsigned long *total, unsigned long *available)
{
    FILE *file = tmpfile();
    int rc;
    assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    rewind(file);
    rc = health_meminfo(file, total, available);
    fclose(file);
    return rc;
}

int main(void)
{
    unsigned long total, available;
    uint32_t words[1024];
    assert(!parse("MemTotal: 1024000 kB\nMemFree: 1 kB\nMemAvailable: 500000 kB\n", &total, &available));
    assert(total == 1024000 && available == 500000);
    assert(parse("", &total, &available));
    assert(parse("MemTotal: 100 kB\n", &total, &available));
    assert(parse("MemTotal: -1 kB\nMemAvailable: 0 kB\n", &total, &available));
    assert(parse("MemTotal: 100 MB\nMemAvailable: 10 kB\n", &total, &available));
    assert(parse("MemTotal: 999999999999999999999999999999 kB\nMemAvailable: 10 kB\n", &total, &available));
    assert(parse("MemTotal: 100 kB garbage\nMemAvailable: 10 kB\n", &total, &available));
    assert(parse("MemTotal: 100kB\nMemAvailable: 10 kB\n", &total, &available));
    assert(parse("MemTotal: 100 kB\nMemAvailable: 200 kB\n", &total, &available));
    assert(parse("MemTotal: 100 kB\nMemTotal: 200 kB\nMemAvailable: 10 kB\n", &total, &available));
    assert(!parse("MemTotal: 100 kB\nMemAvailable: 0 kB\n", &total, &available));
    assert(!health_ram_test(words, 1024));
    assert(words[123] == (0x55555555U ^ 123));
    assert(health_ram_test(NULL, 1));
    assert(health_ram_test(words, 0));
    puts("health tests passed");
    return 0;
}
