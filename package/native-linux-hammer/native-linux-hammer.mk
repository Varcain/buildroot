################################################################################
#
# native-linux-hammer
#
################################################################################

NATIVE_LINUX_HAMMER_VERSION = 9.5.0
NATIVE_LINUX_HAMMER_SITE = $(call github,lvgl,lvgl,v$(NATIVE_LINUX_HAMMER_VERSION))
NATIVE_LINUX_HAMMER_LICENSE = MIT
NATIVE_LINUX_HAMMER_LICENSE_FILES = LICENCE.txt

define NATIVE_LINUX_HAMMER_CONFIGURE_CMDS
	cp $(@D)/lv_conf_template.h $(@D)/lv_conf.h
	sed -i \
		-e 's/^#if 0 \/\* Set this to "1" to enable content \*\//#if 1 \/\* content enabled by Buildroot \*\//' \
		-e 's/^#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN/#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB/' \
		-e 's/^#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN/#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB/' \
		-e 's/^#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN/#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB/' \
		-e 's/^#define LV_USE_LOG 0/#define LV_USE_LOG 1/' \
		-e 's/^    #define LV_LOG_PRINTF 0/    #define LV_LOG_PRINTF 1/' \
		-e 's/^#define LV_FONT_MONTSERRAT_12 0/#define LV_FONT_MONTSERRAT_12 1/' \
		-e 's/^#define LV_FONT_MONTSERRAT_16 0/#define LV_FONT_MONTSERRAT_16 1/' \
		-e 's/^#define LV_FONT_MONTSERRAT_22 0/#define LV_FONT_MONTSERRAT_22 1/' \
		-e 's/^#define LV_FONT_MONTSERRAT_32 0/#define LV_FONT_MONTSERRAT_32 1/' \
		-e 's/^#define LV_USE_SYSMON   0/#define LV_USE_SYSMON   1/' \
		-e 's/^    #define LV_USE_PERF_MONITOR 0/    #define LV_USE_PERF_MONITOR 1/' \
		-e 's/^        #define LV_USE_PERF_MONITOR_LOG_MODE 0/        #define LV_USE_PERF_MONITOR_LOG_MODE 1/' \
		-e 's/^#define LV_USE_LINUX_FBDEV      0/#define LV_USE_LINUX_FBDEV      1/' \
		-e 's/^    #define LV_LINUX_FBDEV_BUFFER_SIZE   60/    #define LV_LINUX_FBDEV_BUFFER_SIZE   272/' \
		-e 's/^    #define LV_LINUX_FBDEV_MMAP          1/    #define LV_LINUX_FBDEV_MMAP          0/' \
		-e 's/^#define LV_USE_EVDEV    0/#define LV_USE_EVDEV    1/' \
		-e 's/^#define LV_BUILD_EXAMPLES 1/#define LV_BUILD_EXAMPLES 0/' \
		-e 's/^#define LV_BUILD_DEMOS 0/#define LV_BUILD_DEMOS 1/' \
		-e 's/^    #define LV_USE_DEMO_MUSIC 0/    #define LV_USE_DEMO_MUSIC 1/' \
		-e 's/^        #define LV_DEMO_MUSIC_LANDSCAPE 0/        #define LV_DEMO_MUSIC_LANDSCAPE 1/' \
		$(@D)/lv_conf.h
	mkdir -p $(@D)/hammer-src
	cp $(NATIVE_LINUX_HAMMER_PKGDIR)/src/*.c $(@D)/hammer-src/
	cp $(NATIVE_LINUX_HAMMER_PKGDIR)/src/Makefile $(@D)/Makefile.hammer
endef

define NATIVE_LINUX_HAMMER_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile.hammer \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -Os -ffunction-sections -fdata-sections" \
		LDFLAGS="$(TARGET_LDFLAGS) -Wl,--gc-sections"
endef

define NATIVE_LINUX_HAMMER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/lvmusic $(TARGET_DIR)/usr/bin/lvmusic
	$(INSTALL) -D -m 0755 $(@D)/hammer-uinput $(TARGET_DIR)/usr/bin/hammer-uinput
	$(INSTALL) -D -m 0755 $(@D)/linux-rt-latency $(TARGET_DIR)/usr/bin/linux-rt-latency
	$(INSTALL) -D -m 0755 $(NATIVE_LINUX_HAMMER_PKGDIR)/native-linux-hammer \
		$(TARGET_DIR)/usr/bin/native-linux-hammer
endef

$(eval $(generic-package))
