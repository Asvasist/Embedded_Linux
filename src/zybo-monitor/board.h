/* SPDX-License-Identifier: MIT */
#ifndef BOARD_H
#define BOARD_H

enum led_mode {
	LED_HEARTBEAT,
	LED_ON,
	LED_OFF,
	LED_ALERT,	/* fast blink, used for over-temperature */
};

int led_init(const char *name);
int led_set_mode(enum led_mode mode);
const char *led_mode_name(enum led_mode mode);

int xadc_init(void);
int xadc_read_temp(double *celsius);

#endif
