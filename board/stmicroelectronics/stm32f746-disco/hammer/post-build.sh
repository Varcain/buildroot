#!/bin/sh
set -eu

board_dir="$(dirname "$0")"

install -m 0644 -D "${board_dir}/extlinux.conf" \
	"${TARGET_DIR}/boot/extlinux/extlinux.conf"

# Host keys are generated into tmpfs at boot; keep the measured rootfs read-only.
rm -rf "${TARGET_DIR}/etc/dropbear"
ln -s /run/dropbear "${TARGET_DIR}/etc/dropbear"

chmod 0700 "${TARGET_DIR}/root/.ssh"
chmod 0600 "${TARGET_DIR}/root/.ssh/authorized_keys"
