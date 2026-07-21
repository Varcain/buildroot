/* SPDX-License-Identifier: MIT
 *
 * oveRTOS Linux-personality LVGL music-player demo.
 *
 * A stock LVGL fbdev program: render lv_demo_music to /dev/fb0 (RGB565, the
 * personality's framebuffer device). Shares the overtos-lvbench build (same
 * LVGL tree, lv_conf.h, patched fbdev + DMA2D overlays) as the lvbench binary,
 * differing only in main(): it starts the music player and runs LVGL's timer
 * loop forever (the demo has no terminating condition — quit from the shell).
 * Built as a static -mfdpic binary by the overtos board (/usr/bin/lvmusic).
 */
#include "lvgl.h"
#include "demos/lv_demos.h"

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	lv_init();

	lv_display_t *disp = lv_linux_fbdev_create();
	if (!disp) {
		printf("lvmusic: cannot create fbdev display\n");
		return 1;
	}
	lv_linux_fbdev_set_file(disp, "/dev/fb0");

	/* Touch input (best-effort — absent if the board has no /dev/input/event0). */
	lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event0");
	if (touch) {
		lv_indev_set_display(touch, disp);
		printf("lvmusic: touch input on /dev/input/event0\n");
	}

	printf("lvmusic: starting lv_demo_music on /dev/fb0\n");
	lv_demo_music();

	/* LVGL main loop: run due timers, sleep until the next one (bounded). */
	for (;;) {
		uint32_t idle = lv_timer_handler();
		if (idle == LV_NO_TIMER_READY || idle > 30)
			idle = 30;
		usleep(idle * 1000);
	}
	return 0;
}
