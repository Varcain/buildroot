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

Three equivalent stress drivers are installed for cross-engine comparisons:

  /usr/libexec/ove-hammer-shell [seconds]
  /usr/libexec/ove-hammer.lua [seconds]
  /usr/libexec/ove-hammer-micropython [seconds]

All run lvmusic at nice -5, an HTTP receive stream at nice 10, and the
SQLite/SD transaction workload at nice 0. They wait for the music-demo intro,
inject the Play touch, use DELETE journaling with FULL synchronization and the
target-wide file-backed temporary-storage policy, retain 128 live rows, VACUUM
every 20 transactions, and print delimited /proc/rt_scope and /proc/lxp_fs
snapshots. The shell driver uses wget/sqlite3. The Lua driver uses LuaSocket and
LuaDBI directly from its controller, avoiding a persistent database-worker
process slot; only VACUUM runs in a short-lived sqlite3 process because the Lua
runtime and VACUUM's complete temporary image do not fit together in one FDPIC
process arena. The MicroPython driver writes Linux evdev input_event records
directly to inject Play, uses its native socket module in the nice-10 network
worker, and uses the built-in sqlite3 module in the controller. Transactions
and VACUUM therefore never spawn sqlite3. Its launcher gives the controller a
bounded 96 KiB MicroPython heap, which leaves enough of its FDPIC process region
for SQLite's VACUUM working set. The default duration is 300 s. All stop SQLite
at a completed transaction boundary instead of terminating it during FAT I/O,
so the resulting database remains recoverable and auditable.

The built-in MicroPython sqlite3 API is intentionally small and bounded for
the NOMMU target:

  import sqlite3
  database = sqlite3.connect("/data/example.db")
  database.execute("CREATE TABLE IF NOT EXISTS values(n INTEGER);")
  database.execute("INSERT INTO values VALUES(42);")
  value = database.scalar("SELECT max(n) FROM values")
  database.close()

execute() accepts one or more SQL statements and returns the connection.
scalar() returns the first column of the first row as None, int, float, str or
bytes; string and blob results are limited to 255 bytes. The API deliberately
has no cursor or parameter-binding surface. Connections use SQLite FULLMUTEX,
a 30 s busy timeout, and release the MicroPython GIL while SQLite blocks.

When starting a stress driver as an SSH remote command, prefix it with the Hush
exec builtin. Replacing the otherwise retained remote-command shell returns one
process slot to the finite NOMMU pool while the controller, lvmusic and network
worker are co-resident, for example:

  ssh root@TARGET 'exec /usr/libexec/ove-hammer-micropython 300'
