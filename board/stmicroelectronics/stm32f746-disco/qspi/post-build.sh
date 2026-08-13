#!/bin/sh
set -eu

# The kernel and DTB live in raw QSPI slots and must not also consume space in
# the read-only root filesystem.
rm -rf "${TARGET_DIR}/boot"
