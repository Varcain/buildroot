/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS Linux-personality /dev/input/event0 smoke test.
 *
 * Blocking-reads evdev events (exercising the personality's park/retry + input
 * feeder + coordinator kick) and prints the single-touch coordinates the QEMU
 * testpad injector produces. Also prints sizeof(struct input_event) to confirm
 * the kernel/uClibc layout agreement. Prints "evread: DONE got=N".
 */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	int fd = open("/dev/input/event0", O_RDONLY);
	if (fd < 0) {
		printf("evread: FAIL (open /dev/input/event0)\n");
		return 1;
	}
	printf("evread: input_event size=%d\n", (int)sizeof(struct input_event));

	struct input_event ev;
	int got = 0, x = -1, y = -1, down = 0;
	for (int i = 0; i < 400 && got < 6; i++) {
		ssize_t n = read(fd, &ev, sizeof(ev)); /* blocks until the feeder produces */
		if (n != (ssize_t)sizeof(ev))
			continue;
		if (ev.type == EV_ABS && ev.code == ABS_X)
			x = ev.value;
		else if (ev.type == EV_ABS && ev.code == ABS_Y)
			y = ev.value;
		else if (ev.type == EV_KEY && ev.code == BTN_TOUCH)
			down = ev.value;
		else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			printf("evread: touch x=%d y=%d down=%d\n", x, y, down);
			got++;
		}
	}
	close(fd);
	printf("evread: DONE got=%d\n", got);
	return 0;
}
