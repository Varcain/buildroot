# STM32F746 native-Linux hammer image

This directory contains the derived native-Linux image and the shell-first
hammer proof of concept.  It deliberately leaves the stock
`stm32f746_disco_sd_defconfig` unchanged.  The implementation plan and its
acceptance gates are in `../native-linux-hammer-plan.md`.

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
make O=output-hammer-qspi-xip stm32f746_disco_hammer_qspi_xip_defconfig
make O=output-hammer-qspi-xip \
    BUILD_DIR=/tmp/hirioic-hammer-qspi-xip.F2VnP2/build \
    TARGET_DIR=/tmp/hirioic-hammer-qspi-xip.F2VnP2/target \
    HOST_DIR=/tmp/hirioic-hammer-qspi-xip.F2VnP2/host \
    CCACHE_DIR=/tmp/hirioic-hammer-qspi-xip.F2VnP2/ccache \
    -j"$(nproc)"
```

Those are the exact directories used for the accepted image. A fresh build
should choose a new private `/tmp` prefix and use the same four overrides for
every make invocation; output symlinks and result metadata must point at that
one prefix.

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

The derived kernel remains `CONFIG_PREEMPT_NONE`. The accepted native run caps
the SD bus at 24 MHz; 2 MHz was not reliable during Linux PL180 write-path
bring-up. This is a material Linux-favouring parity gap versus the selected
LXP reference. LVGL 9.5.0 uses RGB565,
480x272, a 33 ms refresh period, a full-height draw buffer, and console
performance logging.  Native Linux uses software drawing and fbdev `pwrite`;
it does not use DMA2D.

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
older SD-root layout. The accepted run used `/dev/mmcblk0p1`, a 14.9 GiB
data-only FAT32 partition, at 24 MHz in PIO mode. It was synced and cleanly
unmounted after the run, and the kernel log contained no FAT, MMC, or I/O
error. Discover and back up the exact device and obtain confirmation before
creating, formatting, or imaging any partition.

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

On the accepted hardware, the first U-Boot XIP handoff after an OpenOCD QSPI
program stopped after `Starting kernel ...`. A second ST-LINK `reset run`
booted normally. This reset dependency is reproducible and remains a boot
gap. The host session had no SSH agent, so the Pi and jump-host authentication
checks failed before reaching the board; Ethernet traffic to the already
running Pi stream server was nevertheless verified by the complete benchmark
stream. Serial therefore remains the proven administrative access path.

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

The accepted result is
`output-hammer-qspi-xip/hammer-results/native-linux-strict300-embedded-final-300s-20260814/result.json`.
It is tied to full QSPI image SHA-256
`4821e9b3c7fff933f86d1a995b92259389c953d516ceaa75c696fff640888ac2`.
The raw benchmark, full boot, and post-run storage logs sit beside it and are
listed in `comparison.md`.

The latency PoC is an absolute `CLOCK_MONOTONIC` 1 kHz highest-priority
`SCHED_FIFO` userspace thread with the same 512-iteration calculation.  It is
timer-to-thread scheduling latency, not the oveRTOS TIM3 CH1-to-PG7 CH2
interrupt-to-thread measurement.  The final device tree leaves TIM3 disabled,
and neither PB4 nor PG7 is claimed by this PoC; live pinctrl, PWM, clock, and
interrupt ownership still must be audited before adding a kernel driver.

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

For the SD card, no rewrite is needed to return to the LXP hammer: leave the
data-only FAT partition intact and let LXP mount it as `/data`. There is no
whole-device image from before this card was provisioned, so deletion of the
partition cannot be exactly reversed. Any future repartition, format, or
whole-device write must begin with `lsblk`, an exact-device backup, unmount,
and explicit user confirmation.
