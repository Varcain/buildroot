# Native Linux versus LXP + oveRTOS

## Current status

The native image and runner are complete, but no native-Linux hardware number
exists yet.  At the final host check there was no ST-LINK VCP, USB device, or
removable SD device, and no SSH agent was available for the Pi.  Consequently,
all Linux benchmark cells remain `NOT_RUN`; manufacturing deltas from the LXP
numbers would be misleading.

The primary reference is the freshest successful 300-second shell result that
implements the current completion/error and row/meta/live-row contract:

`/home/varcain/projects/private/hIRoic/oveRTOS/output/zephyr-single-sector-full-20260812/zephyr-shell.json`

It reports `Linux overtos 6.1.0 Zephyr 4.4.0 ove-bf9a39c-dirty
lxp-1fa50f8`.  The result itself is `PASS`, but the dirty oveRTOS identity is a
provenance gap.  It is therefore a current-contract reference, not a pristine
release benchmark.

| Metric | Zephyr + LXP shell reference | Native Linux baseline |
| --- | ---: | ---: |
| Duration | 300 s | `NOT_RUN` |
| SQLite transactions | 80 | `NOT_RUN` |
| SQLite elapsed / rate | 313.0 s / 0.2556 tx/s | `NOT_RUN` |
| Rows / meta / live rows | 640 / 640 / 128 | `NOT_RUN` |
| Network | 65,470,464 bytes, 1.7453 Mbit/s | `NOT_RUN` |
| Active LVGL samples | 783 | `NOT_RUN` |
| FPS mean / median | 2.867 / 3 | `NOT_RUN` |
| Render mean | 191.83 ms | `NOT_RUN` |
| Flush mean | 14.58 ms | `NOT_RUN` |
| LVGL CPU mean | 99.75% | `NOT_RUN` |
| RT releases / executions / missed | 319,235 / 319,235 / 0 | `NOT_RUN` |
| RT lifetime avg / p99 / p99.9 / max | 9.648 us / <=32 us / <=50 us / 77.167 us | `NOT_RUN` and different semantics |

The last complete clean-identity nine-run set remains useful background:

`/home/varcain/projects/private/hIRoic/oveRTOS/output/language-hammer-20260811-final-2mhz/comparison.json`

Its shell rows were all 300-second `PASS` results:

| Engine | SQLite tx | Network Mbit/s | FPS mean | Render mean ms | Flush mean ms | Missed releases | RT lifetime max us |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| FreeRTOS | 133 | 1.402 | 3.414 | 224.94 | 7.13 | 0 | 96.833 |
| NuttX | 110 | 2.064 | 2.802 | 198.09 | 13.54 | 0 | 42.259 |
| Zephyr | 62 | 1.342 | 2.878 | 192.06 | 14.41 | 0 | 76.278 |

These older network records predate the stricter server completion fields, so
the nine-run set is background rather than the primary current-contract row.

## Parity assessment

The following gaps must accompany any raw deltas:

| Gap | Likely bias | Consequence |
| --- | --- | --- |
| LXP uses its DMA2D draw unit and DMA2D framebuffer blit; native Linux is software draw plus fbdev `pwrite` | LXP | Display, CPU, SQLite, and network contention can all look better for LXP. |
| Native Linux runs syscalls directly; LXP crosses the guest/personality/coordinator path | Linux | Syscall-heavy SQLite and network work can look better for Linux. |
| Both use FAT `/data` and a 2 MHz, four-bit SD bus, but Linux may issue multiblock I/O while the current Zephyr reference deliberately uses bounded single-sector transfers | Linux for bulk throughput; durability effect indeterminate | Storage rates are not implementation-identical even with media, frequency, and SQLite pragmas held constant. |
| Linux root and `/data` share the SD card; LXP loads its root from QSPI and uses SD for `/data` | LXP if Linux performs residual root I/O | The Linux root is read-only and runtime files are tmpfs to minimize this difference. |
| Linux latency is absolute-clock timer-to-userspace `SCHED_FIFO`; oveRTOS measures TIM3 hardware release through IRQ/event dispatch to a host task and exposes CH1/CH2 | Linux path is shorter, while `PREEMPT_NONE` can bias Linux tails worse | Do not compare latency values as the same physical measurement. |
| The Linux baseline is `CONFIG_PREEMPT_NONE`; the oveRTOS host response runs at its critical priority | LXP for dispatch tails | A later `CONFIG_PREEMPT` Linux run must be a separate result, never folded into baseline. |
| Linux has 8 MiB SDRAM minus a 512 KiB DMA pool; LXP/oveRTOS has a different host/guest memory partition | Indeterminate | Available-memory and cache-pressure results need to be reported with each run. |
| Play is injected through Linux uinput instead of LXP's writable physical event node | None expected after activation | Verify the same active scene from performance logs before accepting a run. |
| The fresh Zephyr reference identifies a dirty oveRTOS tree | Indeterminate | Prefer a new clean-identity LXP shell run when the native hardware session is repeated. |

The raw SQLite, network, and LVGL values are still useful observations of each
complete system.  A claim of scheduler parity requires the Linux TIM3/PB4 and
PG7 kernel implementation plus a two-channel scope capture; the userspace PoC
is intentionally labelled weaker.

