oveRTOS Linux-personality rootfs board.

A uClinux/bFLT userspace (uClibc-ng, static, BusyBox) for the oveRTOS Linux
syscall personality. We consume only output/images/rootfs.cpio + the bFLT
toolchain — no Linux kernel image is built.

  make overtos_defconfig
  make
  # -> output/images/rootfs.cpio (embedded at oveRTOS build time, not committed)

busybox.config is the applet set; enable applets here as the personality grows
to support them.
