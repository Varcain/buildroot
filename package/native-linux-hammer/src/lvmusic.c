// SPDX-License-Identifier: MIT
/* Native-Linux LVGL music workload for the STM32F746G-DISCO. */

#include "lvgl.h"
#include "demos/lv_demos.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include "src/drivers/evdev/lv_evdev.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const char *path_from_arg_or_env(int argc, char **argv, int index,
					const char *name, const char *fallback)
{
	const char *value;

	if (argc > index && argv[index][0] != '\0')
		return argv[index];
	value = getenv(name);
	if (value != NULL && value[0] != '\0')
		return value;
	return fallback;
}

static void report_framebuffer(const char *path)
{
	struct fb_var_screeninfo variable;
	struct fb_fix_screeninfo fixed;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		fprintf(stderr, "lvmusic: cannot inspect %s: %s\n", path,
			strerror(errno));
		return;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &variable) == 0 &&
	    ioctl(fd, FBIOGET_FSCREENINFO, &fixed) == 0) {
		printf("lvmusic: framebuffer=%s id=%.*s xres=%u yres=%u "
		       "virtual=%ux%u bpp=%u line_length=%u\n",
		       path, (int)sizeof(fixed.id), fixed.id, variable.xres,
		       variable.yres, variable.xres_virtual, variable.yres_virtual,
		       variable.bits_per_pixel, fixed.line_length);
	}
	close(fd);
}

static int attach_pointer(lv_display_t *display, const char *path,
			  const char *role, int required)
{
	lv_indev_t *pointer;

	if (path == NULL || path[0] == '\0')
		return required ? -1 : 0;
	pointer = lv_evdev_create(LV_INDEV_TYPE_POINTER, path);
	if (pointer == NULL) {
		fprintf(stderr, "lvmusic: cannot open %s input %s: %s\n", role,
			path, strerror(errno));
		return required ? -1 : 0;
	}
	lv_indev_set_display(pointer, display);
	printf("lvmusic: %s-input=%s\n", role, path);
	return 0;
}

int main(int argc, char **argv)
{
	const char *framebuffer = path_from_arg_or_env(argc, argv, 1,
						      "LVGL_FBDEV", "/dev/fb0");
	const char *primary = path_from_arg_or_env(argc, argv, 2,
						  "LVGL_EVDEV", NULL);
	const char *secondary = getenv("LVGL_EVDEV_SECONDARY");
	lv_display_t *display;

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);
	report_framebuffer(framebuffer);
	printf("lvmusic: lvgl=9.5.0 color=RGB565 refresh_ms=33 "
	       "render=software flush=linux-fbdev-pwrite dma2d=none "
	       "buffer_lines=272 perf_log=1\n");

	lv_init();
	display = lv_linux_fbdev_create();
	if (display == NULL) {
		fprintf(stderr, "lvmusic: cannot create fbdev display\n");
		return EXIT_FAILURE;
	}
	if (lv_linux_fbdev_set_file(display, framebuffer) != LV_RESULT_OK) {
		fprintf(stderr, "lvmusic: cannot use framebuffer %s: %s\n",
			framebuffer, strerror(errno));
		return EXIT_FAILURE;
	}
	if (attach_pointer(display, primary, "automation", 1) != 0)
		return EXIT_FAILURE;
	(void)attach_pointer(display, secondary, "physical", 0);

	printf("lvmusic: starting lv_demo_music\n");
	lv_demo_music();
	for (;;) {
		uint32_t wait_ms = lv_timer_handler();

		if (wait_ms == LV_NO_TIMER_READY || wait_ms > 30)
			wait_ms = 30;
		if (wait_ms == 0)
			wait_ms = 1;
		usleep(wait_ms * 1000U);
	}
}
