// SPDX-License-Identifier: MIT
/*
 * sysfs helpers: LED class device and XADC (IIO) die temperature.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "board.h"

#define LED_SYSFS	"/sys/class/leds"
#define IIO_SYSFS	"/sys/bus/iio/devices"

static char led_dir[512];
static char xadc_dir[512];

static int sysfs_write(const char *dir, const char *attr, const char *val)
{
	char path[640];
	size_t len = strlen(val);
	ssize_t n;
	int fd, ret = 0;

	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	n = write(fd, val, len);
	if (n < 0)
		ret = -errno;
	else if ((size_t)n != len)
		ret = -EIO;

	close(fd);
	return ret;
}

static int sysfs_read(const char *dir, const char *attr, char *buf, size_t size)
{
	char path[640];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return -errno;

	buf[n] = '\0';
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

static int sysfs_read_double(const char *dir, const char *attr, double *val)
{
	char buf[64], *end;
	int ret;

	ret = sysfs_read(dir, attr, buf, sizeof(buf));
	if (ret)
		return ret;

	errno = 0;
	*val = strtod(buf, &end);
	if (errno || end == buf)
		return -EINVAL;
	return 0;
}

int led_init(const char *name)
{
	snprintf(led_dir, sizeof(led_dir), LED_SYSFS "/%s", name);
	if (access(led_dir, F_OK) < 0) {
		led_dir[0] = '\0';
		return -errno;
	}
	return 0;
}

int led_set_mode(enum led_mode mode)
{
	int ret;

	if (!led_dir[0])
		return -ENODEV;

	switch (mode) {
	case LED_HEARTBEAT:
		return sysfs_write(led_dir, "trigger", "heartbeat");
	case LED_ON:
	case LED_OFF:
		ret = sysfs_write(led_dir, "trigger", "none");
		if (!ret)
			ret = sysfs_write(led_dir, "brightness",
					  mode == LED_ON ? "1" : "0");
		return ret;
	case LED_ALERT:
		/* delay_on/delay_off only show up once the trigger is set */
		ret = sysfs_write(led_dir, "trigger", "timer");
		if (!ret)
			ret = sysfs_write(led_dir, "delay_on", "100");
		if (!ret)
			ret = sysfs_write(led_dir, "delay_off", "100");
		return ret;
	}
	return -EINVAL;
}

const char *led_mode_name(enum led_mode mode)
{
	switch (mode) {
	case LED_HEARTBEAT:	return "heartbeat";
	case LED_ON:		return "on";
	case LED_OFF:		return "off";
	case LED_ALERT:		return "alert";
	}
	return "?";
}

/* find the iio:deviceN whose name is "xadc" */
int xadc_init(void)
{
	struct dirent *de;
	char dir[512], name[32];
	DIR *d;

	d = opendir(IIO_SYSFS);
	if (!d)
		return -errno;

	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "iio:device", 10))
			continue;
		snprintf(dir, sizeof(dir), IIO_SYSFS "/%s", de->d_name);
		if (sysfs_read(dir, "name", name, sizeof(name)))
			continue;
		if (!strcmp(name, "xadc")) {
			snprintf(xadc_dir, sizeof(xadc_dir), "%s", dir);
			closedir(d);
			return 0;
		}
	}

	closedir(d);
	return -ENODEV;
}

int xadc_read_temp(double *celsius)
{
	double raw, offset, scale;
	int ret;

	if (!xadc_dir[0])
		return -ENODEV;

	ret = sysfs_read_double(xadc_dir, "in_temp0_raw", &raw);
	if (!ret)
		ret = sysfs_read_double(xadc_dir, "in_temp0_offset", &offset);
	if (!ret)
		ret = sysfs_read_double(xadc_dir, "in_temp0_scale", &scale);
	if (ret)
		return ret;

	/* IIO: (raw + offset) * scale gives milli degrees C */
	*celsius = (raw + offset) * scale / 1000.0;
	return 0;
}
