#!/bin/bash
# Back up, program, and verify the complete on-board 16 MiB QSPI NOR image.
set -euo pipefail

output_dir=${1:-output-qspi}
image="${output_dir}/images/qspi-linux.img"
openocd="${output_dir}/host/bin/openocd"
scripts="${output_dir}/host/share/openocd/scripts"
board_cfg=board/stm32f746g-disco.cfg
qspi_bank=3
qspi_size=$((16 * 1024 * 1024))
backup_dir="${output_dir}/hardware-backup"

[ -f "$image" ] || { echo "not found: $image" >&2; exit 1; }
[ -x "$openocd" ] || { echo "not found: $openocd" >&2; exit 1; }
[ "$(stat -c%s "$image")" -eq "$qspi_size" ] || {
	echo "refusing non-16 MiB QSPI image: $image" >&2
	exit 1
}

mkdir -p "$backup_dir"
timestamp=$(date -u +%Y%m%dT%H%M%SZ)
backup="${backup_dir}/qspi-before-linux-${timestamp}.bin"

echo "Backing up existing QSPI to $backup"
"$openocd" -s "$scripts" -f "$board_cfg" \
	-c init -c "reset init" -c "flash probe $qspi_bank" \
	-c "flash read_bank $qspi_bank $backup 0 $qspi_size" \
	-c "reset run" -c shutdown
[ "$(stat -c%s "$backup")" -eq "$qspi_size" ]
sha256sum "$backup" >"${backup}.sha256"

echo "Programming and verifying $image at QSPI 0x90000000"
"$openocd" -s "$scripts" -f "$board_cfg" \
	-c init -c "reset init" -c "flash probe $qspi_bank" \
	-c "flash erase_address pad 0x90000000 $qspi_size" \
	-c "flash write_bank $qspi_bank $image 0" \
	-c "flash verify_bank $qspi_bank $image 0" \
	-c "reset run" -c shutdown

echo "QSPI program and verification completed"
