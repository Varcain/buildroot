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

The production shell is BusyBox Hush with every user-facing Hush feature
enabled (the internal HUSH_MEMLEAK debugger remains disabled). The Lua runtime
contains Lua 5.4, readline, LuaSocket, LuaDBI/SQLite, LuaFileSystem, cjson and
argparse. lua-ove-process supplies posix_spawnp/wait/kill/nice control without
the fork() dependency that makes upstream LuaPOSIX unsuitable for NOMMU.

Two equivalent stress drivers are installed for cross-engine comparisons:

  /usr/libexec/ove-hammer-shell [seconds]
  /usr/libexec/ove-hammer.lua [seconds]

Both run lvmusic at nice -5, an HTTP receive stream at nice 10, and the
SQLite/SD transaction workload at nice 0. They wait for the music-demo intro,
inject the Play touch, use DELETE/FULL/MEMORY SQLite pragmas, retain 128 live
rows, VACUUM every 20 transactions, and print delimited /proc/rt_scope and
/proc/lxp_fs snapshots. The shell driver uses wget/sqlite3; the Lua driver uses
LuaSocket/LuaDBI and the NOMMU process module. The default duration is 300 s.
Both stop SQLite at a completed transaction boundary instead of terminating it
during FAT I/O, so the resulting database remains recoverable and auditable.
