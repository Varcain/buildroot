# Native Linux versus LXP + oveRTOS

## Current parity-closure status

The current image closes the XIP reset, SD-clock, and physical latency-path
gaps. It boots Linux 5.15.211 deterministically at 216 MHz from QSPI, requests
a 2 MHz SD clock (1.95 MHz actual), and continuously generates the same scope
signals as LXP: Arduino D3/PB4/TIM3_CH1 is a hardware 1 kHz reference with a
50 us high pulse, while the priority-99 Linux kernel response thread raises
Arduino D4/PG7 around the identical 512-iteration calculation.

The definitive parity run is not yet admissible. The existing FAT data volume
reports an allocation entry beyond EOF and Linux remounts it read-only before
the workload starts. Repairing the card is intentionally waiting for explicit
approval. No USB oscilloscope was enumerated, so the D3/D4 instrument capture
also remains to be attached. Exact current state and artifact hashes are in
`parity-status-20260814.json`.

A 300-second 216 MHz physical-scope diagnostic completed the SQL, network,
LVGL, and scope workloads before the 2 MHz change. It recorded 147 SQLite
transactions, 19,267,584 network bytes, 55 active LVGL samples, 302,367 scope
releases, 276,791 executions, and 25,575 misses. Its observer triggered one
OOM before the measurement boundary and its old scope-read timestamp included
capture delay, so it is evidence of end-to-end operation, not an accepted
comparison result. Both harness defects are fixed in commit `540362df91`.

## Historical accepted native result

The native Linux shell workload passed on hardware on 2026-08-14. The exact
machine-readable result is:

`output-hammer-qspi-xip/hammer-results/native-linux-strict300-embedded-final-300s-20260814/result.json`

Raw evidence in `output-hammer-qspi-xip/hammer-results/`:

- `native-linux-strict300-boot-20260814.log`: U-Boot, full Linux boot, login,
  identities, mounts, devices, addresses, and embedded binary hashes;
- `native-linux-strict300-embedded-final-300s-20260814.log`: the definitive
  unedited serial benchmark log and return status;
- `native-linux-strict300-postrun-20260814.log`: cold integrity `ok`, no
  FAT/MMC/I/O error, and a
  successful sync/unmount;
- `native-linux-strict300-smoke-30s-20260814.log`: the prior
  passing smoke run.

The accepted full QSPI image SHA-256 is
`4821e9b3c7fff933f86d1a995b92259389c953d516ceaa75c696fff640888ac2`.
It contains Linux 5.15.211, the QSPI-XIP kernel at `0x90100000`, and the
read-only linear CramFS root at `0x90800000`; U-Boot 2026.07 remains in
internal flash. The recorded kernel configuration is `CONFIG_PREEMPT_NONE`.
The boot log identifies the kernel build as Buildroot
`2026.05-740-gc7a58ca96d-dirty`; the result JSON records the exact source
commit, dirty paths, configuration hashes, and image hashes that were present
for the run. The final source changes are committed separately after hardware
validation.

Root mounted read-only at 2.657 s with 6,712 KiB of 8 MiB reported available
by the kernel; Ethernet linked at 6.108 s. This historical image had an
alternating warm-reset failure. The current early-XIP stack-protector fix
removes that failure and has passed consecutive warm boots.

## Five-minute measurements

The freshest successful clean-identity FreeRTOS shell result matching the
current strict workload/result contract is used as the primary LXP reference:

`/home/varcain/projects/private/hIRoic/oveRTOS/output/hammer-full-clean-sd-20260812/freertos-shell.json`

It reports `FreeRTOS V11.2.0 ove-fa0ec69 lxp-1fa50f8`, 300 seconds, one
completed error-free stream, the exact SQLite row/meta/live-row invariants,
and zero workload failures. The older complete nine-run 2 MHz comparison at
`output/language-hammer-20260811-final-2mhz/comparison.json` remains useful
background. The primary reference JSON does not itself encode SD clock, so
the Linux 24 MHz result must not be called storage-parity even though it uses
the same benchmark lineage and physical card setup.

| Metric | Native Linux | FreeRTOS + LXP reference |
| --- | ---: | ---: |
| Result | PASS | PASS |
| Timed workload | 300 s | 300 s |
| SQLite transactions | 94 | 164 |
| SQLite elapsed / rate | 300.83 s / 0.3125 tx/s | 300.0 s / 0.5467 tx/s |
| Rows / meta / live rows | 752 / 752 / 128 | 1312 / 1312 / 128 |
| SQLite retries / errors / integrity | 0 / 0 bytes / ok | 0 / 0 bytes / ok |
| Network | 12,976,128 bytes / 0.3457 Mbit/s | 52,363,264 bytes / 1.3945 Mbit/s |
| Network elapsed / completed / errors | 300.297 s / 1 / 0 | 300.397 s / 1 / 0 |
| Active LVGL samples | 86 | 617 |
| FPS mean / median / p99 | 2.221 / 2 / 3 | 3.290 / 3 / 4 |
| Render mean / median / p99 | 345.03 / 333 / 475 ms | 241.58 / 207 / 438 ms |
| Flush mean / median / p99 | 21.35 / 16 / 57 ms | 7.02 / 7 / 12 ms |
| Reported LVGL CPU mean | 100% | 98.55% |
| Overall Linux CPU busy | 99.994% | not reported on the same basis |
| Latency releases / executions / missed | 300,000 / 275,091 / 24,909 | 304,929 / 304,929 / 0 |
| Late finishes | 17,739 | 0 |
| Dispatch avg / p99 / p99.9 / max | 607.0 us / <=1 ms / <=1 ms / 103.258 ms | 9.611 us / <=20 us / <=100 us / 96.019 us |

Raw deltas are directional only. Linux delivered 42.8% fewer SQLite
transactions/s, 75.2% less network throughput, and 32.5% lower mean FPS. Its
mean render time was 42.8% higher and mean flush time 204.1% higher. Those
deltas combine scheduler, driver, SD-clock, rendering, and latency-load
differences and must not be attributed to the kernel alone.

The Linux storage snapshots show five read sectors and 20,254 written sectors
(9.89 MiB) during the measured interval. VFAT net allocation grew by 23 8 KiB
clusters. Linux root had zero block I/O because it was the QSPI linear CramFS.
The run ended with no filesystem warning and a clean `/data` unmount.

At the initial Linux snapshot, `linux-rt-latency` accounted for approximately
62% CPU and software-rendered lvmusic for 27%; the CPU had 0% sampled idle.
After the latency duration ended, lvmusic accounted for approximately 79%.

## Parity gaps and directional bias

| Gap | Likely bias | Consequence |
| --- | --- | --- |
| Current Linux SD cap is 2 MHz and hardware reports 1.95 MHz; the selected historical LXP set is 2 MHz | Small residual, indeterminate | The former 24 MHz Linux-favouring clock gap is closed; controller rounding is documented. |
| LXP uses DMA2D draw and framebuffer blit; Linux uses software LVGL drawing and fbdev `pwrite` | LXP | Linux spends much more CPU and wall time rendering/flushing. |
| Linux syscalls are native; LXP crosses its guest/personality/coordinator path | Linux | Syscall-heavy work may look better on Linux. |
| Both roots use QSPI and data uses SD/FAT, but Linux uses XIP CramFS while LXP uses its personality CPIO/rootfs path | Indeterminate | Root instruction/data fetch and cache pressure differ even though neither root writes SD. |
| Linux uses PL180 PIO; LXP storage engines use different SDMMC/DMA and transfer policies | Indeterminate | Bus clock and FAT are aligned, but block timing and CPU cost remain driver-specific. |
| Both systems use TIM3/PB4 CH1 and PG7 CH2 with the same timer phase and calculation; Linux uses a priority-99 kernel thread while oveRTOS uses its portable critical host task | Indeterminate | The physical quantity is now aligned, but the scheduler abstraction being measured remains intentionally system-native. |
| Linux baseline is `PREEMPT_NONE`; oveRTOS schedules the response at critical priority | LXP for Linux dispatch tails | Report baseline first. The separate `CONFIG_PREEMPT` profile is implemented and configuration-validated, but still needs its own hardware run. |
| Linux drives Play through uinput while LXP writes its extended input node | None expected | Both reached the active player scene, but injection paths differ. |
| Linux keeps SQLite in one process with a fixed MEMSYS5 heap; the reference shell uses its established personality-side execution path | Indeterminate | SQL, durability, transaction boundaries, and validations match, but process/allocation overhead does not. |
| Host SSH agent was absent | None to workload | Serial captured the run; Dropbear jump-host administration was configured but not authenticated in this session. |
| The data card currently has a FAT allocation inconsistency | Neither implementation | No new comparison is valid until an approved repair and read-only integrity check succeed. |
| No oscilloscope instrument is attached/enumerated | Neither implementation | The kernel generates D3/D4 continuously and reports software metrics, but a saved two-channel trace still requires the physical probe. |

The old numerical table above remains a historical software-timer/24 MHz
result and must not be relabelled. The next table must be regenerated only
from a clean five-minute 2 MHz run using `/proc/rt_scope`, followed by the
separately identified PREEMPT run.
