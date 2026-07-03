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
	/* Emit the benchmark's own per-scene CSV breakdown to the console before exiting:
	 * "Name, Avg. CPU, Avg. FPS, Avg. time, render time, flush time" + one row per scene
	 * (rectangles, borders, images, text, lines, arcs, ...) + the all-scenes average. This
	 * is logged via LV_LOG inside summary_display (LV_USE_LOG + LV_LOG_PRINTF in lv_conf.h);
	 * the summary screen it also builds is never flushed since we _exit right after. */
	lv_demo_benchmark_summary_display(s);
	/* The summary's total_avg_* fields are SUMS over the scenes; the benchmark's own
	 * "All scenes avg." divides them by valid_scene_cnt — do the same so this line is a
	 * real average, not a per-scene sum. */
	long n = s->valid_scene_cnt > 0 ? s->valid_scene_cnt : 1;
	printf("lvbench: DONE scenes=%ld avg_fps=%ld avg_cpu=%ld%% avg_render_ms=%ld avg_flush_ms=%ld\n",
	       (long)s->valid_scene_cnt, (long)s->total_avg_fps / n, (long)s->total_avg_cpu / n,
	       (long)s->total_avg_render_time / n, (long)s->total_avg_flush_time / n);
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
