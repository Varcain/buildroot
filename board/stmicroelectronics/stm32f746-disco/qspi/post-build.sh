#!/bin/sh
set -eu

# The kernel and DTB live in raw QSPI slots and must not also consume space in
# the read-only root filesystem.
rm -rf "${TARGET_DIR}/boot"

# FDPIC insmod maps the complete module file before relocation. Buildroot does
# not strip these ARM no-MMU modules by default, leaving a 13 KiB MMCI module
# as a 400+ KiB file and recreating the order-7 allocation failure. Keep the
# relocation/symbol tables required by the loader and remove debug sections.
if [ -d "${TARGET_DIR}/lib/modules" ]; then
	find "${TARGET_DIR}/lib/modules" -type f -name '*.ko' -exec \
		"${HOST_DIR}/bin/arm-buildroot-uclinuxfdpiceabi-strip" \
		--strip-debug {} +
fi
