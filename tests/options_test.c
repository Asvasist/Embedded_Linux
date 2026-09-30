/* SPDX-License-Identifier: MIT */
#include "options.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    unsigned int interval;
    double temperature;
    const char *bad_intervals[] = {"", "0", "-1", "1x", "1.5", " 1", "1 ",
                                   "86401", "999999999999999999999", "0x10"};
    const char *bad_temperatures[] = {"", "nan", "NaN", "inf", "-inf", "1e9999",
                                      "70C", "70 ", " 70", "-41", "126"};
    size_t i;

    assert(!parse_interval("1", &interval) && interval == 1);
    assert(!parse_interval("86400", &interval) && interval == 86400);
    for (i = 0; i < sizeof(bad_intervals) / sizeof(*bad_intervals); i++)
        assert(parse_interval(bad_intervals[i], &interval));
    assert(!parse_temperature("70.5", &temperature) && temperature == 70.5);
    assert(!parse_temperature("-40", &temperature) && temperature == -40);
    assert(!parse_temperature("125", &temperature) && temperature == 125);
    for (i = 0; i < sizeof(bad_temperatures) / sizeof(*bad_temperatures); i++)
        assert(parse_temperature(bad_temperatures[i], &temperature));
    puts("monitor option tests passed");
    return 0;
}
