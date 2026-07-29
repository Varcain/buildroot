#!/bin/sh
# Strip non-runtime ELF metadata from every FDPIC executable and shared object
# after all other rootfs post-build hooks have installed their files.
#
# Buildroot's normal target-finalize strip pass is gated by BR2_BINFMT_ELF and
# therefore does not run for BR2_BINFMT_FDPIC. Keep the same target/staging
# split here: only TARGET_DIR is stripped, so staging retains debug information
# for host-side and remote debugging.
set -eu

TARGET="$1"
STRIP="$HOST_DIR/bin/arm-buildroot-uclinuxfdpiceabi-strip"
READELF="$HOST_DIR/bin/arm-buildroot-uclinuxfdpiceabi-readelf"

if [ ! -x "$STRIP" ] || [ ! -x "$READELF" ]; then
	echo "FDPIC strip: target binutils are unavailable" >&2
	exit 1
fi

STRIP_LIST="$(mktemp /tmp/overtos-strip-fdpic.XXXXXX)"
trap 'rm -f "$STRIP_LIST"' 0 1 2 15
find "$TARGET" -type f > "$STRIP_LIST"

STRIPPED=0
SAVED=0
while IFS= read -r ELF; do
	REL="${ELF#"$TARGET"/}"
	case "$REL" in
		# dbgdemo intentionally retains source-level debug information.
		usr/bin/dbgdemo)
			continue
			;;
		# Match Buildroot's normal strip exclusions: loaders and libpthread
		# retain metadata needed by low-level debuggers; modules require
		# module-aware stripping rather than --strip-unneeded.
		lib/ld-uClibc-*.so* | usr/lib/ld-uClibc-*.so* | \
		lib/libpthread*.so* | usr/lib/libpthread*.so* | *.ko)
			continue
			;;
	esac

	# readelf is the format probe. This avoids treating executable scripts
	# and other non-ELF files as strip failures.
	if ! "$READELF" -h "$ELF" >/dev/null 2>&1; then
		continue
	fi

	BEFORE="$(stat -c %s "$ELF")"
	if ! "$STRIP" --strip-unneeded "$ELF"; then
		echo "FDPIC strip: failed: $REL" >&2
		exit 1
	fi
	AFTER="$(stat -c %s "$ELF")"
	STRIPPED=$((STRIPPED + 1))
	SAVED=$((SAVED + BEFORE - AFTER))
done < "$STRIP_LIST"

rm -f "$STRIP_LIST"
trap - 0 1 2 15

echo "FDPIC strip: $STRIPPED ELF files, $SAVED bytes removed"
