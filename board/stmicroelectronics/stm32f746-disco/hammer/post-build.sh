#!/bin/sh
set -eu

board_dir="$(dirname "$0")"

install -m 0644 -D "${board_dir}/extlinux.conf" \
	"${TARGET_DIR}/boot/extlinux/extlinux.conf"

# The host key is persisted on /data and linked into this tmpfs directory at
# boot, keeping both the measured rootfs read-only and SSH identity stable.
rm -rf "${TARGET_DIR}/etc/dropbear"
ln -s /run/dropbear "${TARGET_DIR}/etc/dropbear"

chmod 0700 "${TARGET_DIR}/root/.ssh"
chmod 0600 "${TARGET_DIR}/root/.ssh/authorized_keys"
