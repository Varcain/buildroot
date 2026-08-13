#!/bin/bash
set -euo pipefail

flash_size=$((0x01000000))
kernel_offset=$((0x00000000))
kernel_read_size=$((0x00300000))
dtb_offset=$((0x005f0000))
dtb_slot_size=$((0x00010000))
rootfs_offset=$((0x00600000))
rootfs_slot_size=$((0x00a00000))

kernel="${BINARIES_DIR}/zImage"
dtb="${BINARIES_DIR}/stm32f746-disco-qspi.dtb"
rootfs="${BINARIES_DIR}/rootfs.squashfs"
image="${BINARIES_DIR}/qspi-linux.img"
manifest="${BINARIES_DIR}/qspi-linux.manifest"

for input in "$kernel" "$dtb" "$rootfs"; do
	[ -f "$input" ] || { echo "missing QSPI input: $input" >&2; exit 1; }
done

kernel_size=$(stat -c%s "$kernel")
dtb_size=$(stat -c%s "$dtb")
rootfs_size=$(stat -c%s "$rootfs")

[ "$kernel_size" -le "$kernel_read_size" ] || {
	echo "zImage exceeds the 3 MiB load slot: $kernel_size" >&2
	exit 1
}
[ "$dtb_size" -le "$dtb_slot_size" ] || {
	echo "DTB exceeds the 64 KiB slot: $dtb_size" >&2
	exit 1
}
[ "$rootfs_size" -le "$rootfs_slot_size" ] || {
	echo "SquashFS exceeds the 10 MiB rootfs slot: $rootfs_size" >&2
	exit 1
}

# Use erased-NOR bytes between payloads.  A complete 16 MiB image also removes
# stale data from the previous QSPI personality and makes verification exact.
dd if=/dev/zero bs=1M count=16 status=none | tr '\000' '\377' >"$image"
dd if="$kernel" of="$image" bs=64K seek=$((kernel_offset / 65536)) conv=notrunc status=none
dd if="$dtb" of="$image" bs=64K seek=$((dtb_offset / 65536)) conv=notrunc status=none
dd if="$rootfs" of="$image" bs=64K seek=$((rootfs_offset / 65536)) conv=notrunc status=none

[ "$(stat -c%s "$image")" -eq "$flash_size" ]

{
	printf 'format=qspi-linux-v1\n'
	printf 'flash_base=0x90000000\n'
	printf 'flash_size=0x%08x\n' "$flash_size"
	printf 'kernel_offset=0x%08x\n' "$kernel_offset"
	printf 'kernel_size=0x%08x\n' "$kernel_size"
	printf 'kernel_read_size=0x%08x\n' "$kernel_read_size"
	printf 'kernel_sha256=%s\n' "$(sha256sum "$kernel" | awk '{print $1}')"
	printf 'dtb_offset=0x%08x\n' "$dtb_offset"
	printf 'dtb_size=0x%08x\n' "$dtb_size"
	printf 'dtb_sha256=%s\n' "$(sha256sum "$dtb" | awk '{print $1}')"
	printf 'rootfs_offset=0x%08x\n' "$rootfs_offset"
	printf 'rootfs_size=0x%08x\n' "$rootfs_size"
	printf 'rootfs_sha256=%s\n' "$(sha256sum "$rootfs" | awk '{print $1}')"
	printf 'image_sha256=%s\n' "$(sha256sum "$image" | awk '{print $1}')"
} >"$manifest"

echo "QSPI image: $image ($(stat -c%s "$image") bytes)"
cat "$manifest"
