oveRTOS Linux-personality rootfs board.

A uClinux/FDPIC userspace (uClibc-ng, BusyBox, shared libraries) for the
oveRTOS Linux syscall personality. We consume only output/images/rootfs.cpio
and the FDPIC toolchain — no Linux kernel image is built.

  make overtos_fdpic_defconfig
  make
  # -> output/images/rootfs.cpio (flashed to board QSPI, not committed)

The parallel Cortex-M7 hard-float FDPIC build uses a separate output directory
so it cannot contaminate the compatible soft-float sysroot:

  make O=output-hardfloat overtos_fdpic_hardfloat_defconfig
  make O=output-hardfloat
  # -> output-hardfloat/images/rootfs.cpio

Its post-build audit rejects a non-FDPIC object, a soft-float object, an object
without FPv5-SP attributes, or one that does not pass arguments in VFP
registers. /usr/bin/fpcheck validates s0-s31 and FPSCR at runtime.

busybox.config is the applet set; enable applets here as the personality grows
to support them.
