# Native Linux versus LXP + oveRTOS

## Accepted native-Linux result

The baseline `CONFIG_PREEMPT_NONE` run passed the complete 300-second shell
contract on 2026-08-14. Its normalized result, exact configurations, deployed
artifact hashes, raw serial capture, and boot log are in:

`output-hammer-qspi-xip-maintenance/hammer-results/native-linux-pio-poll-scope-v5-final-300s-v2-20260814/`

The machine-readable comparison is `comparison.json`. Linux 5.15.211 build
`#19`, U-Boot 2026.07, and Buildroot `9688027a8f` ran at the board's 216 MHz
clock. U-Boot remained in internal flash; the kernel executed from QSPI at
`0x90100000`, and the read-only CramFS root executed from QSPI at
`0x90800000`. The SD card was not in the boot path. `/dev/mmcblk0p1` was the
data-only VFAT volume at `/data`.

The kernel reported 6,712 KiB of 8 MiB available at boot, mounted the root at
3.442 seconds, discovered the 14.9 GiB SD card by 3.870 seconds, mounted the
repaired active FAT at 5.323 seconds, and initialized Ethernet at 6.423
seconds. `MemFree` was 1,100 KiB at the measurement boundary. The display was
`stmdrmfb`, touch was `generic ft5x06`, and Play activation used `/dev/uinput`.

## Primary LXP reference

The freshest successful clean-identity FreeRTOS shell result matching the
strict workload and validation contract is:

`/home/varcain/projects/private/hIRoic/oveRTOS/output/hammer-full-clean-sd-20260812/freertos-shell.json`

It identifies `FreeRTOS V11.2.0 ove-fa0ec69 lxp-1fa50f8`, runs for 300 seconds,
uses the same SQL and validation rules, completes one error-free stream, and
reports the TIM3 scope. Its JSON does not independently encode the SD clock.
The immediately preceding complete nine-run lineage is explicitly identified
as `language-hammer-20260811-final-2mhz`; this is supporting clock evidence,
not a reason to hide the missing field in the primary result.

## Five-minute measurements

| Metric | Native Linux | FreeRTOS + LXP |
| --- | ---: | ---: |
| Result | PASS | PASS |
| Timed workload | 300 s | 300 s |
| SQLite transactions | 83 | 164 |
| SQLite elapsed / rate | 302.07 s / 0.2748 tx/s | 300.0 s / 0.5467 tx/s |
| Rows / meta / live | 664 / 664 / 128 | 1312 / 1312 / 128 |
| SQLite retries / error bytes / integrity | 0 / 0 / ok | 0 / 0 / ok |
| Network | 15,597,568 bytes / 0.4154 Mbit/s | 52,363,264 bytes / 1.3945 Mbit/s |
| Network elapsed / completed / errors | 300.382 s / 1 / 0 | 300.397 s / 1 / 0 |
| Active LVGL samples | 57 | 617 |
| FPS mean / median / p99 | 1.000 / 1 / 1 | 3.290 / 3 / 4 |
| Render mean / median / p99 | 822.25 / 833 / 889 ms | 241.58 / 207 / 438 ms |
| Flush mean / median / p99 | 63.00 / 64 / 76 ms | 7.02 / 7 / 12 ms |
| Reported LVGL CPU mean | 100% | 98.55% |
| Scope releases / executions / missed | 302,404 / 268,598 / 33,805 | 304,929 / 304,929 / 0 |
| Missed / late finish rate | 11.18% / 1.81% | 0% / 0% |
| Dispatch avg / p99 / p99.9 / max | 448.3 us / <=1 ms / <=1 ms / 999.870 us | 9.611 us / <=20 us / <=100 us / 96.019 us |

Relative to that LXP reference, Linux delivered 49.7% fewer SQLite
transactions per second, 70.2% less network throughput, and 69.6% lower mean
FPS. Linux mean render time was 240.4% higher and mean flush time was 797.3%
higher. These are end-to-end platform results; they must not be attributed to
the kernel alone because the rendering and SD transfer engines differ.

The Linux `/data` block delta was five reads/2,560 bytes and 4,769 writes/
9,180,160 bytes. VFAT remained read-write before and after the run, SQLite
integrity was `ok`, and the kernel logged no block, FAT, OOM, or oops failure.
Overall sampled CPU busy time was 100%. The scope thread accounted for 41.97%,
lvmusic 28.37%, SQLite 4.14%, and the resident runner 3.76%; the terminated
network child and interrupt work explain part of the remainder.

## Physical latency contract

Both systems generate the same two-channel physical contract:

- oscilloscope CH1: Arduino **D3**, MCU **PB4/TIM3_CH1**, 1 kHz period and a
  hardware-generated 50 us high pulse;
- oscilloscope CH2: Arduino **D4**, MCU **PG7**, high around the same fixed
  512-iteration calculation executed by the highest-priority scheduled work.

These are Arduino D3/D4 connector names, not MCU pins PD3/PD4. Linux uses IRQ
29 to wake a priority-99 `SCHED_FIFO` kernel thread; oveRTOS uses its portable
critical-priority host task. The Linux driver reports an internally consistent
302.404-second window. One release was pending at the atomic snapshot, so
`executions + misses + pending == releases`.

Both Linux waveforms are continuously present for an attached oscilloscope,
but no `/dev/usbtmc`, serial scope, or other instrument interface is available
to archive a trace automatically. A photographed/manual observation can prove
the external waveform but is not silently substituted for machine-captured
evidence.

## Remaining parity gaps and bias

| Gap | Likely bias | Consequence |
| --- | --- | --- |
| LXP uses DMA2D draw/blit; Linux uses software drawing and fbdev `pwrite` | LXP | Linux spends substantially more CPU and wall time rendering and flushing. |
| Linux uses one-sector, CPU-polled PL180 PIO writes; LXP uses its engine-specific SDMMC/DMA path | LXP | Linux holds the only CPU while filling each write FIFO; this affects GUI, network, SQL, and dispatch simultaneously. |
| Linux records 1.95 MHz actual; the primary LXP JSON omits its SD clock while the preceding lineage is explicitly 2 MHz | Indeterminate | Clock parity is strongly indicated but not self-attested by both primary JSON files. |
| Linux syscalls are native; LXP crosses the guest/personality/coordinator path | Linux | Syscall-heavy operations may favour Linux. |
| Linux root is XIP CramFS; LXP uses its QSPI personality CPIO/rootfs | Indeterminate | Instruction fetch, file lookup, and cache pressure differ; neither root writes the SD card. |
| Linux uses a FIFO kernel thread on `PREEMPT_NONE`; oveRTOS uses a critical host task | LXP for Linux tails | The pins and calculation match, but the scheduler objects remain system-native. |
| Linux uses a persistent fixed-heap SQLite worker | Indeterminate | SQL, durability, transaction boundaries, and validation match, but allocation/process overhead does not. |
| No instrument trace is archived | None | Software counters and pin generation are complete; an external CH1/CH2 capture remains physical evidence. |
| Pi local SSH rejects the available agent key | None | Serial produced the accepted result; SSH administration needs a Pi `authorized_keys` update, not a Linux/QSPI change. |

The repaired FAT blocker, 24 MHz Linux storage advantage, software-only
latency measurement, XIP warm-reset failure, and missing D3/D4 signal generator
are closed. A `CONFIG_PREEMPT` build remains an optional, separately identified
follow-up; it must not replace or be mixed with this requested baseline.
