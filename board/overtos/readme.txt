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
every 20 transactions, and print delimited before/after /proc/rt_scope and
/proc/lxp_fs snapshots. Each driver waits until the HTTP response headers have
arrived (or, for wget, until the benchmark server reports an active stream)
before taking its before snapshots. The server owns the common requested
deadline and reports its actual elapsed time, which is the throughput
denominator. Database and VACUUM shadow files live in /data/.ove-hammer rather
than the FAT root directory, keeping temporary-file creation independent of
unrelated root-directory occupancy. The shell driver uses wget/sqlite3. The
Lua driver uses LuaSocket and
LuaDBI directly from its controller and writes Linux evdev input_event records
directly, avoiding persistent database and touch-helper process slots. Lua
releases its DBI connection and runs only VACUUM in a short-lived sqlite3
process: an isolated in-process VACUUM works, but under simultaneous LVGL and
network load it exhausts the shared 256 KiB FDPIC arena and can starve the
network stack. The MicroPython driver uses direct evdev, its native socket module in
the nice-10 network worker, and the built-in sqlite3 module in the controller.
Its built-in ove_process module launches and reaps lvmusic and the network
worker with posix_spawnp/waitpid, without transient Hush or nice processes.
Transactions and VACUUM therefore never spawn sqlite3. Its launcher gives the controller a
bounded 96 KiB MicroPython heap, which leaves enough of its FDPIC process region
for SQLite's VACUUM working set. The default duration is 300 s. All stop SQLite
at a completed transaction boundary instead of terminating it during FAT I/O,
so the resulting database remains recoverable and auditable. The result fields
have the same meanings in all three drivers: transactions is the number of
committed eight-row batches, rows is transactions times eight, meta is the
persisted cumulative row counter, and live_rows is the bounded events-table
cardinality. A valid result satisfies rows == meta == transactions * 8 and
live_rows == min(meta, 128). Network workers distinguish normal deadline EOF,
early EOF, and mid-stream errors, and every driver has bounded cleanup.

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
