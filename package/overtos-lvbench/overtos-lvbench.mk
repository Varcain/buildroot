################################################################################
#
# overtos-lvbench — LVGL fbdev benchmark for the oveRTOS Linux personality
#
# Reuses the LVGL v9.5 tree the oveRTOS checkout already vendors (a sibling of
# this buildroot clone: ../oveRTOS/dl/lvgl) via the "local" site method, so no
# network fetch or hash is needed. main.c + lv_conf.h + the build Makefile are
# copied into the extracted tree, then compiled into one static -mfdpic binary.
#
################################################################################

OVERTOS_LVBENCH_VERSION = 9.5.0
# The sibling oveRTOS checkout (../oveRTOS) vendors LVGL v9.5 at dl/lvgl.
OVERTOS_LVBENCH_SITE = $(realpath $(TOPDIR)/../oveRTOS/dl/lvgl)
OVERTOS_LVBENCH_SITE_METHOD = local
OVERTOS_LVBENCH_LICENSE = MIT
OVERTOS_LVBENCH_LICENSE_FILES = LICENCE.txt

# Copy our app sources into the synced LVGL tree, then build. (The local site
# method rsyncs the tree on every build, so the copy must run here, not in an
# extract hook, or a re-sync would wipe it.)
define OVERTOS_LVBENCH_BUILD_CMDS
	cp $(OVERTOS_LVBENCH_PKGDIR)/src/main.c $(@D)/main.c
	cp $(OVERTOS_LVBENCH_PKGDIR)/src/main_music.c $(@D)/main_music.c
	cp $(OVERTOS_LVBENCH_PKGDIR)/src/lv_conf.h $(@D)/lv_conf.h
	cp $(OVERTOS_LVBENCH_PKGDIR)/src/Makefile.lvbench $(@D)/Makefile.lvbench
	# P3: overlay our patched fbdev driver (runtime mmap-or-pwrite fallback) onto the rsynced
	# LVGL tree. LV_LINUX_FBDEV_MMAP=1 makes the guest mmap /dev/fb0 and memcpy pixels straight
	# into it; if the personality can't map the fb (no free MPU region on that engine) the mmap
	# fails and the driver falls back to the per-scanline pwrite, so ONE binary renders on every
	# engine. Copied here (not into dl/lvgl, which `ove download` re-clones) so it is tracked +
	# survives a re-sync, matching how lv_conf.h/main.c are vendored.
	cp $(OVERTOS_LVBENCH_PKGDIR)/src/lv_linux_fbdev.c $(@D)/src/drivers/display/fb/lv_linux_fbdev.c
	# DMA2D offload: route LVGL's DMA2D draw unit through an ioctl on /dev/dma2d (the
	# guest is unprivileged; the coordinator owns the peripheral). Same overlay pattern.
	cp $(OVERTOS_LVBENCH_PKGDIR)/src/lv_draw_dma2d.c $(@D)/src/draw/dma2d/lv_draw_dma2d.c
	cp $(OVERTOS_LVBENCH_PKGDIR)/src/ove_dma2d_hal_shim.h $(@D)/src/draw/dma2d/ove_dma2d_hal_shim.h
	$(MAKE) -C $(@D) -f Makefile.lvbench $(TARGET_CONFIGURE_OPTS) LVGL_DIR=$(@D)
endef

define OVERTOS_LVBENCH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/lvbench $(TARGET_DIR)/usr/bin/lvbench
	$(TARGET_STRIP) --strip-unneeded $(TARGET_DIR)/usr/bin/lvbench
	$(INSTALL) -D -m 0755 $(@D)/lvmusic $(TARGET_DIR)/usr/bin/lvmusic
	$(TARGET_STRIP) --strip-unneeded $(TARGET_DIR)/usr/bin/lvmusic
endef

$(eval $(generic-package))
