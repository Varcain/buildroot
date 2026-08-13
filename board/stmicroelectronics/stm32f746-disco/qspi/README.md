# STM32F746 no-SD QSPI boot experiment

## Hardware result (2026-08-13)

The complete 16 MiB `qspi-linux.img` was programmed and verified byte for
byte.  U-Boot loads the kernel and DTB from QSPI, Linux identifies the
N25Q128A, and mounts the read-only SquashFS root at `0x90600000`.  Ethernet,
FT5x06 touch, and LTDC/DRM framebuffer probe before init.  The system reaches
the serial login prompt, starts Dropbear through inetd, configures Ethernet as
`172.1.1.2/24`, and talks to the Pi at `172.1.1.1`.  This proves that the SD
card is not required for the native Linux boot path.

Five consecutive autonomous warm boots loaded aligned kernel/DTB images,
mounted the QSPI root at 1.516--1.517 seconds, reached login, and negotiated a
100 Mbps link.  The soak log contains no `Bad magic`, root-mount, panic, or
BusFault errors.  The final clean-built image was then programmed and verified
over all 16 MiB and passed another complete warm boot.  See:

```text
output-qspi/logs/native-linux-qspi-full-boot-soak-20260813.log
output-qspi/logs/native-linux-qspi-final-clean-boot-20260813.log
```

The 8 MiB NOMMU target reports 4,152 KiB available at kernel boot and 448 KiB
free after init, networking, inetd, and Dropbear host-key generation.  MMC is
an optional module and is not loaded unless `qspi_mmc=1` is added to the kernel
command line, preserving memory for the no-SD boot.

The requested internal-flash XIP image was also built.  It is 2,869,947 bytes,
but the board has exactly 1,048,576 bytes of internal flash.  With the upstream
STM32 32 KiB first-stage-loader reservation, only 1,015,808 bytes remain and
the image exceeds capacity by 1,854,139 bytes.  It was deliberately not
flashed.  See `results/20260813-qspi-bringup.json` and the generated
`output-internal-xip/images/internal-xip-capacity.txt`.

Two controller fixes are required for repeatable full boot:

- U-Boot applies the documented STM32F74x/F75x ES0290 section 2.4.3 sequence:
  clear `QUADSPI_AR`, abort the previous indirect transaction, wait for idle,
  then enter memory-mapped mode.  This removes the stale leading byte seen on
  warm reboots and permits aligned kernel/DTB loads.
- Linux direct reads from the QSPI aperture BusFault on this NOMMU handoff.
  The board-scoped PoC therefore uses 25 MHz indirect reads, limits them to the
  64-byte FIFO depth, polls transfer completion, and waits for completion flags
  to clear before the next command.  It is reliable but slower than a proven
  direct-map or XIP implementation.

Ethernet also needs a board-scoped no-MMU coherency workaround: stmmac flushes
descriptor rings in the cacheable coherent pool before DMA ownership changes
and restarts a suspended transmit engine.  Without it the DMA observed zeroed
descriptors and TX remained suspended.

## Decision and phases

The STM32F746 has 1 MiB of internal flash.  The native-Linux baseline's
uncompressed `Image` is over 4 MiB, while U-Boot alone is about 364 KiB.
Consequently, a useful native Linux kernel cannot execute in place from the
internal flash, even if U-Boot is removed.  An internal-XIP build is retained
as a size experiment, not as a flashable board image.

The safe implementation is phased:

1. Preserve the existing SD baseline and build a separate no-SD configuration.
2. Keep U-Boot in internal flash and place zImage, DTB, and a read-only
   SquashFS root in the 16 MiB memory-mapped QSPI NOR.
3. Back up the existing QSPI contents, then program and verify the complete
   combined image.
4. Boot the zImage from QSPI into SDRAM and mount the SquashFS through the
   Linux STM32 QSPI/SPI-NOR driver.  This establishes that SD is not required
   for boot.
5. Build an XIP kernel first at the upstream STM32 address `0x08008000` to
   leave 32 KiB for a first-stage loader and record the unavoidable capacity
   failure, then evaluate a QSPI-addressed XIP build at `0x90000000`.
6. Attempt QSPI XIP only with a bootloader hand-off which deliberately leaves
   the controller in memory-mapped mode.  The XIP kernel must expose its root
   through a read-only memory map rather than resetting the QSPI controller it
   is executing from.  This is not part of the working full-boot result: the
   Linux direct aperture is not yet safe and the driver currently owns and
   resets the same controller used for the root filesystem.

## QSPI image layout

| Offset | Size | Purpose |
| --- | ---: | --- |
| `0x000000` | `0x300000` read / `0x5f0000` reserved | zImage now; XIP kernel experiment later |
| `0x5f0000` | `0x010000` | device tree |
| `0x600000` | `0xa00000` | read-only SquashFS root |

`post-image.sh` creates a complete 16 MiB `qspi-linux.img`, fills unused bytes
with the NOR erased value `0xff`, enforces every slot boundary, and writes a
hash/size manifest.

Build without changing the SD baseline artifacts:

```sh
make O=output-qspi stm32f746_disco_qspi_defconfig
make O=output-qspi -j"$(nproc)"
```

Build the internal-flash XIP capacity experiment separately:

```sh
make O=output-internal-xip stm32f746_disco_internal_xip_defconfig
make O=output-internal-xip -j"$(nproc)"
```

This emits `images/xipImage` and `images/internal-xip-capacity.txt`.  The
report is deliberately non-flashing and accounts for the board's exact 1 MiB
internal flash plus the 32 KiB first-stage-loader reservation.

`flash_qspi_linux.sh output-qspi` first reads all 16 MiB into a timestamped
backup, then erases, programs, and verifies the QSPI image.  Programming QSPI
destroys the validated LXP rootfs there; the backup and the established LXP
programmer are both restoration paths.

Flash the matching QSPI-boot U-Boot into internal flash only after the QSPI
image verifies:

```sh
./board/stmicroelectronics/stm32f746-disco/flash_sd.sh output-qspi
```

Despite its name, `flash_sd.sh` does not touch an SD card.

## Is an SD card still needed?

It is not needed for boot, serial access, display/touch bring-up, networking,
the SSH listener, or read-only workload staging.  SSH through the Pi was not
logged in because the development host had no usable SSH agent; no password
was embedded as a workaround.  With no usable SD benchmark partition,
`/data` is ramfs solely so first boot and Dropbear work.  This board uses a
no-MMU kernel, for which tmpfs is unavailable.

It remains needed for an apples-to-apples hammer result because the benchmark
contract requires SQLite on persistent FAT/VFAT media, on the same physical SD
card and at the same bus speed as LXP.  QSPI SquashFS is read-only; a RAM
filesystem is non-persistent; and a writable flash filesystem would have
different erase, durability, and scheduling semantics.  The hammer preflight
must continue to reject the ramfs fallback as non-VFAT.

## Restoration

The first backups made before Linux programming are:

```text
output-qspi/hardware-backup/qspi-before-linux-20260813T020133Z.bin
  sha256 dc370faee88fc88cab1bf23ed1ecac90fd2e1c9d9b17bc42b967183b0225a753
output-qspi/hardware-backup/internal-flash-before-linux-20260813T020133Z.bin
  sha256 1960bb74140f55881604aac48d17fda187735d635baa2a331469bc5bd83b1b7c
```

Restore and verify the exact saved QSPI image:

```sh
cd /home/varcain/projects/private/hIRoic/buildroot2
test "$(stat -c%s output-qspi/hardware-backup/qspi-before-linux-20260813T020133Z.bin)" -eq 16777216
output-qspi/host/bin/openocd \
  -s output-qspi/host/share/openocd/scripts \
  -f board/stm32f746g-disco.cfg \
  -c init -c 'reset init' -c 'flash probe 3' \
  -c 'flash erase_address pad 0x90000000 0x01000000' \
  -c 'flash write_bank 3 output-qspi/hardware-backup/qspi-before-linux-20260813T020133Z.bin 0' \
  -c 'flash verify_bank 3 output-qspi/hardware-backup/qspi-before-linux-20260813T020133Z.bin 0' \
  -c 'reset run' -c shutdown
```

Restore and verify the exact saved internal-flash image:

```sh
cd /home/varcain/projects/private/hIRoic/buildroot2
test "$(stat -c%s output-qspi/hardware-backup/internal-flash-before-linux-20260813T020133Z.bin)" -eq 1048576
output-qspi/host/bin/openocd \
  -s output-qspi/host/share/openocd/scripts \
  -f board/stm32f746g-disco.cfg \
  -c init -c 'reset init' -c 'flash probe 0' \
  -c 'flash write_image erase output-qspi/hardware-backup/internal-flash-before-linux-20260813T020133Z.bin 0x08000000 bin' \
  -c 'verify_image output-qspi/hardware-backup/internal-flash-before-linux-20260813T020133Z.bin 0x08000000 bin' \
  -c 'reset run' -c shutdown
```

Restore the previous QSPI contents exactly from the backup made by the flash
script using the same OpenOCD `stmqspi` bank and a full erase/write/verify.
Alternatively, restore the regular LXP rootfs with the already verified
programmer:

```sh
cd /home/varcain/projects/private/hIRoic/oveRTOS
boards/stm32f746g-discovery/freertos/flash-qspi.sh \
  /home/varcain/projects/private/hIRoic/buildroot/output/images/rootfs.cpio
```

Restore the intended oveRTOS engine in internal flash only after verifying its
artifact, for example:

```sh
cd /home/varcain/projects/private/hIRoic/oveRTOS
test -x output/stm32f746/freertos/linux_interop/flash
output/stm32f746/freertos/linux_interop/flash
```

No step in this experiment writes, repartitions, or formats an SD card.  The
inserted 14.9 GiB card was detected as `/dev/mmcblk0` with no partitions, so it
cannot presently supply the required VFAT `/data`.  Creating that partition is
destructive and still requires explicit device confirmation from the user.
