oveRTOS Linux-personality rootfs board.

A uClinux/FDPIC userspace (uClibc-ng, BusyBox, shared libraries) for the
oveRTOS Linux syscall personality. We consume only output/images/rootfs.cpio
and the FDPIC toolchain — no Linux kernel image is built.

  make overtos_defconfig
  make
  # -> output/images/rootfs.cpio (flashed to board QSPI, not committed)

busybox.config is the applet set; enable applets here as the personality grows
to support them.
