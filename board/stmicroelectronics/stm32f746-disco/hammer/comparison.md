# Native Linux versus LXP + oveRTOS

## Accepted native-Linux result

The accepted PREEMPT_RT run passed the complete 300-second shell contract on
2026-08-15. Its normalized result, exact configuration and image hashes, raw
serial capture, clean boot log and FAT maintenance logs are under:

`output-hammer-iterations/08-preempt-rt-sd-dma-pfctrl/`

The machine-readable comparison is `comparison.json`, and the compact iteration
record is `results/iterations/08-preempt-rt-sd-dma-pfctrl.json`. Linux
5.15.211-rt97 build `#6 PREEMPT_RT`, U-Boot 2026.07 and Buildroot 2026.08-git
ran at 216 MHz. U-Boot remained in internal flash. The kernel executed directly
from QSPI and mounted a read-only QSPI CramFS root; the SD card was not in the
boot path. `/dev/mmcblk0p1` was the data-only VFAT volume at `/data`.

The kernel reported 6,708 KiB of 8 MiB available, mounted root at 6.400 seconds
and started init at 6.425 seconds. At the measurement boundary, `MemFree` was
468 KiB and `MemAvailable` was 1,524 KiB. The display was `stmdrmfb`, touch was
`generic ft5x06`, and Play activation used `/dev/uinput`.

This result closes the earlier Linux SD-transfer mismatch. MMCI uses the same
STM32 DMA2 streams 3 and 6, channel 4, full FIFO, INC4 and peripheral flow
control as the validated oveRTOS implementation, while retaining the common
one-sector request limit and approximately 1.95 MHz clock. The serial boot log
shows both DMA channels and no DMA-fallback message.

## Primary LXP reference

The freshest successful clean-identity FreeRTOS shell result matching the
strict workload and validation contract is:

`/home/varcain/projects/private/hIRoic/oveRTOS/output/hammer-full-clean-sd-20260812/freertos-shell.json`

It identifies `FreeRTOS V11.2.0 ove-fa0ec69 lxp-1fa50f8`, runs for 300 seconds,
uses the same SQL and validation rules, completes one error-free stream, and
reports the TIM3 scope. Its JSON does not independently encode the SD clock.
The immediately preceding nine-run lineage is explicitly identified as
`language-hammer-20260811-final-2mhz`; that is supporting evidence, not a
substitute for clock attestation in the primary result.

## Five-minute measurements

| Metric | Native Linux PREEMPT_RT | FreeRTOS + LXP |
| --- | ---: | ---: |
| Result | PASS | PASS |
| Timed workload | 300 s | 300 s |
| SQLite transactions | 60 | 164 |
| SQLite elapsed / rate | 321.64 s / 0.1865 tx/s | 300.0 s / 0.5467 tx/s |
| Rows / meta / live | 480 / 480 / 128 | 1312 / 1312 / 128 |
| SQLite retries / error bytes / integrity | 0 / 0 / ok | 0 / 0 / ok |
| Network | 5,439,488 bytes / 0.1439 Mbit/s | 52,363,264 bytes / 1.3945 Mbit/s |
| Network elapsed / completed / errors | 302.454 s / 1 / 0 | 300.397 s / 1 / 0 |
| Active LVGL samples | 72 | 617 |
| FPS mean / median / p99 | 1.000 / 1 / 1 | 3.290 / 3 / 4 |
| Render mean / median / p99 | 745.26 / 736 / 862 ms | 241.58 / 207 / 438 ms |
| Flush mean / median / p99 | 56.44 / 55 / 72 ms | 7.02 / 7 / 12 ms |
| Reported LVGL CPU mean | 100% | 98.55% |
| Scope releases / executions / missed | 322,106 / 320,410 / 1,696 | 304,929 / 304,929 / 0 |
| Missed / late finish rate | 0.5265% / 0.0255% | 0% / 0% |
| Dispatch avg / p99 / p99.9 / max | 420.2 us / <=1 ms / <=1 ms / 999.759 us | 9.611 us / <=20 us / <=100 us / 96.019 us |

Relative to that LXP reference, Linux delivered 65.9% fewer SQLite
transactions per second, 89.7% less network throughput and 69.6% lower mean
FPS. Linux mean render time was 208.5% higher and mean flush time was 703.9%
higher. Its physical dispatch mean was 43.7 times the LXP after-snapshot mean,
with 0.5265% missed releases versus zero. These are end-to-end platform
results, not a kernel-only attribution: the rendering path remains materially
different.

The Linux `/data` block delta was 332 reads/1,340,928 bytes and 3,566
writes/6,651,392 bytes. SQLite integrity was `ok`; the result parser reported
no failures, early EOF, network error or SQLite error. Overall sampled CPU busy
time was 100%. The scope thread accounted for 30.92%, lvmusic 25.37%, SQLite
4.13%, and the resident runner 3.44% by itself or 4.46% with reaped children.
Interrupt and terminated network work account for part of the remainder.

Before the run, a clean-boot read-only dosfstools check found only the dirty
flag and boot-sector backup byte 65 mismatch. With `/data` unmounted, the
low-memory repair changed active FAT1 in place; it did not format, repartition
or touch QSPI. A full read-only follow-up passed (`12 files, 65/1948688
clusters`, return code zero), followed by a reboot with no dirty-volume warning.
After the accepted run, `/data` was synced and cleanly unmounted.

## What PREEMPT and PREEMPT_RT changed

| Iteration | Kernel/storage change | Result | SQLite tx/s | Network Mbit/s | Scope missed |
| --- | --- | --- | ---: | ---: | ---: |
| 1 | PREEMPT_NONE, fast physical-scope path, PIO SD | PASS | 0.3166 | 0.4471 | 11.658% |
| 2 | CONFIG_PREEMPT | PASS | 0.2614 | 0.2322 | 7.487% |
| 3 | PREEMPT_RT | Invalid smoke | — | — | unloaded only |
| 4 | PREEMPT_RT, threaded MMCI PIO | Storage corruption | — | — | — |
| 5 | PREEMPT_RT, hard-IRQ MMCI PIO | Storage stall | — | — | — |
| 6 | PREEMPT_RT, full-FIFO SD DMA | DMA fallback | — | — | — |
| 7 | PREEMPT_RT, direct-mode SD DMA | DMA fallback | — | — | — |
| 8 | PREEMPT_RT, full-FIFO SD DMA + PFCTRL | PASS | 0.1865 | 0.1439 | 0.527% |

The accepted PREEMPT_RT result reduces the missed-release rate by 95.5% and
the late-finish rate by 84.7% relative to iteration 1, while SQLite throughput
falls 41.1% and network throughput falls 67.8%. Its average dispatch latency is
3.3% higher, because far fewer releases are dropped but the executed jobs still
wait hundreds of microseconds. Iterations 3 through 7 could not run the full
contract, so iteration 8 measures the cumulative scheduler, storage and
bounded-allocation corrections. It does not isolate PFCTRL performance.

## Syscall-path comparison

"Native Linux syscalls" means the FDPIC application enters its own Linux
kernel directly through the ARM syscall ABI. Under LXP, the Linux-ABI guest
request enters the LXP personality and is dispatched through its coordinator
to the host-side implementation before returning to the guest. That is a real
boundary and includes dispatch, argument handling, synchronization and the
host operation; it does not imply that every call is expensive enough to
dominate this benchmark.

The LXP result counted 171,745 SVC calls during the interval and reported a
cumulative after-snapshot average of 5,260 cycles at 216 MHz. Multiplying those
figures gives an order-of-magnitude estimate of 4.18 seconds, or 1.39% of one
CPU over 300 seconds. This is not exact accounting: the call count is an
interval delta, the average is cumulative, and coordinator work may occur
outside the timed SVC body. It nevertheless establishes scale. The crossing
cannot plausibly explain LXP being 2.9 times faster in SQLite, 9.7 times faster
on the network stream and 3.3 times faster in FPS. The syscall path is therefore
a small or indeterminate bias, probably in Linux's favour, not a primary
explanation for the result.

## Physical latency contract

Both systems generate the same two-channel physical contract:

- oscilloscope CH1: Arduino **D3**, MCU **PB4/TIM3_CH1**, 1 kHz period and a
  hardware-generated 50 us high pulse;
- oscilloscope CH2: Arduino **D4**, MCU **PG7**, high around the same fixed
  512-iteration calculation executed by the highest-priority scheduled work.

These are Arduino D3/D4 connector names, not MCU pins PD3/PD4. Linux uses IRQ
29 to wake a priority-99 `SCHED_FIFO` kernel thread; oveRTOS uses its portable
critical-priority host task. The Linux counters are internally consistent:
`executions + missed == releases` at the atomic snapshot.

Both Linux waveforms are continuously present for an attached oscilloscope,
but no `/dev/usbtmc`, serial scope or other instrument interface was available
to archive an external trace. The physical generator and software counters are
implemented; an external capture remains evidence to collect, not a missing
signal implementation.

## Remaining parity gaps and bias

| Gap | Likely bias | Consequence |
| --- | --- | --- |
| LXP uses DMA2D draw/blit; Linux uses software drawing and fbdev `pwrite` | LXP | Linux spends substantially more CPU and wall time rendering and flushing. |
| Linux and LXP now use matching STM32 SD DMA resources and PFCTRL | Substantially closed | Driver structure/accounting still differ, but Linux no longer holds the CPU in polled PIO. |
| Linux records 1.95 MHz actual; the primary LXP JSON omits its SD clock while the preceding lineage is explicitly 2 MHz | Indeterminate | Clock parity is strongly indicated but not self-attested by both primary JSON files. |
| Linux syscalls are native; LXP crosses the guest/personality/coordinator boundary | Small/indeterminate, likely Linux | Measured SVC timing suggests roughly 1.4% of one CPU, far below the end-to-end gaps. |
| Linux root is XIP CramFS; LXP uses its QSPI personality CPIO/rootfs | Indeterminate | Instruction fetch, lookup and memory pressure differ; neither database is on root. |
| Linux uses a FIFO kernel thread on PREEMPT_RT; oveRTOS uses a critical host task | LXP for measured latency | The pins and calculation match, but the scheduler objects remain system-native. |
| Linux uses a persistent 448 KiB fixed-heap SQLite worker | Indeterminate | SQL, durability, transaction boundaries and validation match; allocation/process overhead does not. |
| No external instrument trace is archived | None | Software counters and pin generation are complete; an external CH1/CH2 capture remains physical evidence. |
| Pi-to-board Dropbear stream corrupts after negotiation | None | Serial captured the accepted run; this affects administration, not workload results. |

The major remaining implementation parity task is native-Linux DMA2D rendering.
Until that is done, the GUI, CPU, network and SQLite totals are useful platform
measurements but not a controlled kernel-scheduler comparison.

## Exact board restoration

The accepted Linux state has U-Boot in internal flash, the native XIP kernel
and CramFS root in QSPI, and the benchmark FAT volume cleanly unmounted. QSPI
was not changed during the final FAT repair or benchmark.

To restore the FreeRTOS oveRTOS firmware in internal flash, first verify that
the intended launcher exists, then run from the read-only source repository:

```sh
test -x /home/varcain/projects/private/hIRoic/oveRTOS/output/stm32f746/freertos/linux_interop/flash
cd /home/varcain/projects/private/hIRoic/oveRTOS
output/stm32f746/freertos/linux_interop/flash
```

That replaces internal-flash U-Boot with oveRTOS firmware. Restoring the LXP
rootfs in QSPI is a separate destructive operation and should be done only
after explicit confirmation:

```sh
cd /home/varcain/projects/private/hIRoic/oveRTOS
boards/stm32f746g-discovery/freertos/flash-qspi.sh \
    /home/varcain/projects/private/hIRoic/buildroot/output/images/rootfs.cpio
```

The SD card does not need rewriting for LXP: its existing FAT partition is the
benchmark `/data` volume. For another native-Linux run, mount it with:

```sh
mount -t vfat -o rw,nosuid,nodev,noexec,noatime,flush,errors=remount-ro \
    /dev/mmcblk0p1 /data
```

Recreating a stock Buildroot boot SD image is destructive and unnecessary for
this QSPI-XIP configuration. If it is ever required, identify the exact
removable device with `lsblk`, unmount its partitions, obtain confirmation,
and only then write `output/images/sdcard.img` to that exact device.
