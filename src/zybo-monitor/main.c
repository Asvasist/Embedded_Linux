// SPDX-License-Identifier: MIT
/*
 * zybo-monitor - small board daemon for the Zybo Z7
 *
 *   BTN4  cycle LD4 mode: heartbeat -> on -> off
 *   BTN5  log a status line (temperature, uptime, load, presses)
 *
 * The XADC die temperature is sampled every -i seconds. Above the warning
 * threshold LD4 blinks fast until the temperature drops TEMP_HYST below it.
 *
 * Runs in the foreground; the init script backgrounds it.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>
#include <syslog.h>
#include <unistd.h>

#include "zybo_btn.h"
#include "board.h"
#include "options.h"

#define DEF_DEVICE	"/dev/zybo_btn"
#define DEF_LED		"zynq-zybo-z7:green:ld4"
#define DEF_INTERVAL	60
#define DEF_WARN	70
#define TEMP_HYST	5.0

#define BTN_MODE	0	/* BTN4 / MIO50 */
#define BTN_STATUS	1	/* BTN5 / MIO51 */

static enum led_mode user_mode = LED_HEARTBEAT;
static int alert;
static unsigned long presses[2];
static double warn_temp = DEF_WARN;

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-d device] [-l led] [-i seconds] [-w celsius] [-v]\n"
		"  -d  button device       (default " DEF_DEVICE ")\n"
		"  -l  LED class name      (default " DEF_LED ")\n"
		"  -i  temperature period  (1..86400 s, default %d s)\n"
		"  -w  warning threshold   (-40..125 C, default %d C)\n"
		"  -v  also log to stderr\n",
		prog, DEF_INTERVAL, DEF_WARN);
}

static void set_led(enum led_mode mode)
{
	int ret = led_set_mode(mode);

	if (ret && ret != -ENODEV)
		syslog(LOG_ERR, "led %s: %s", led_mode_name(mode), strerror(-ret));
}

static void check_temp(int log_it)
{
	static int warned;
	double t;
	int ret;

	ret = xadc_read_temp(&t);
	if (ret) {
		if (!warned)
			syslog(LOG_WARNING, "xadc: %s", strerror(-ret));
		warned = 1;
		return;
	}
	warned = 0;

	if (log_it)
		syslog(LOG_INFO, "temp %.1f C", t);

	if (!alert && t >= warn_temp) {
		alert = 1;
		syslog(LOG_WARNING, "over temperature: %.1f C (limit %.0f)",
		       t, warn_temp);
		set_led(LED_ALERT);
	} else if (alert && t < warn_temp - TEMP_HYST) {
		alert = 0;
		syslog(LOG_NOTICE, "temperature back to normal: %.1f C", t);
		set_led(user_mode);
	}
}

static void log_status(void)
{
	struct sysinfo si;
	char temp[16] = "n/a";
	double t;

	if (!xadc_read_temp(&t))
		snprintf(temp, sizeof(temp), "%.1f C", t);

	if (sysinfo(&si) < 0) {
		syslog(LOG_ERR, "sysinfo: %m");
		return;
	}

	syslog(LOG_INFO, "status: temp %s, up %lds, load %.2f, "
	       "mem free %" PRIu64 " kB, led %s%s, presses %lu/%lu",
	       temp, si.uptime, si.loads[0] / 65536.0,
	       (uint64_t)si.freeram * si.mem_unit / 1024,
	       led_mode_name(user_mode), alert ? " (alert)" : "",
	       presses[0], presses[1]);
}

static void handle_event(const struct zybo_btn_event *ev)
{
	if (!ev->pressed)
		return;
	if (ev->button < 2)
		presses[ev->button]++;

	switch (ev->button) {
	case BTN_MODE:
		user_mode = (user_mode + 1) % 3;	/* skip LED_ALERT */
		syslog(LOG_INFO, "led mode: %s", led_mode_name(user_mode));
		if (!alert)
			set_led(user_mode);
		break;
	case BTN_STATUS:
		log_status();
		break;
	default:
		syslog(LOG_DEBUG, "button %u ignored", ev->button);
		break;
	}
}

static int read_buttons(int fd)
{
	struct zybo_btn_event ev[8];
	ssize_t n;
	size_t i;

	n = read(fd, ev, sizeof(ev));
	if (n < 0)
		return (errno == EAGAIN || errno == EINTR) ? 0 : -errno;
	if (!n)
		return -ENODEV;
	if ((size_t)n % sizeof(ev[0]))
		return -EPROTO;

	for (i = 0; i < (size_t)n / sizeof(ev[0]); i++)
		handle_event(&ev[i]);
	return 0;
}

static int open_signalfd(void)
{
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
		return -1;

	return signalfd(-1, &mask, SFD_CLOEXEC);
}

static int open_timerfd(unsigned int sec)
{
	struct itimerspec its = {
		.it_interval.tv_sec = sec,
		.it_value.tv_sec = sec,
	};
	int fd;

	fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (fd < 0)
		return -1;
	if (timerfd_settime(fd, 0, &its, NULL) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char *argv[])
{
	const char *device = DEF_DEVICE;
	const char *led = DEF_LED;
	unsigned int interval = DEF_INTERVAL;
	int logopt = LOG_PID;
	struct pollfd pfd[3];
	int btn_fd, tfd, sfd, opt, ret;
	int running = 1;
	int exit_status = 0;

	while ((opt = getopt(argc, argv, "d:l:i:w:vh")) != -1) {
		switch (opt) {
		case 'd':
			device = optarg;
			break;
		case 'l':
			led = optarg;
			break;
		case 'i':
			if (parse_interval(optarg, &interval)) {
				fprintf(stderr, "interval must be a decimal integer in 1..86400 seconds\n");
				return 1;
			}
			break;
		case 'w':
			if (parse_temperature(optarg, &warn_temp)) {
				fprintf(stderr, "warning temperature must be finite and in -40..125 C\n");
				return 1;
			}
			break;
		case 'v':
			logopt |= LOG_PERROR;
			break;
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}
	if (optind != argc) {
		usage(argv[0]);
		return 1;
	}

	openlog("zybo-monitor", logopt, LOG_DAEMON);

	sfd = open_signalfd();
	tfd = open_timerfd(interval);
	if (sfd < 0 || tfd < 0) {
		syslog(LOG_ERR, "signalfd/timerfd: %m");
		if (sfd >= 0)
			close(sfd);
		if (tfd >= 0)
			close(tfd);
		closelog();
		return 1;
	}

	/* keep going with whatever is available, missing parts are logged */
	btn_fd = open(device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (btn_fd < 0)
		syslog(LOG_WARNING, "%s: %m, buttons disabled", device);

	ret = led_init(led);
	if (ret)
		syslog(LOG_WARNING, "led %s: %s", led, strerror(-ret));
	else
		set_led(user_mode);

	ret = xadc_init();
	if (ret)
		syslog(LOG_WARNING, "no xadc found, temperature disabled");

	syslog(LOG_INFO, "started (interval %us, warn %.0f C)",
	       interval, warn_temp);
	check_temp(1);

	pfd[0].fd = btn_fd;	/* poll() ignores a negative fd */
	pfd[0].events = POLLIN;
	pfd[1].fd = tfd;
	pfd[1].events = POLLIN;
	pfd[2].fd = sfd;
	pfd[2].events = POLLIN;

	while (running) {
		if (poll(pfd, 3, -1) < 0) {
			if (errno == EINTR)
				continue;
			syslog(LOG_ERR, "poll: %m");
			exit_status = 1;
			break;
		}

		if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			syslog(LOG_ERR, "button device disconnected; buttons disabled");
			close(btn_fd);
			btn_fd = pfd[0].fd = -1;
		} else if (pfd[0].revents & POLLIN) {
			ret = read_buttons(btn_fd);
			if (ret) {
				syslog(LOG_ERR, "read %s: %s", device, strerror(-ret));
				close(btn_fd);
				btn_fd = pfd[0].fd = -1;
			}
		}
		if ((pfd[1].revents | pfd[2].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
			syslog(LOG_ERR, "timer/signal descriptor failed");
			exit_status = 1;
			break;
		}

		if (pfd[1].revents & POLLIN) {
			uint64_t expirations;

			if (read(tfd, &expirations, sizeof(expirations)) > 0)
				check_temp(1);
		}

		if (pfd[2].revents & POLLIN) {
			struct signalfd_siginfo si;

			if (read(sfd, &si, sizeof(si)) == sizeof(si))
				syslog(LOG_INFO, "got signal %u, exiting", si.ssi_signo);
			running = 0;
		}
	}

	set_led(LED_HEARTBEAT);
	if (btn_fd >= 0)
		close(btn_fd);
	close(tfd);
	close(sfd);
	closelog();
	return exit_status;
}
