/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Event record returned by read() on /dev/zybo_btn.
 * Shared between the kernel driver and userspace.
 */
#ifndef _ZYBO_BTN_H
#define _ZYBO_BTN_H

#include <linux/types.h>

struct zybo_btn_event {
	__u64 timestamp_ns;	/* CLOCK_MONOTONIC, taken after debounce */
	__u32 button;		/* index into the DT "button-gpios" list */
	__u32 pressed;		/* 1 = pressed, 0 = released */
};

#endif /* _ZYBO_BTN_H */
