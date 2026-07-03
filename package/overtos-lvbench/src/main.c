/* SPDX-License-Identifier: MIT
 *
 * oveRTOS Linux-personality LVGL benchmark.
 *
 * A stock LVGL fbdev program: render lv_demo_benchmark to /dev/fb0 (RGB565, the
 * personality's framebuffer device), driving the display with pwrite() scanlines
 * (LV_LINUX_FBDEV_MMAP=0). Built as a static -mfdpic binary by the overtos board.
 */
#include "lvgl.h"
#include "demos/lv_demos.h"

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* Print the benchmark summary to stdout when all scenes finish, then exit — so a
 * headless run has a definitive "done + FPS" signal and returns to the shell. */
static void on_end(const lv_demo_benchmark_summary_t *s)
{
	printf("lvbench: DONE avg_fps=%ld avg_cpu=%ld avg_render_ms=%ld\n",
	       (long)s->total_avg_fps, (long)s->total_avg_cpu, (long)s->total_avg_render_time);
	fflush(stdout);
	_exit(0);
}

int main(void)
{
	lv_init();

	lv_display_t *disp = lv_linux_fbdev_create();
	if (!disp) {
		printf("lvbench: cannot create fbdev display\n");
		return 1;
	}
	lv_linux_fbdev_set_file(disp, "/dev/fb0");

	/* Touch input (best-effort — absent if the board has no /dev/input/event0). */
	lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event0");
	if (touch) {
		lv_indev_set_display(touch, disp);
		printf("lvbench: touch input on /dev/input/event0\n");
	}

	printf("lvbench: starting lv_demo_benchmark on /dev/fb0\n");
	lv_demo_benchmark_set_end_cb(on_end);
	lv_demo_benchmark();

	/* LVGL main loop: run due timers, sleep until the next one (bounded). */
	for (;;) {
		uint32_t idle = lv_timer_handler();
		if (idle == LV_NO_TIMER_READY || idle > 30)
			idle = 30;
		usleep(idle * 1000);
	}
	return 0;
}
