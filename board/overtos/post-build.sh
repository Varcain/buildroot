#!/bin/sh
# oveRTOS rootfs post-build. $1 = target dir.
#
# 1. Drop libgcc_s.so* — the busybox/uClibc userspace links only libc.so.0; the 2.8 MB
#    libgcc blob is dead weight in the (firmware-embedded) rootfs.cpio.
# 2. Provide the PT_INTERP path the FDPIC binaries name (/usr/lib/ld.so.1) as a symlink to
#    the real loader, so the personality can resolve the interpreter by its named path.
set -e
TARGET="$1"
rm -f "$TARGET"/lib/libgcc_s.so* "$TARGET"/usr/lib/libgcc_s.so*
if [ -e "$TARGET/lib/ld-uClibc.so.0" ]; then
	mkdir -p "$TARGET/usr/lib"
	ln -sf /lib/ld-uClibc.so.0 "$TARGET/usr/lib/ld.so.1"
fi
