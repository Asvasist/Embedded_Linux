/* SPDX-License-Identifier: MIT */
#ifndef ZYBO_HEALTH_H
#define ZYBO_HEALTH_H
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

int health_meminfo(FILE *file, unsigned long *total, unsigned long *available);
int health_ram_test(volatile uint32_t *words, size_t count);
#endif
