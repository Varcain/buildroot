# Native Linux hammer proof of concept

## Scope and frozen starting point

This proof of concept runs the existing five-minute STM32F746 hammer workload
on the upstream native-Linux board port, then compares it with the oveRTOS +
LXP result only where the physical and workload contracts match.

The starting point is the committed Buildroot tree at
`0e94f8d4de7b5421d5c0d21e22122a444c632770`.  At that revision the stock
`stm32f746_disco_sd_defconfig` selects Linux 5.15.211 and U-Boot 2026.07.  This
is newer than the 5.15.202/2026.04 combination recorded at handoff; result
manifests must report the versions actually built.  Its kernel fragment
explicitly disables `CONFIG_PREEMPT`.  That policy remains unchanged for the
baseline and the first hammer result.

The stock boot path writes U-Boot only to STM32 internal flash and loads the
kernel, device tree, and root filesystem from SD.  That was the initial
baseline.  The user subsequently authorized replacing QSPI to test a minimal
QSPI root and XIP kernel; every QSPI write therefore requires a full backup and
readback verification, and restoration remains part of the deliverable.

### Hardware-driven boot-plan revision

The user-directed QSPI experiment proved a full read-only QSPI-root boot and
changed the hammer feasibility decision.  A copied-to-RAM QSPI root left only
about 448 KiB free after display, touch, Ethernet, init, and Dropbear.  The
requested internal-flash XIP image is also 2,913,695 bytes versus less than
1 MiB of usable internal flash.

The revised hammer path is therefore:

1. retain U-Boot in internal flash;
2. execute kernel text and read-only data in place from QSPI;
3. keep the QSPI controller memory-mapped across the U-Boot-to-Linux handoff;
4. map an 8 MiB read-only linear CramFS window at QSPI `0x90800000` and execute
   eligible userspace text directly from it;
5. use SD only for a data-only VFAT partition at `/data`;
6. keep one dynamically linked SQLite worker alive, with a bounded 500 KiB
   MEMSYS5 heap, because repeated FDPIC exec/mapping cannot be made reliable
   under NOMMU fragmentation.

This deliberately replaces the current QSPI contents and requires a verified
full-range backup first.  Linux boot no longer requires SD.  The benchmark
still requires SD solely because parity requires persistent FAT `/data`.

## Execution status (2026-08-14)

Phases 1 through 6 are implemented for the `CONFIG_PREEMPT_NONE` baseline.
Serial, display, uinput automation, physical touch discovery, Ethernet, QSPI
XIP root, and VFAT mounting were verified on hardware. The XIP warm-reset bug
is fixed, and Linux now runs at the same 216 MHz board clock as oveRTOS. The
physical scope driver owns TIM3/PB4/PG7 safely and continuously generates the
same Arduino D3/D4 contract at a 54 MHz timer phase. The current SD profile
requests 2 MHz and the driver reports 1.95 MHz actual. A separate
`CONFIG_PREEMPT` follow-up profile has passed Kconfig validation.

Phase 7 is currently blocked before measurement: the existing FAT data volume
contains an allocation entry beyond EOF and Linux remounts it read-only. No
repair or format is authorized. The 300-second 216 MHz physical-scope
diagnostic proves all concurrent workers but is non-admissible because it used
the former 24 MHz profile and its old observer/timestamp logic. Dropbear and
the static address are configured, but the host had no SSH agent, so serial is
the proven administrative and log-capture path. No USB oscilloscope was
enumerated, so a saved two-channel instrument trace remains external work.

## Phases and acceptance gates

### 1. Freeze and build the native baseline

1. Run `make O=output stm32f746_disco_sd_defconfig` and save the initial
   Buildroot configuration before making PoC changes.
2. Build it with `make O=output -j"$(nproc)"`.
3. Archive Buildroot, Linux, U-Boot, toolchain, image hashes, and the final
   Buildroot and kernel configurations under a timestamped result directory.
4. Verify from the archived kernel configuration that preemption was not
   silently enabled.

Gate: the untouched upstream image builds, or its failure is captured with a
reproducible log before the derived configuration is introduced.

### 2. Native board image and access

Create a separate `stm32f746_disco_hammer_defconfig` so the stock board
configuration remains an inspectable baseline.  Add only the facilities needed
by the experiment:

- a known root console login and Dropbear without embedding a password or
  private key;
- UART console at 115200 8N1 with no flow control;
- static Ethernet address 172.1.1.2/24 and Pi/server address 172.1.1.1;
- procfs/sysfs and the diagnostic commands needed to record identity, memory,
  mounts, interrupts, networking, storage, processes, and CPU use;
- SQLite CLI, an HTTP receive client, and the native hammer utilities;
- a read-only XIP CramFS root in QSPI and a data-only VFAT `/data` partition on
  SD.

The root filesystem is mounted read-only. Runtime files belong in ramfs and
benchmark writes belong on `/data`. Root and data do not share a physical
medium in the final layout.

Gate: the generated image has the expected partition table, `/data` is VFAT,
and no credential secret exists in the repository or image.

### 3. Serial, SSH, and peripheral bring-up

Discover the ST-LINK VCP rather than assuming `/dev/ttyACM0`, capture the full
U-Boot and Linux log, and record `uname -a`, `/proc/cmdline`, `/proc/meminfo`,
`/proc/interrupts`, mounts, and addresses.  Verify Dropbear first over the
direct benchmark link and then with `ssh -J pi root@172.1.1.2`.

Probe and record the actual devices and drivers for:

- LTDC/framebuffer, its resolution, stride, pixel format, and whether rendering
  is software, framebuffer-assisted, or DMA2D-assisted;
- touchscreen/input coordinates and event capabilities;
- STM32 Ethernet link and error counters;
- SD/MMC block device, data-only partition layout, and VFAT `/data`;
- pinctrl, clock, PWM, and interrupt ownership relevant to TIM3, PB4, and PG7.

Gate: display, touch, Ethernet, SD/VFAT, serial, and SSH each have a captured
positive test.  Any absent device becomes a named parity gap rather than an
assumed path.

### 4. Port LVGL/lvmusic and Play automation

Build the same LVGL/lv_demo_music revision, resolution, color format, and
refresh period used by LXP.  Deliberately set
`LV_USE_PERF_MONITOR_LOG_MODE=1` and capture FPS, refresh/render time, flush
time, and CPU samples throughout the run.  Keep LXP DMA2D probes as harmless
fallbacks only if the native binary requires that source compatibility, and
report the driver actually selected.

Use Linux uinput to activate Play after the 15-second intro.  A selectable
input path may be added to lvmusic if uinput coordinate mapping is unreliable.
A manual tap is acceptable for an initial display smoke test but not for a
comparable five-minute result.

Gate: automated Play reaches the same active scene and yields enough
performance-log samples to identify the active interval.

### 5. Port and validate the shell hammer

Adapt the shell reference without changing its work:

- lvmusic at nice -5, a 15-second intro, then Play;
- one continuous HTTP receive stream at nice 10;
- SQLite at nice 0 on `/data`, `journal_mode=DELETE`, `synchronous=FULL`;
- eight 1 KiB blobs per transaction, a persistent metadata increment of eight,
  at most 128 live rows, and `VACUUM` every 20 transactions;
- a 300-second common deadline and stop only at a transaction/VACUUM boundary;
- `PRAGMA integrity_check` and exact row/meta/live-row validation;
- no SQLite errors, no early network EOF, a positive deadline-length transfer,
  and enough active LVGL samples.

Replace `/proc/lxp_fs` with before/after `/proc/mounts`, `/proc/diskstats`,
block counters, interface counters, CPU accounting, and process attribution.
Do not execute `df`, `statfs`, `top`, or repeated `cat` processes under the
measured NOMMU load: they can require unavailable high-order allocations, and
the first FAT free-cluster scan is extremely slow at 2 MHz. Read procfs/sysfs
with the resident shell and keep any filesystem-space query outside the load.
Preserve unmodified raw output as well as normalized JSON.
Scripts are runnable from `/tmp` during iteration; the accepted result uses
the copy embedded at `/usr/bin/native-linux-hammer` in the recorded CramFS.

Gate: a short smoke run satisfies the same invariants before any 300-second
run is accepted.

### 6. Linux real-time latency counterpart

The fallback absolute-clock, 1 kHz `SCHED_FIFO` userspace thread remains
available, but it is not selected when the physical driver is present.

The live device-tree, pinctrl, PWM, clock, and interrupt audit found TIM3, PB4,
and PG7 available. The implemented kernel driver exclusively claims the timer
resource, IRQ 29, pinctrl reference pin, and response GPIO. TIM3_CH1 generates
the 1 kHz/50 us D3 pulse in hardware; a priority-99 `SCHED_FIFO` kernel thread
pulses D4 around the same calculation. `/proc/rt_scope` reports the software
metrics. Oscilloscope observations still require a connected instrument.

Gate: baseline latency is collected with `CONFIG_PREEMPT` disabled.  Any later
`CONFIG_PREEMPT` image and result have distinct identities and result paths.

### 7. Reproducible five-minute execution

Deploy and reset the Pi stream server, validate `/reset`, `/ready`, the
300-second `/stream`, and `/metrics`, then launch the shell benchmark over SSH
while retaining serial as the boot/fault channel.  Collect:

- complete boot and benchmark logs;
- image hashes and final Buildroot/kernel configurations;
- boot time and available memory;
- LVGL FPS/render/flush distributions;
- SQLite transactions and transactions/s;
- network bytes, elapsed time, and Mbit/s;
- SD/filesystem/network counters before and after;
- CPU utilization and per-process attribution;
- dispatch distribution, missed releases, and late finishes;
- two-channel scope observations when the hardware driver exists.

Gate: the 300-second result passes every workload invariant and retains enough
raw evidence to regenerate its machine-readable summary.

### 8. Apples-to-apples analysis and handoff

Select a freshly identified successful LXP + oveRTOS shell result matching the
current contract.  Use the known nine-run
`language-hammer-20260811-final-2mhz` set only as background until its exact
revisions and settings are confirmed.  Compare same board, clock, SD card,
VFAT data filesystem, LVGL scene/settings, duration, stream server, nice
levels, SQL, VACUUM cadence, validation rules, and (where available) physical
latency semantics.

Every deviation is included beside the result and classified by likely bias:
Linux-favouring, LXP-favouring, or indeterminate.  Do not combine baseline and
preemptible-kernel numbers or equate software timer latency with physical
CH1-to-CH2 latency.

Gate: final artifacts contain the raw logs, JSON summaries, concise comparison,
parity-gap table, and exact restoration steps.

## Risks and unavoidable differences

| Risk or difference | Mitigation and reporting rule |
| --- | --- |
| STM32F746G-DISCO exposes 8 MiB external SDRAM, and LTDC needs a 512 KiB default DMA pool | Track boot-time free memory and peak process memory; minimize userspace and avoid adding secondary language runtimes to the shell PoC.  Preserve the stock 16 MiB config/8 MiB DTS mismatch as baseline evidence, but make the PoC config and DTS agree on the physical 8 MiB. |
| ARM NOMMU/FDPIC package support is uneven | Build each dependency early; replace only unsupported tooling with a small purpose-built program and disclose it. |
| Stock fragment removes block, multiuser, timerfd, sysctl, and crypto facilities | Re-enable only demonstrated dependencies in the derived config; retain the untouched baseline config and non-preempt policy. |
| Native framebuffer/touch coverage may be incomplete in Linux 5.15 | Verify device-tree and driver binding from logs/sysfs; treat patches as separate logical changes. |
| Native Linux may lack LXP's DMA2D path | Record the actual LVGL draw/flush backend; software rendering is not performance-parity and its directional bias must accompany results. |
| QSPI root differs from LXP's personality CPIO layout | Keep it read-only, hash the full QSPI image, and report that only Linux executable/file reads use QSPI while SD carries only FAT data writes. |
| The current Linux SD request is 2 MHz and PL180 reports 1.95 MHz actual | Record the requested and actual clocks; retain driver/DMA policy as an unavoidable implementation difference. |
| FAT implementations and cache/writeback semantics differ | Match VFAT and SQLite durability pragmas, sync at defined boundaries, capture mount options, and avoid claiming semantic identity beyond those controls. |
| Non-preemptible Linux can have long scheduling tails | Preserve it as the requested baseline; evaluate `CONFIG_PREEMPT` only as a separately identified follow-up. |
| Linux and oveRTOS now share the D3/PB4 hardware reference and D4/PG7 response, but use system-native scheduler objects | Compare the physical edge quantity while documenting Linux's FIFO kernel thread versus oveRTOS's portable critical host task. |
| Current `/data` FAT has an allocation entry beyond EOF | Do not benchmark, repair, or format until explicit approval; back up and run a read-only check before any repair. |
| U-Boot flashing overwrites oveRTOS internal flash | Verify the intended existing oveRTOS flash launcher before use and include its exact restoration command. |
| SD imaging is destructive and may erase `/data` | Resolve the exact removable device with `lsblk`, show it to the user, and require confirmation before unmount or `dd`; preserve benchmark data first when requested. |

## Destructive-operation boundary and restoration

Build commands do not write the board, SD card, or QSPI.  The approved QSPI
XIP image and matching internal-flash U-Boot have now been programmed only
after exact backups and readback verification.  Before SD imaging, show `lsblk -o
NAME,PATH,SIZE,MODEL,TRAN,MOUNTPOINTS`, resolve one exact removable whole-disk
path, and obtain explicit user confirmation before unmounting or using `dd`.

Flashing native U-Boot changes only STM32 internal flash.  Restore the intended
oveRTOS engine only after verifying its artifact, for example:

```
cd /home/varcain/projects/private/hIRoic/oveRTOS
test -x output/stm32f746/freertos/linux_interop/flash
output/stm32f746/freertos/linux_interop/flash
```

QSPI now contains the Linux XIP experiment.  The exact pre-Linux full-bank
backup is
`output-qspi/hardware-backup/qspi-before-linux-20260813T020133Z.bin`
(SHA-256 `dc370faee88fc88cab1bf23ed1ecac90fd2e1c9d9b17bc42b967183b0225a753`).
The exact internal-flash backup made in the same session is
`output-qspi/hardware-backup/internal-flash-before-linux-20260813T020133Z.bin`
(SHA-256 `1960bb74140f55881604aac48d17fda187735d635baa2a331469bc5bd83b1b7c`).
The regular verified oveRTOS flash launchers and LXP QSPI programmer are the
preferred personality restoration path and are listed in `hammer/README.md`.
The SD card is a data-only MBR/FAT card, but it is currently inconsistent and
Linux remounts it read-only when the bad allocation entry is encountered. No
whole-device pre-format image exists, so byte-exact recovery of content that
predated provisioning is impossible; do not repair or claim recovery without
explicit user approval.
