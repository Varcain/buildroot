#!/usr/bin/env python3
"""Repair an unmounted FAT volume through the STM32 low-memory block proxy.

Only FAT metadata and clusters marked allocated by the active FAT are copied
into a sparse image.  fsck.fat runs on the maintenance host and this script
emits a bounded, preimage-verified repair bundle; it never writes the device.
The bundle must be applied locally with sd-fat-apply while it is unmounted.
"""

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import time
import urllib.request
import zlib
from pathlib import Path


SECTOR = 512
HTTP_CHUNK = 1024 * 1024


def request(url, method="GET", data=None, timeout=120):
    req = urllib.request.Request(url, data=data, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return response.read()


def get_range(base_url, offset, length):
    return request(f"{base_url}/read?offset={offset}&length={length}")


def chunks(offset, length, maximum=HTTP_CHUNK):
    while length:
        amount = min(length, maximum)
        yield offset, amount
        offset += amount
        length -= amount


def copy_from_board(base_url, destinations, offset, length, progress):
    for chunk_offset, amount in chunks(offset, length):
        payload = get_range(base_url, chunk_offset, amount)
        if len(payload) != amount:
            raise RuntimeError(
                f"short proxy read at {chunk_offset}: {len(payload)} != {amount}"
            )
        for destination in destinations:
            os.pwrite(destination, payload, chunk_offset)
        progress[0] += amount
        if progress[0] >= progress[1]:
            print(f"fetched_bytes={progress[0]}", flush=True)
            progress[1] += 8 * 1024 * 1024


def parse_boot_sector(boot):
    if len(boot) != SECTOR or boot[510:512] != b"\x55\xaa":
        raise RuntimeError("invalid FAT boot-sector signature")
    bytes_per_sector = struct.unpack_from("<H", boot, 11)[0]
    sectors_per_cluster = boot[13]
    reserved_sectors = struct.unpack_from("<H", boot, 14)[0]
    fat_count = boot[16]
    total16 = struct.unpack_from("<H", boot, 19)[0]
    total32 = struct.unpack_from("<I", boot, 32)[0]
    sectors_per_fat16 = struct.unpack_from("<H", boot, 22)[0]
    sectors_per_fat32 = struct.unpack_from("<I", boot, 36)[0]
    ext_flags = struct.unpack_from("<H", boot, 40)[0]
    root_cluster = struct.unpack_from("<I", boot, 44)[0]
    total_sectors = total16 or total32
    sectors_per_fat = sectors_per_fat16 or sectors_per_fat32
    if bytes_per_sector != SECTOR:
        raise RuntimeError(f"unsupported logical sector size {bytes_per_sector}")
    if (
        not sectors_per_cluster
        or sectors_per_cluster & (sectors_per_cluster - 1)
        or not reserved_sectors
        or not fat_count
        or not total_sectors
        or not sectors_per_fat
    ):
        raise RuntimeError("invalid FAT geometry")
    active_fat = (ext_flags & 0xF) if ext_flags & 0x80 else 0
    if active_fat >= fat_count:
        raise RuntimeError(f"invalid active FAT index {active_fat}")
    data_start_sector = reserved_sectors + fat_count * sectors_per_fat
    data_clusters = (total_sectors - data_start_sector) // sectors_per_cluster
    return {
        "bytes_per_sector": bytes_per_sector,
        "sectors_per_cluster": sectors_per_cluster,
        "reserved_sectors": reserved_sectors,
        "fat_count": fat_count,
        "sectors_per_fat": sectors_per_fat,
        "active_fat": active_fat,
        "root_cluster": root_cluster,
        "total_sectors": total_sectors,
        "total_bytes": total_sectors * bytes_per_sector,
        "data_start_sector": data_start_sector,
        "data_clusters": data_clusters,
    }


def allocated_cluster_ranges(image_fd, geometry):
    fat_offset = (
        geometry["reserved_sectors"]
        + geometry["active_fat"] * geometry["sectors_per_fat"]
    ) * SECTOR
    fat_length = geometry["sectors_per_fat"] * SECTOR
    allocated = []
    fat = os.pread(image_fd, fat_length, fat_offset)
    if len(fat) != fat_length:
        raise RuntimeError("short active FAT in sparse image")
    maximum = min(geometry["data_clusters"] + 2, fat_length // 4)
    for cluster in range(2, maximum):
        value = struct.unpack_from("<I", fat, cluster * 4)[0] & 0x0FFFFFFF
        if value and value != 0x0FFFFFF7:
            allocated.append(cluster)
    ranges = []
    for cluster in allocated:
        if ranges and cluster == ranges[-1][1]:
            ranges[-1] = (ranges[-1][0], cluster + 1)
        else:
            ranges.append((cluster, cluster + 1))
    return allocated, ranges


def cluster_byte_range(geometry, first, end):
    cluster_bytes = geometry["sectors_per_cluster"] * SECTOR
    data_start = geometry["data_start_sector"] * SECTOR
    return data_start + (first - 2) * cluster_bytes, (end - first) * cluster_bytes


def run_fsck(fsck, mode, image, log_path):
    command = [str(fsck), mode, "-v", str(image)]
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log_path.write_bytes(completed.stdout)
    print(f"{' '.join(command)} exit={completed.returncode}", flush=True)
    sys.stdout.buffer.write(completed.stdout)
    sys.stdout.buffer.flush()
    return completed.returncode


def file_extents(fd, size):
    if not hasattr(os, "SEEK_DATA"):
        raise RuntimeError("maintenance host lacks SEEK_DATA")
    position = 0
    while position < size:
        try:
            start = os.lseek(fd, position, os.SEEK_DATA)
        except OSError as error:
            if error.errno == 6:  # ENXIO: no more data
                return
            raise
        end = os.lseek(fd, start, os.SEEK_HOLE)
        yield start, min(end, size)
        position = end


def merge_ranges(ranges):
    merged = []
    for start, end in sorted(ranges):
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(end, merged[-1][1]))
        else:
            merged.append((start, end))
    return merged


def changed_sector_ranges(original_fd, repaired_fd, size):
    extents = merge_ranges(
        list(file_extents(original_fd, size)) + list(file_extents(repaired_fd, size))
    )
    changed = []
    for extent_start, extent_end in extents:
        start = extent_start // SECTOR * SECTOR
        end = min(size, (extent_end + SECTOR - 1) // SECTOR * SECTOR)
        for offset, amount in chunks(start, end - start):
            before = os.pread(original_fd, amount, offset)
            after = os.pread(repaired_fd, amount, offset)
            for inner in range(0, amount, SECTOR):
                sector_offset = offset + inner
                if before[inner : inner + SECTOR] == after[inner : inner + SECTOR]:
                    continue
                if changed and changed[-1][0] + changed[-1][1] == sector_offset:
                    changed[-1] = (changed[-1][0], changed[-1][1] + SECTOR)
                else:
                    changed.append((sector_offset, SECTOR))
    return changed


def write_repair_bundle(path, original_fd, repaired_fd, device_size, total_bytes, changed):
    records = []
    for offset, length in changed:
        for chunk_offset, amount in chunks(offset, length, 64 * 1024):
            before = os.pread(original_fd, amount, chunk_offset)
            after = os.pread(repaired_fd, amount, chunk_offset)
            records.append(
                (
                    chunk_offset,
                    before,
                    after,
                    zlib.crc32(before) & 0xFFFFFFFF,
                    zlib.crc32(after) & 0xFFFFFFFF,
                )
            )
    changed_bytes = sum(len(after) for _, _, after, _, _ in records)
    with path.open("wb") as bundle:
        bundle.write(
            struct.pack(
                "<8sQQIQ",
                b"HIRFAT1\0",
                device_size,
                total_bytes,
                len(records),
                changed_bytes,
            )
        )
        for offset, _before, after, before_crc, after_crc in records:
            bundle.write(
                struct.pack("<QIII", offset, len(after), before_crc, after_crc)
            )
            bundle.write(after)
    return len(records), changed_bytes, hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://172.1.1.2:8090")
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--fsck", default="/usr/sbin/fsck.fat", type=Path)
    parser.add_argument("--max-allocated-mib", type=int, default=1024)
    parser.add_argument("--max-write-mib", type=int, default=64)
    args = parser.parse_args()

    started = time.time()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    image_path = args.work_dir / "volume.repaired.img"
    original_path = args.work_dir / "volume.original.img"
    result_path = args.work_dir / "result.json"
    proxy_size = int(request(args.base_url + "/size").strip())
    boot = get_range(args.base_url, 0, SECTOR)
    geometry = parse_boot_sector(boot)
    if geometry["total_bytes"] > proxy_size:
        raise RuntimeError(
            f"filesystem size {geometry['total_bytes']} exceeds block device {proxy_size}"
        )
    print(json.dumps(geometry, sort_keys=True), flush=True)

    image_fd = os.open(image_path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o600)
    original_fd = os.open(original_path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.ftruncate(image_fd, geometry["total_bytes"])
        os.ftruncate(original_fd, geometry["total_bytes"])
        progress = [0, 8 * 1024 * 1024]
        metadata_length = geometry["data_start_sector"] * SECTOR
        copy_from_board(
            args.base_url, (image_fd, original_fd), 0, metadata_length, progress
        )
        copy_from_board(
            args.base_url,
            (image_fd, original_fd),
            geometry["total_bytes"] - SECTOR,
            SECTOR,
            progress,
        )
        allocated, allocated_ranges = allocated_cluster_ranges(image_fd, geometry)
        allocated_bytes = len(allocated) * geometry["sectors_per_cluster"] * SECTOR
        print(
            f"allocated_clusters={len(allocated)} allocated_bytes={allocated_bytes} "
            f"ranges={len(allocated_ranges)}",
            flush=True,
        )
        if allocated_bytes > args.max_allocated_mib * 1024 * 1024:
            raise RuntimeError(
                f"allocated data {allocated_bytes} exceeds safety cap "
                f"{args.max_allocated_mib} MiB"
            )
        for first, end in allocated_ranges:
            offset, length = cluster_byte_range(geometry, first, end)
            copy_from_board(
                args.base_url, (image_fd, original_fd), offset, length, progress
            )
        os.fsync(image_fd)
        os.fsync(original_fd)

        repair_exit = run_fsck(
            args.fsck, "-a", image_path, args.work_dir / "fsck-repair.log"
        )
        if repair_exit not in (0, 1):
            raise RuntimeError(f"fsck.fat repair failed with exit {repair_exit}")
        verify_exit = run_fsck(
            args.fsck, "-n", image_path, args.work_dir / "fsck-verify.log"
        )
        if verify_exit != 0:
            raise RuntimeError(f"repaired image verification failed with exit {verify_exit}")

        changed = changed_sector_ranges(
            original_fd, image_fd, geometry["total_bytes"]
        )
        changed_bytes = sum(length for _, length in changed)
        plan = {
            "geometry": geometry,
            "allocated_clusters": len(allocated),
            "allocated_bytes": allocated_bytes,
            "changed_ranges": [
                {"offset": offset, "length": length} for offset, length in changed
            ],
            "changed_bytes": changed_bytes,
            "repair_exit": repair_exit,
            "verify_exit": verify_exit,
        }
        (args.work_dir / "repair-plan.json").write_text(
            json.dumps(plan, indent=2, sort_keys=True) + "\n"
        )
        print(json.dumps(plan, indent=2, sort_keys=True), flush=True)
        if changed_bytes > args.max_write_mib * 1024 * 1024:
            raise RuntimeError(
                f"repair writes {changed_bytes} exceed safety cap "
                f"{args.max_write_mib} MiB"
            )
        bundle_path = args.work_dir / "repair.bundle"
        record_count, bundle_bytes, bundle_sha256 = write_repair_bundle(
            bundle_path,
            original_fd,
            image_fd,
            proxy_size,
            geometry["total_bytes"],
            changed,
        )
        result = {
            **plan,
            "bundle": str(bundle_path),
            "bundle_records": record_count,
            "bundle_payload_bytes": bundle_bytes,
            "bundle_sha256": bundle_sha256,
            "elapsed_s": time.time() - started,
            "status": "NO_CHANGES" if not changed else "PLAN_READY",
        }
        result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        print(f"result={result_path} status={result['status']}", flush=True)
    finally:
        os.close(original_fd)
        os.close(image_fd)


if __name__ == "__main__":
    main()
