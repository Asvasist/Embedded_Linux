// SPDX-License-Identifier: MIT
#include "options.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>

int parse_interval(const char *text, unsigned int *value)
{
    char *end;
    unsigned long number;

    if (!text || !isdigit((unsigned char)text[0]))
        return -1;
    errno = 0;
    number = strtoul(text, &end, 10);
    if (errno || *end || number < 1 || number > 86400)
        return -1;
    *value = (unsigned int)number;
    return 0;
}

int parse_temperature(const char *text, double *value)
{
    char *end;
    double number;

    if (!text || !*text || isspace((unsigned char)text[0]))
        return -1;
    errno = 0;
    number = strtod(text, &end);
    if (errno || end == text || *end || !isfinite(number) || number < -40 || number > 125)
        return -1;
    *value = number;
    return 0;
}
