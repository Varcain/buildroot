/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Inject single-touch events into the oveRTOS Linux personality evdev device.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_HOLD_MS 50
#define MAX_DELAY_MS 60000
#define MAX_DRAG_STEPS 100

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s tap X Y [hold-ms]\n"
		"  %s down X Y\n"
		"  %s move X Y\n"
		"  %s up X Y\n"
		"  %s drag X1 Y1 X2 Y2 [duration-ms [steps]]\n",
		prog, prog, prog, prog, prog);
}

static int parse_int(const char *s, int min, int max, int *out)
{
	char *end;
	errno = 0;
	long v = strtol(s, &end, 0);
	if (errno || *s == '\0' || *end != '\0' || v < min || v > max)
		return -1;
	*out = (int)v;
	return 0;
}

static int report_touch(int fd, int x, int y, int pressed)
{
	struct input_event ev[4];
	memset(ev, 0, sizeof(ev));
	ev[0].type = EV_ABS;
	ev[0].code = ABS_X;
	ev[0].value = x;
	ev[1].type = EV_ABS;
	ev[1].code = ABS_Y;
	ev[1].value = y;
	ev[2].type = EV_KEY;
	ev[2].code = BTN_TOUCH;
	ev[2].value = pressed;
	ev[3].type = EV_SYN;
	ev[3].code = SYN_REPORT;

	ssize_t n = write(fd, ev, sizeof(ev));
	if (n == (ssize_t)sizeof(ev))
		return 0;
	if (n >= 0)
		errno = EIO;
	return -1;
}

static int sleep_ms(int ms)
{
	struct timespec req = {
		.tv_sec = ms / 1000,
		.tv_nsec = (ms % 1000) * 1000000L,
	};
	while (nanosleep(&req, &req) != 0) {
		if (errno != EINTR)
			return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int x1, y1, x2, y2, delay_ms, steps;
	int fd = -1;
	int rc = 1;

	if (argc < 4 || parse_int(argv[2], 0, 65535, &x1) ||
	    parse_int(argv[3], 0, 65535, &y1)) {
		usage(argv[0]);
		return 2;
	}

	fd = open("/dev/input/event0", O_WRONLY);
	if (fd < 0) {
		perror("touchctl: open /dev/input/event0");
		return 1;
	}

	if (strcmp(argv[1], "tap") == 0) {
		delay_ms = DEFAULT_HOLD_MS;
		if ((argc != 4 && argc != 5) ||
		    (argc == 5 && parse_int(argv[4], 0, MAX_DELAY_MS, &delay_ms)))
			goto bad_usage;
		if (report_touch(fd, x1, y1, 1) || sleep_ms(delay_ms) ||
		    report_touch(fd, x1, y1, 0))
			goto io_error;
	} else if (strcmp(argv[1], "down") == 0 || strcmp(argv[1], "move") == 0) {
		if (argc != 4)
			goto bad_usage;
		if (report_touch(fd, x1, y1, 1))
			goto io_error;
	} else if (strcmp(argv[1], "up") == 0) {
		if (argc != 4)
			goto bad_usage;
		if (report_touch(fd, x1, y1, 0))
			goto io_error;
	} else if (strcmp(argv[1], "drag") == 0) {
		delay_ms = 500;
		steps = 10;
		if (argc < 6 || argc > 8 ||
		    parse_int(argv[4], 0, 65535, &x2) ||
		    parse_int(argv[5], 0, 65535, &y2) ||
		    (argc >= 7 && parse_int(argv[6], 0, MAX_DELAY_MS, &delay_ms)) ||
		    (argc == 8 && parse_int(argv[7], 1, MAX_DRAG_STEPS, &steps)))
			goto bad_usage;
		if (report_touch(fd, x1, y1, 1))
			goto io_error;
		for (int i = 1; i <= steps; i++) {
			if (sleep_ms(delay_ms / steps) ||
			    report_touch(fd, x1 + (x2 - x1) * i / steps,
					 y1 + (y2 - y1) * i / steps, 1))
				goto io_error;
		}
		if (report_touch(fd, x2, y2, 0))
			goto io_error;
	} else {
		goto bad_usage;
	}

	rc = 0;
	goto out;

bad_usage:
	usage(argv[0]);
	rc = 2;
	goto out;
io_error:
	perror("touchctl");
out:
	close(fd);
	return rc;
}
