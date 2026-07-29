################################################################################
#
# micropython
#
################################################################################

MICROPYTHON_VERSION = 1.28.0
MICROPYTHON_SITE = https://micropython.org/resources/source
MICROPYTHON_SOURCE = micropython-$(MICROPYTHON_VERSION).tar.xz
# Micropython has a lot of code copied from other projects, and also a number
# of submodules for various libs. However, we don't even clone the submodules,
# and most of the copied code is not used in the unix build.
MICROPYTHON_LICENSE = MIT, BSD-1-Clause, BSD-3-Clause, Zlib
MICROPYTHON_LICENSE_FILES = LICENSE
MICROPYTHON_DEPENDENCIES = host-python3
MICROPYTHON_CPE_ID_VENDOR = micropython

# 0004-py-objarray-fix-use-after-free-if-extending-a-bytearray-from-itself.patch
MICROPYTHON_IGNORE_CVES += CVE-2024-8947

# Use fallback implementation for exception handling on architectures that don't
# have explicit support.
ifeq ($(BR2_i386)$(BR2_x86_64)$(BR2_arm)$(BR2_armeb),)
MICROPYTHON_CFLAGS += -DMICROPY_GCREGS_SETJMP=1
endif

# xtensa has problems with nlr_push, use setjmp based implementation instead
ifeq ($(BR2_xtensa),y)
MICROPYTHON_CFLAGS += -DMICROPY_NLR_SETJMP=1
endif

# The FDPIC/NOMMU personality provides a 512 KiB per-process allocation
# arena.  Leave room for libc, stacks and extension libraries around the
# interpreter's GC heap.
ifeq ($(BR2_BINFMT_FDPIC),y)
MICROPYTHON_CFLAGS += -DMICROPY_UNIX_GC_HEAP_SIZE=327680
endif

# When building from a tarball we don't have some of the dependencies that are in
# the git repository as submodules
MICROPYTHON_MAKE_OPTS += \
	MICROPY_PY_BTREE=0 \
	MICROPY_PY_USSL=0 \
	CROSS_COMPILE=$(TARGET_CROSS) \
	CFLAGS_EXTRA="$(MICROPYTHON_CFLAGS)" \
	LDFLAGS_EXTRA="$(TARGET_LDFLAGS)" \
	CWARN=

# MicroPython's Unix FFI assumes direct function pointers. FDPIC uses
# function descriptors, so calls through modffi are not ABI-safe.
ifeq ($(BR2_BINFMT_FDPIC),y)
MICROPYTHON_MAKE_OPTS += MICROPY_PY_FFI=0
else
# Support libffi in MicroPython itself; separate from unix-ffi libraries.
ifeq ($(BR2_PACKAGE_LIBFFI),y)
MICROPYTHON_DEPENDENCIES += host-pkgconf libffi
MICROPYTHON_MAKE_OPTS += MICROPY_PY_FFI=1
else
MICROPYTHON_MAKE_OPTS += MICROPY_PY_FFI=0
endif
endif

define MICROPYTHON_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/mpy-cross
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/ports/unix \
		$(MICROPYTHON_MAKE_OPTS)
endef

define MICROPYTHON_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/ports/unix \
		$(MICROPYTHON_MAKE_OPTS) \
		DESTDIR=$(TARGET_DIR) \
		PREFIX=/usr \
		install
endef

ifeq ($(BR2_PACKAGE_MICROPYTHON_LIB),y)
define MICROPYTHON_COLLECT_LIBS
	$(EXTRA_ENV) PYTHONPATH=$(@D)/tools \
		package/micropython/collect_micropython_lib.py \
		$(@D) $(@D)/.built_pylib \
		$(if $(BR2_PACKAGE_MICROPYTHON_LIB_UNIXFFI),--ffi,--no-ffi)
endef

define MICROPYTHON_INSTALL_LIBS
	rm -rf $(TARGET_DIR)/usr/lib/micropython
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/lib/micropython
	cp -a $(@D)/.built_pylib/* $(TARGET_DIR)/usr/lib/micropython
endef

MICROPYTHON_POST_BUILD_HOOKS += MICROPYTHON_COLLECT_LIBS
MICROPYTHON_POST_INSTALL_TARGET_HOOKS += MICROPYTHON_INSTALL_LIBS
endif

$(eval $(generic-package))
