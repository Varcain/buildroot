# STM32F746 native-Linux hammer image

This directory contains the derived native-Linux image and the shell-first
hammer proof of concept.  It deliberately leaves the stock
`stm32f746_disco_sd_defconfig` unchanged.  The implementation plan and its
acceptance gates are in `../native-linux-hammer-plan.md`.

The current hardware closure status is machine-readable in
`parity-status-20260815.json`. Native Linux boots deterministically from QSPI,
the in-place FAT repair is complete, sustained DMA-backed Linux writes pass,
and the physical D3/D4 latency driver is live. The `CONFIG_PREEMPT_NONE`,
`CONFIG_PREEMPT`, and corrected `CONFIG_PREEMPT_RT` five-minute iterations are
recorded separately; the accepted current PREEMPT_RT run passes the full
contract. The principal comparison gap is Linux software rendering versus LXP
DMA2D. An external oscilloscope trace and Pi-to-board Dropbear repair remain
useful follow-ups but do not invalidate the serial-captured benchmark.

The memory-feasible hammer boot keeps U-Boot in the STM32's 1 MiB internal
flash, executes the Linux kernel directly from the 16 MiB QSPI aperture, and
mounts a read-only XIP-enabled CramFS from another aligned QSPI window. ELF
read-only segments execute directly from QSPI, which preserves enough of the
board's 8 MiB SDRAM for the concurrent GUI, SQLite, network, and latency work.
The SD
card is not in the boot path; it is used only for persistent FAT `/data` when
running the parity benchmark. A copied-to-RAM QSPI-root boot was proven
separately, but had only about 448 KiB free after init. Combining QSPI root
with QSPI XIP recovers kernel text and read-only-data RAM without moving the
root filesystem to SD. The internal-flash XIP kernel cannot fit.

## Reproduce the images

From the `buildroot2` root:

```sh
make O=output-hammer-qspi-xip \
    stm32f746_disco_hammer_qspi_xip_preempt_rt_defconfig
make O=output-hammer-qspi-xip \
    BUILD_DIR=/tmp/hirioic-hammer-qspi-xip.F2VnP2/build \
    TARGET_DIR=/tmp/hirioic-hammer-qspi-xip.F2VnP2/target \
    HOST_DIR=/tmp/hirioic-hammer-qspi-xip.F2VnP2/host \
    CCACHE_DIR=/tmp/hirioic-hammer-qspi-xip.F2VnP2/ccache \
    -j"$(nproc)"
```

Those `/tmp` directories identify the proven build tree. A fresh build should
choose a new private `/tmp` prefix and use the same four overrides for every
make invocation; output symlinks and result metadata must point at that one
prefix. Keep `stm32f746_disco_hammer_qspi_xip_defconfig` for the historical
PREEMPT_NONE baseline and the `_preempt_defconfig` profile for the measured
generic-preemption variant.

The relevant outputs are:

- `output-hammer-qspi-xip/images/u-boot.bin`, for internal flash;
- `output-hammer-qspi-xip/images/xipImage` and `uImage.xip`;
- `output-hammer-qspi-xip/images/qspi-hammer-xip.img`, the complete verified
  16 MiB QSPI layout with the DTB at `0x0e0000`, legacy header at
  `0x0fffc0`, 1 MiB-aligned XIP kernel at `0x100000`, and CramFS at
  `0x800000`;
- `output-hammer-qspi-xip/images/rootfs.cramfs`, the XIP-enabled read-only root.

The combined image layout is:

| QSPI offset | CPU address | Slot size | Purpose |
| --- | --- | ---: | --- |
| `0x0e0000` | loaded to SDRAM | 64 KiB | device tree |
| `0x0fffc0` | `0x900fffc0` | 64 bytes | legacy U-Boot header |
| `0x100000` | `0x90100000` | to `0x800000` | XIP kernel |
| `0x800000` | `0x90800000` | 8 MiB | read-only XIP CramFS root |

The stock-comparable baseline remains `CONFIG_PREEMPT_NONE`; PREEMPT and
PREEMPT_RT are separately identified experiments and never replace its stored
result. The current accepted profile is
`stm32f746_disco_hammer_qspi_xip_preempt_rt_defconfig`. It caps the SD bus at
2 MHz; debugfs reports a requested 2,000,000 Hz and an actual 1,950,000 Hz
clock. The earlier accepted 24 MHz run remains a historical diagnostic. LVGL
9.5.0 uses RGB565, 480x272, a 33 ms refresh period, a full-height draw buffer,
and console performance logging. Native Linux uses software drawing and fbdev
`pwrite`; it does not use DMA2D.

The STM32F7 MPU is part of the boot contract. U-Boot temporarily maps the
QSPI aperture as executable Normal, non-cacheable memory in region 3 and
leaves the controller enabled. Linux immediately replaces region 3 with its
own read-only XIP ROM mapping. The kernel image is deliberately 1 MiB aligned
so its approximately 2.7 MiB text/rodata span fits the required power-of-two
PMSA region without an invalid base/size combination. Linux then maps the
8 MiB rootfs window as executable, user-readable, read-only Normal memory and
exposes it through `mtd-rom` directly to CramFS. Filesystem permissions and
the ROM MTD driver prevent writes, while the MPU permits direct user XIP. It never
probes or resets the STM32 QSPI controller from which it executes.

## SD-card boundary

No build or boot command writes removable media, and the card does not need to
be removed for QSPI bring-up. With no usable card Linux still boots and mounts
volatile ramfs at `/data`; the benchmark preflight rejects that fallback.

For the workload, `/data` must be an explicitly identified VFAT partition.
The init script accepts partition 1 on a data-only card or partition 2 on the
older SD-root layout. The current card is `/dev/mmcblk0p1`, a 14.9 GiB
data-only FAT32 partition, at an actual 1.95 MHz. The accepted PREEMPT_RT
profile uses STM32 DMA2 streams 3 and 6, channel 4, full FIFO, INC4, and
peripheral flow control, matching the validated oveRTOS transport controls. It
retains the common one-sector request cap. An explicitly authorized in-place
recovery selected FAT1 through standard FAT32 ExtFlags, repaired it with the
bounded-memory maintenance image, and did not format or repartition the card.
A compact dosfstools 4.2 checker and matching libc were staged in `/tmp`
through the Pi, so closing the dirty flag required no QSPI update. The
clean-boot precheck found no chain or directory damage; the repair touched
active FAT1 only. The final read-only check reports `12 files, 65/1948688
clusters` with return code zero. The next boot had no dirty-volume warning, the
five-minute benchmark passed SQLite integrity and all row invariants, and
`/data` was then synced and cleanly unmounted. Evidence is under
`output-hammer-iterations/08-preempt-rt-sd-dma-pfctrl/logs/`. Any future format,
repartition, or whole-device image still requires exact-device discovery,
backup, and explicit confirmation.

BusyBox has no FAT consistency checker. The maintenance profile therefore
enables dosfstools `fsck.fat`, disables its large static iconv tables, and uses
an 8,192-entry bounded ownership table. The checker used for the final repair
is 80,888 bytes with SHA-256
`e5a15c1f562a43601dc11568ed534ea2f279b22c5041335798e5004d1d1ee712`.
Moving the libiconv dependency under the disabled charset option removed about
930 KiB of unused target payload. Building or RAM-staging these files does not
program QSPI. Prefer `/tmp` or `/data` staging for runtime scripts and
diagnostics; reprogram QSPI only when a changed kernel, device tree, or rootfs
must become the booted artifact.

## Flash QSPI XIP, U-Boot, and capture the boot

The XIP programmer first backs up all 16 MiB of the existing QSPI, then erases,
programs, and verifies the complete new image. This replaces the QSPI-root or
LXP personality contents and does not access the SD card.

```sh
board/stmicroelectronics/stm32f746-disco/hammer/flash_qspi_xip.sh \
    output-hammer-qspi-xip
```

The board's `flash_sd.sh` name is misleading: it writes only U-Boot to internal
flash and never writes the SD card or QSPI.  Flash the matching XIP-aware
U-Boot only after QSPI verification succeeds.

```sh
./board/stmicroelectronics/stm32f746-disco/flash_sd.sh \
    output-hammer-qspi-xip
```

Discover the VCP rather than assuming its number:

```sh
ls -l /dev/serial/by-id/ 2>/dev/null
ls -l /dev/ttyACM* 2>/dev/null
dmesg | tail -n 50
fuser -v /dev/EXACT_SERIAL
```

Capture from reset through the login prompt:

```sh
python3 scripts/capture_serial_boot.py \
    --device /dev/EXACT_SERIAL \
    --output output-hammer-qspi-xip/hammer-results/native-linux-boot.log
```

Interactive settings are 115200 baud, 8N1, and no flow control.  Serial login
is `root` / `root`.  Dropbear is key-only and uses the tracked public key; its
private host key is generated under `/data/.native-linux`, not on the
read-only root. Without VFAT it is regenerated in ramfs after each boot. After
login, retain all of:

```sh
uname -a
cat /proc/cmdline
cat /proc/meminfo
cat /proc/interrupts
mount
ip addr
cat /sys/class/graphics/fb*/name
cat /sys/class/input/event*/device/name
```

The image configures `eth0` as `172.1.1.2/24`.  Verify the benchmark path with:

```sh
ssh -J pi root@172.1.1.2
```

The alternating warm-boot failure was traced to stack protection in ARM's
pre-`.data` XIP inflater: successful inflation changed the guard used by the
same function's epilogue. Compiling only that early inflater without stack
protection fixed the false panic. Consecutive Linux warm reboots and an
OpenOCD reset now boot without a retry; see
`output-hammer-qspi-xip/logs/native-linux-216mhz-consecutive-boot.log` and the
current `native-linux-216mhz-2mhz-final-boot-identity.log`. The `pi` alias
identifies `varcain@192.168.1.12` through `odroid`. The local agent key is
rejected on the second hop, but entering through `ssh odroid` and using
odroid's resident key reaches the Pi. That route reproduced the accepted
`/metrics` values, and Pi-to-board ICMP passes without interface errors.
Dropbear on the STM32 completes negotiation but its only offered
`chacha20-poly1305` stream then reports a corrupted packet. Serial therefore
remains the proven board-administration and capture path; no password is
embedded in the image, scripts, or logs.

For reliable automation over the small UART FIFO, use the paced runner and
keep the password outside the command line:

```sh
SERIAL_CONSOLE_PASSWORD=root python3 scripts/run_serial_command.py \
    --device /dev/EXACT_SERIAL --output /tmp/serial-command.log \
    --command 'uname -a; cat /proc/rt_scope'
```

## Stream server and benchmark

The runner copies `scripts/hammer_stream_server.py` to `/tmp` on the Pi, starts
it, and checks `/reset`, `/ready`, and `/metrics`.  To validate deployment only:

```sh
python3 scripts/hardware_hammer_linux.py \
    --build-output output-hammer-qspi-xip --deploy-only
```

Once a smoke run passes, execute the accepted five-minute result:

```sh
python3 scripts/hardware_hammer_linux.py \
    --build-output output-hammer-qspi-xip --duration 300
```

A short run is diagnostic and is always labelled non-comparable:

```sh
python3 scripts/hardware_hammer_linux.py \
    --build-output output-hammer-qspi-xip \
    --duration 30 --allow-smoke
```

Each timestamped result contains the target identity, raw benchmark log,
Buildroot/Linux/U-Boot/BusyBox/LVGL configurations, image hashes, and normalized
JSON.  The validator requires the exact SQLite row/meta/live-row invariants,
zero SQLite error output, a complete deadline-length stream, enough active
LVGL samples, and consistent latency release accounting.

The accepted current 2 MHz-profile physical-scope result is:
`output-hammer-iterations/08-preempt-rt-sd-dma-pfctrl/results/full-300s-448k-arena-v2/result.json`.
It archives the exact Buildroot and Linux configurations and hashes the
deployed kernel, DTB, SQLite helper, runner and wrapper. The result, raw serial
log, clean boot, FAT repair and post-run unmount hashes are repeated in
`results/iterations/08-preempt-rt-sd-dma-pfctrl.json`. Historical baseline and
failed intermediate experiments remain under `results/iterations/` and are
summarized in `comparison.md`.

The current latency implementation is a kernel driver with the same physical
contract as LXP + oveRTOS: TIM3 generates a 1 kHz, 50 us high pulse on Arduino
D3/PB4/TIM3_CH1, IRQ 29 wakes a priority-99 `SCHED_FIFO` kernel thread, and
Arduino D4/PG7 is high around the same 512-iteration calculation. TIM3 runs at
54 MHz from the 108 MHz APB1 timer input. `/proc/rt_scope` reports releases,
executions, misses, late finishes, min/average/p99/p99.9/max dispatch, work
time, and IRQ-age diagnostics. Connect oscilloscope CH1 to Arduino D3 and CH2
to Arduino D4 with both probe grounds on board GND. No USB oscilloscope was
enumerated during this session, so an instrument trace remains external
evidence even though both waveforms are generated continuously.

## Restoration

To restore the regular FreeRTOS oveRTOS firmware in internal flash, first
verify the selected existing artifact, close serial consumers, and then run:

```sh
cd /home/varcain/projects/private/hIRoic/oveRTOS
test -x output/stm32f746/freertos/linux_interop/flash
output/stm32f746/freertos/linux_interop/flash
```

Equivalent NuttX and Zephyr launchers must be selected and verified explicitly
when those engines are intended. QSPI XIP replaces the complete QSPI range.
The full bank immediately before the current 2 MHz parity image is
`output-hammer-qspi-xip/hardware-backup/qspi-before-hammer-xip-20260814T011845Z.bin`,
SHA-256
`ec8e0fd2e94f370e55484d94b316a595428502efd703572a741f2bc9d8d7947d`.
It restores the preceding 216 MHz Linux scope image. The earliest pre-Linux
backup below restores the state from before native-Linux work.
The earliest pre-Linux full-bank backup is
`output-qspi/hardware-backup/qspi-before-linux-20260813T020133Z.bin`, SHA-256
`dc370faee88fc88cab1bf23ed1ecac90fd2e1c9d9b17bc42b967183b0225a753`.
After closing serial consumers and obtaining confirmation for the destructive
QSPI replacement, restore that exact image with:

```sh
cd /home/varcain/projects/private/hIRoic/buildroot2
backup=output-qspi/hardware-backup/qspi-before-linux-20260813T020133Z.bin
test "$(stat -c%s "$backup")" -eq 16777216
printf '%s  %s\n' \
    dc370faee88fc88cab1bf23ed1ecac90fd2e1c9d9b17bc42b967183b0225a753 \
    "$backup" | sha256sum -c -
output-hammer-qspi-xip/host/bin/openocd \
    -s output-hammer-qspi-xip/host/share/openocd/scripts \
    -f board/stm32f746g-disco.cfg \
    -c init -c 'reset init' -c 'flash probe 3' \
    -c "flash erase_address pad 0x90000000 0x01000000" \
    -c "flash write_bank 3 $backup 0" \
    -c "flash verify_bank 3 $backup 0" \
    -c 'reset run' -c shutdown
```

Alternatively, restore the regular LXP rootfs with:

```sh
cd /home/varcain/projects/private/hIRoic/oveRTOS
boards/stm32f746g-discovery/freertos/flash-qspi.sh \
    /home/varcain/projects/private/hIRoic/buildroot/output/images/rootfs.cpio
```

The exact pre-Linux internal-flash backup is
`output-qspi/hardware-backup/internal-flash-before-linux-20260813T020133Z.bin`,
SHA-256
`1960bb74140f55881604aac48d17fda187735d635baa2a331469bc5bd83b1b7c`.
Prefer the engine-specific verified launcher above unless byte-for-byte
restoration is required.

The SD partition layout does not need to change to return to LXP. FAT1 is now
the standard active FAT, passes read-only checking, and survived the Linux
write test and benchmark. There is no whole-device image from before this card
was provisioned, so deletion of the partition cannot be exactly reversed. Any
future repair, repartition, format, or whole-device write must begin with
exact-device identification and backup, unmount, and explicit user
confirmation.
