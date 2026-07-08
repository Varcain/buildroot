#!/bin/sh
# oveRTOS rootfs post-build. $1 = target dir.
#
# 1. Provide the PT_INTERP path the FDPIC binaries name (/usr/lib/ld.so.1) as a symlink to
#    the real loader, so the personality can resolve the interpreter by its named path.
# 2. Strip the debug-info-heavy uClibc + libgcc_s + busybox (they ship unstripped — no
#    global BR2_STRIP): QSPI has room for the rootfs, but avoid wasting it. dbgdemo
#    (compiled -g in step 3, AFTER the strip) keeps its source-level-debug symbols.
# 3. Compile the personality's test programs (sources in board/overtos/progs) into /usr/bin,
#    so the QSPI rootfs.cpio carries them for CI + on-target debugging (t_pth and
#    t_pmin exercise LinuxThreads; dbgdemo is built -g for source-level debug; segv is the
#    negative KERNEL-isolation test — a deliberate kernel-SRAM write, denied by the ARM default
#    map; xregion is the negative INTER-PROGRAM-isolation test — a write to a SIBLING's pool
#    region, denied by the privileged-only whole-pool base MPU region (Phase 2). Both must fault
#    and be contained).
set -e
TARGET="$1"
if [ -e "$TARGET/lib/ld-uClibc.so.0" ]; then
	mkdir -p "$TARGET/usr/lib"
	ln -sf /lib/ld-uClibc.so.0 "$TARGET/usr/lib/ld.so.1"
fi

STRIP="$HOST_DIR/bin/arm-buildroot-uclinuxfdpiceabi-strip"
if [ -x "$STRIP" ]; then
	"$STRIP" --strip-unneeded "$TARGET"/lib/libuClibc-*.so "$TARGET"/lib/libgcc_s.so.1 "$TARGET"/bin/busybox 2>/dev/null || true
fi

GCC="$HOST_DIR/bin/arm-buildroot-uclinuxfdpiceabi-gcc"
PROGS="$(CDPATH= cd -- "$(dirname -- "$0")/progs" 2>/dev/null && pwd)"
if [ -x "$GCC" ] && [ -n "$PROGS" ]; then
	mkdir -p "$TARGET/usr/bin"
	"$GCC" -mfdpic -O2 "$PROGS/t_pth.c" -o "$TARGET/usr/bin/t_pth" -pthread
	"$GCC" -mfdpic -O2 "$PROGS/t_pmin.c" -o "$TARGET/usr/bin/t_pmin" -pthread
	"$GCC" -mfdpic -g -O0 "$PROGS/dbgdemo.c" -o "$TARGET/usr/bin/dbgdemo"
	"$GCC" -mfdpic -O2 "$PROGS/segv.c" -o "$TARGET/usr/bin/segv"
	"$GCC" -mfdpic -O2 "$PROGS/xregion.c" -o "$TARGET/usr/bin/xregion"
	"$GCC" -mfdpic -O2 "$PROGS/kstress.c" -o "$TARGET/usr/bin/kstress"
	"$GCC" -mfdpic -O2 "$PROGS/lbench.c" -o "$TARGET/usr/bin/lbench"
	"$GCC" -mfdpic -O2 "$PROGS/fbtest.c" -o "$TARGET/usr/bin/fbtest"
	"$GCC" -mfdpic -O2 "$PROGS/evread.c" -o "$TARGET/usr/bin/evread"
	"$GCC" -mfdpic -O2 "$PROGS/nettest.c" -o "$TARGET/usr/bin/nettest"
fi
