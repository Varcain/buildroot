/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS Linux-personality /dev/fb0 smoke test.
 *
 * Opens the framebuffer, prints its geometry (FBIOGET_*SCREENINFO), fills it
 * with an RGB565 gradient (so a host viewer shows a picture), then verifies a
 * pwrite()/pread() round-trip through a known scanline — the fb device is
 * exercised with the exact positioned-write path LVGL's fbdev driver uses when
 * LV_LINUX_FBDEV_MMAP=0. Prints "fbtest: PASS" / "fbtest: FAIL".
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		printf("fbtest: FAIL (open /dev/fb0)\n");
		return 1;
	}

	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
		printf("fbtest: FAIL (ioctl screeninfo)\n");
		return 1;
	}
	printf("fbtest: %ux%u %ubpp line=%lu smem=%lu id=%s\n", vinfo.xres, vinfo.yres,
	       vinfo.bits_per_pixel, (unsigned long)finfo.line_length,
	       (unsigned long)finfo.smem_len, finfo.id);

	if (vinfo.bits_per_pixel != 16 || finfo.line_length == 0) {
		printf("fbtest: FAIL (unexpected geometry)\n");
		return 1;
	}

	/* Fill the panel with an RGB565 gradient, one scanline per pwrite. */
	static uint16_t line[2048];
	unsigned w = vinfo.xres, h = vinfo.yres;
	if (w > 2048)
		w = 2048;
	for (unsigned y = 0; y < h; y++) {
		for (unsigned x = 0; x < w; x++) {
			unsigned r = (x * 31) / (w ? w : 1);
			unsigned g = (y * 63) / (h ? h : 1);
			unsigned b = 31 - r;
			line[x] = (uint16_t)((r << 11) | (g << 5) | b);
		}
		if (pwrite(fd, line, (size_t)w * 2, (off_t)y * finfo.line_length) < 0) {
			printf("fbtest: FAIL (pwrite y=%u)\n", y);
			return 1;
		}
	}

	/* Round-trip a known marker through a middle scanline. */
	uint16_t mark[16], back[16];
	for (int i = 0; i < 16; i++)
		mark[i] = (uint16_t)(0xA000 + i);
	off_t off = (off_t)(h / 2) * finfo.line_length;
	if (pwrite(fd, mark, sizeof(mark), off) != (ssize_t)sizeof(mark)) {
		printf("fbtest: FAIL (pwrite marker)\n");
		return 1;
	}
	if (pread(fd, back, sizeof(back), off) != (ssize_t)sizeof(back)) {
		printf("fbtest: FAIL (pread marker)\n");
		return 1;
	}
	if (memcmp(mark, back, sizeof(mark)) != 0) {
		printf("fbtest: FAIL (round-trip mismatch)\n");
		return 1;
	}

	close(fd);
	printf("fbtest: PASS\n");
	return 0;
}
