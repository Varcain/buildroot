#!/bin/bash
set -euo pipefail

flash_size=$((0x01000000))
kernel_offset=$((0x000fffc0))
dtb_offset=$((0x005f0000))
kernel_slot_size=$((dtb_offset - kernel_offset))
dtb_slot_size=$((0x00010000))
xip_address=0x90100000

xip_image="${BINARIES_DIR}/xipImage"
legacy_image="${BINARIES_DIR}/uImage.xip"
dtb="${BINARIES_DIR}/stm32f746-disco-hammer.dtb"
image="${BINARIES_DIR}/qspi-hammer-xip.img"
manifest="${BINARIES_DIR}/qspi-hammer-xip.manifest"
mkimage="${HOST_DIR}/bin/mkimage"

if [ ! -x "$mkimage" ]; then
	set -- "${BUILD_DIR}"/uboot-*/tools/mkimage
	[ "$#" -eq 1 ] && [ -x "$1" ] || {
		echo "missing host mkimage (checked HOST_DIR and the U-Boot build tree)" >&2
		exit 1
	}
	mkimage=$1
fi

for input in "$xip_image" "$dtb"; do
	[ -f "$input" ] || { echo "missing QSPI XIP input: $input" >&2; exit 1; }
done

"$mkimage" -A arm -O linux -T kernel -C none \
	-a "$xip_address" -e "$xip_address" \
	-n "STM32F746 native Linux hammer QSPI XIP" \
	-d "$xip_image" "$legacy_image"

kernel_size=$(stat -c%s "$legacy_image")
dtb_size=$(stat -c%s "$dtb")
[ "$kernel_size" -le "$kernel_slot_size" ] || {
	echo "XIP uImage exceeds its QSPI slot: $kernel_size" >&2
	exit 1
}
[ "$dtb_size" -le "$dtb_slot_size" ] || {
	echo "DTB exceeds its QSPI slot: $dtb_size" >&2
	exit 1
}

dd if=/dev/zero bs=1M count=16 status=none | tr '\000' '\377' >"$image"
dd if="$legacy_image" of="$image" bs=64 seek=$((kernel_offset / 64)) \
	conv=notrunc status=none
dd if="$dtb" of="$image" bs=64K seek=$((dtb_offset / 65536)) \
	conv=notrunc status=none
[ "$(stat -c%s "$image")" -eq "$flash_size" ]

{
	printf 'format=qspi-hammer-xip-v1\n'
	printf 'flash_base=0x90000000\n'
	printf 'flash_size=0x%08x\n' "$flash_size"
	printf 'kernel_offset=0x%08x\n' "$kernel_offset"
	printf 'kernel_xip_address=%s\n' "$xip_address"
	printf 'kernel_size=0x%08x\n' "$kernel_size"
	printf 'kernel_sha256=%s\n' "$(sha256sum "$legacy_image" | awk '{print $1}')"
	printf 'dtb_offset=0x%08x\n' "$dtb_offset"
	printf 'dtb_size=0x%08x\n' "$dtb_size"
	printf 'dtb_sha256=%s\n' "$(sha256sum "$dtb" | awk '{print $1}')"
	printf 'image_sha256=%s\n' "$(sha256sum "$image" | awk '{print $1}')"
} >"$manifest"

echo "QSPI XIP image: $image ($(stat -c%s "$image") bytes)"
cat "$manifest"
