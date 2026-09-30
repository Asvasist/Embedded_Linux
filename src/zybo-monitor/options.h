/* SPDX-License-Identifier: MIT */
#ifndef ZYBO_MONITOR_OPTIONS_H
#define ZYBO_MONITOR_OPTIONS_H

int parse_interval(const char *text, unsigned int *value);
int parse_temperature(const char *text, double *value);

#endif
