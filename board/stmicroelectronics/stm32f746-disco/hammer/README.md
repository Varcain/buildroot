# STM32F746 native-Linux hammer image

This directory contains the derived native-Linux image and the shell-first
hammer proof of concept.  It deliberately leaves the stock
`stm32f746_disco_sd_defconfig` unchanged.  The implementation plan and its
acceptance gates are in `../native-linux-hammer-plan.md`.

## Reproduce the images

From the `buildroot2` root:

```sh
make O=output stm32f746_disco_hammer_defconfig
make O=output -j"$(nproc)"
```

The relevant outputs are:

- `output/images/u-boot.bin`, for STM32 internal flash;
- `output/images/zImage` and `stm32f746-disco-hammer.dtb`;
- `output/images/rootfs.ext2`, a 64 MiB root filesystem booted read-only;
- `output/images/data.vfat`, an empty 256 MiB FAT benchmark filesystem;
- `output/images/sdcard.img`, containing the ext2 root and FAT `/data`.

The derived kernel remains `CONFIG_PREEMPT_NONE`.  The SD bus is capped at
2 MHz to match the proven LXP + oveRTOS hammer runs.  LVGL 9.5.0 uses RGB565,
480x272, a 33 ms refresh period, a full-height draw buffer, and console
performance logging.  Native Linux uses software drawing and fbdev `pwrite`;
it does not use DMA2D.

## Destructive SD-card gate

No build command writes removable media.  Before imaging, discover the card:

```sh
lsblk -o NAME,PATH,SIZE,MODEL,TRAN,RM,RO,MOUNTPOINTS
```

Resolve and display one exact removable whole-disk path.  Back up any existing
card, including benchmark data, and obtain explicit confirmation before
unmounting or running either `dd`.  Substitute only the confirmed literal path
for `/dev/EXACT_DEVICE`; do not use a glob or inferred device number.

```sh
sudo umount /dev/EXACT_PARTITION_1
sudo umount /dev/EXACT_PARTITION_2
sudo dd if=/dev/EXACT_DEVICE of=/safe/path/sdcard-before-native-linux.img \
    bs=4M conv=fsync status=progress
sync
sudo dd if=output/images/sdcard.img of=/dev/EXACT_DEVICE \
    bs=4M conv=fsync status=progress
sync
```

Unmount only partitions actually reported as mounted.  The generated image is
321 MiB and has these expected MBR partitions:

| Partition | Start sector | Size | Type | Use |
| --- | ---: | ---: | --- | --- |
| 1 | 2048 | 64 MiB | Linux/ext2 | read-only root |
| 2 | 133120 | 256 MiB | W95 FAT | `/data` |

Root and `/data` remain on the same physical card.  This is recorded in every
result because unrelated root I/O could still contend with data I/O.

## Flash U-Boot and capture the boot

The board's `flash_sd.sh` name is misleading: it writes only U-Boot to internal
flash and never writes the SD card or QSPI.

```sh
./board/stmicroelectronics/stm32f746-disco/flash_sd.sh output
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
    --output output/hammer-results/native-linux-boot.log
```

Interactive settings are 115200 baud, 8N1, and no flow control.  Serial login
is `root` / `root`.  Dropbear is key-only and uses the tracked public key; its
private host key is generated once under `/data/.native-linux`, not on the
read-only root.  After login, retain all of:

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

## Stream server and benchmark

The runner copies `scripts/hammer_stream_server.py` to `/tmp` on the Pi, starts
it, and checks `/reset`, `/ready`, and `/metrics`.  To validate deployment only:

```sh
python3 scripts/hardware_hammer_linux.py --deploy-only
```

Once a smoke run passes, execute the accepted five-minute result:

```sh
python3 scripts/hardware_hammer_linux.py --duration 300
```

A short run is diagnostic and is always labelled non-comparable:

```sh
python3 scripts/hardware_hammer_linux.py --duration 30 --allow-smoke
```

Each timestamped result contains the target identity, raw benchmark log,
Buildroot/Linux/U-Boot/BusyBox/LVGL configurations, image hashes, and normalized
JSON.  The validator requires the exact SQLite row/meta/live-row invariants,
zero SQLite error output, a complete deadline-length stream, enough active
LVGL samples, and consistent latency release accounting.

The latency PoC is an absolute `CLOCK_MONOTONIC` 1 kHz highest-priority
`SCHED_FIFO` userspace thread with the same 512-iteration calculation.  It is
timer-to-thread scheduling latency, not the oveRTOS TIM3 CH1-to-PG7 CH2
interrupt-to-thread measurement.  The final device tree leaves TIM3 disabled,
and neither PB4 nor PG7 is claimed by this PoC; live pinctrl, PWM, clock, and
interrupt ownership still must be audited before adding a kernel driver.

## Restoration

Native Linux does not touch QSPI.  To restore the regular FreeRTOS oveRTOS
firmware in internal flash, first verify the selected existing artifact, close
serial consumers, and then run:

```sh
cd /home/varcain/projects/private/hIRoic/oveRTOS
test -x output/stm32f746/freertos/linux_interop/flash
output/stm32f746/freertos/linux_interop/flash
```

Equivalent NuttX and Zephyr launchers must be selected and verified explicitly
when those engines are intended.  Since QSPI was not changed, no QSPI restore
is needed.  If a separately approved experiment later changes it, restore the
regular LXP rootfs with:

```sh
cd /home/varcain/projects/private/hIRoic/oveRTOS
boards/stm32f746g-discovery/freertos/flash-qspi.sh \
    /home/varcain/projects/private/hIRoic/buildroot/output/images/rootfs.cpio
```

Restore the SD card only to the same confirmed whole-disk device used above:

```sh
sudo dd if=/safe/path/sdcard-before-native-linux.img of=/dev/EXACT_DEVICE \
    bs=4M conv=fsync status=progress
sync
```

The backup is the only exact restoration source for the pre-existing partition
table and `/data`; do not image the card until that backup exists or the user
explicitly accepts losing it.
