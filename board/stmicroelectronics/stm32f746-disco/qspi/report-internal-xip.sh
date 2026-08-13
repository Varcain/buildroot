#!/bin/bash
set -euo pipefail

internal_flash_base=$((0x08000000))
internal_flash_size=$((0x00100000))
xip_address=$((0x08008000))
loader_reserve=$((xip_address - internal_flash_base))
available=$((internal_flash_size - loader_reserve))
image="${BINARIES_DIR}/xipImage"
report="${BINARIES_DIR}/internal-xip-capacity.txt"

[ -f "$image" ] || { echo "missing internal-XIP image: $image" >&2; exit 1; }
image_size=$(stat -c%s "$image")

if [ "$image_size" -le "$available" ]; then
	result=FIT
else
	result=DOES_NOT_FIT
fi

{
	printf 'result=%s\n' "$result"
	printf 'internal_flash_base=0x%08x\n' "$internal_flash_base"
	printf 'internal_flash_size=%u\n' "$internal_flash_size"
	printf 'xip_address=0x%08x\n' "$xip_address"
	printf 'loader_reserve=%u\n' "$loader_reserve"
	printf 'available_for_xip=%u\n' "$available"
	printf 'xip_image_size=%u\n' "$image_size"
	printf 'over_capacity_by=%u\n' "$((image_size > available ? image_size - available : 0))"
	printf 'xip_image_sha256=%s\n' "$(sha256sum "$image" | awk '{print $1}')"
} >"$report"

cat "$report"
[ "$result" = FIT ] || echo "Refusing to treat this artifact as flashable."
