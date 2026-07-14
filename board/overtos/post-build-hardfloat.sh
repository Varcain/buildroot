#!/bin/sh
# Hard-float-only rootfs hook: build the VFP context stress test and reject any
# mixed soft/hard or non-FDPIC runtime component before a rootfs is published.
set -eu

TARGET="$1"
GCC="$HOST_DIR/bin/arm-buildroot-uclinuxfdpiceabi-gcc"
READELF="$HOST_DIR/bin/arm-buildroot-uclinuxfdpiceabi-readelf"
PROGS="$(CDPATH= cd -- "$(dirname -- "$0")/progs" && pwd)"

mkdir -p "$TARGET/usr/bin"
"$GCC" -mfdpic -O2 "$PROGS/fpcheck.c" -o "$TARGET/usr/bin/fpcheck"

check_hardfloat_fdpic()
{
	elf="$1"
	[ -e "$elf" ] || { echo "hard-float audit: missing $elf" >&2; exit 1; }
	"$READELF" -h "$elf" | grep -q 'OS/ABI:.*ARM FDPIC' || {
		echo "hard-float audit: not FDPIC: $elf" >&2
		exit 1
	}
	"$READELF" -h "$elf" | grep -q 'hard-float ABI' || {
		echo "hard-float audit: soft-float object in hard rootfs: $elf" >&2
		exit 1
	}
	"$READELF" -A "$elf" | grep -q 'Tag_FP_arch: FPv5' || {
		echo "hard-float audit: object is not FPv5: $elf" >&2
		exit 1
	}
	"$READELF" -A "$elf" | grep -q 'Tag_ABI_VFP_args: VFP registers' || {
		echo "hard-float audit: object does not use VFP arguments: $elf" >&2
		exit 1
	}
}

set -- "$TARGET/bin/busybox" \
	"$TARGET"/lib/libuClibc-*.so \
	"$TARGET"/lib/ld-uClibc-*.so \
	"$TARGET/lib/libgcc_s.so.1" \
	"$TARGET/usr/bin/fpcheck"
for elf do
	check_hardfloat_fdpic "$elf"
done

echo "hard-float audit: FDPIC + FPv5-SP-D16 + VFP argument ABI verified"
