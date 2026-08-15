// SPDX-License-Identifier: MIT
/* Apply a bounded, preimage-verified FAT repair bundle to an unmounted device. */
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BUNDLE_HEADER_SIZE 36U
#define RECORD_HEADER_SIZE 20U
#define MAX_RECORD_SIZE (64U * 1024U)
#define MAX_RECORDS 131072U
#define MAX_CHANGED_BYTES (64ULL * 1024ULL * 1024ULL)

static unsigned char buffer[MAX_RECORD_SIZE];

static uint32_t get_le32(const unsigned char *data)
{
	return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
		(uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint64_t get_le64(const unsigned char *data)
{
	return (uint64_t)get_le32(data) | (uint64_t)get_le32(data + 4) << 32;
}

static uint32_t crc32_bytes(const unsigned char *data, size_t length)
{
	uint32_t crc = 0xffffffffU;
	size_t index;

	for (index = 0; index < length; index++) {
		unsigned int bit;

		crc ^= data[index];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
	}
	return ~crc;
}

static int read_full(int fd, void *data, size_t length)
{
	unsigned char *cursor = data;

	while (length) {
		ssize_t received = read(fd, cursor, length);

		if (received < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!received)
			return -1;
		cursor += received;
		length -= (size_t)received;
	}
	return 0;
}

static int pread_full(int fd, void *data, size_t length, uint64_t offset)
{
	unsigned char *cursor = data;

	while (length) {
		ssize_t received = pread(fd, cursor, length, (off_t)offset);

		if (received < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!received)
			return -1;
		cursor += received;
		offset += (uint64_t)received;
		length -= (size_t)received;
	}
	return 0;
}

static int pwrite_full(int fd, const void *data, size_t length, uint64_t offset)
{
	const unsigned char *cursor = data;

	while (length) {
		ssize_t written = pwrite(fd, cursor, length, (off_t)offset);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!written)
			return -1;
		cursor += written;
		offset += (uint64_t)written;
		length -= (size_t)written;
	}
	return 0;
}

static int device_is_mounted(const char *device)
{
	FILE *mounts = fopen("/proc/mounts", "r");
	char source[256], target[256], type[64], options[256];
	int mounted = 0;

	if (!mounts)
		return -1;
	while (fscanf(mounts, "%255s %255s %63s %255s %*d %*d",
		      source, target, type, options) == 4) {
		if (!strcmp(source, device)) {
			mounted = 1;
			break;
		}
	}
	fclose(mounts);
	return mounted;
}

struct bundle_info {
	uint64_t device_size;
	uint64_t filesystem_size;
	uint32_t record_count;
	uint64_t changed_bytes;
};

static int read_bundle_header(int bundle, struct bundle_info *info)
{
	unsigned char header[BUNDLE_HEADER_SIZE];

	if (lseek(bundle, 0, SEEK_SET) < 0 || read_full(bundle, header, sizeof(header)))
		return -1;
	if (memcmp(header, "HIRFAT1\0", 8)) {
		errno = EINVAL;
		return -1;
	}
	info->device_size = get_le64(header + 8);
	info->filesystem_size = get_le64(header + 16);
	info->record_count = get_le32(header + 24);
	info->changed_bytes = get_le64(header + 28);
	if (info->record_count > MAX_RECORDS ||
	    info->changed_bytes > MAX_CHANGED_BYTES ||
	    info->filesystem_size > info->device_size) {
		errno = EFBIG;
		return -1;
	}
	return 0;
}

static int verify_preimages(int bundle, int device, const struct bundle_info *info)
{
	unsigned char record[RECORD_HEADER_SIZE];
	uint64_t previous_end = 0, total = 0;
	uint32_t index;

	if (lseek(bundle, BUNDLE_HEADER_SIZE, SEEK_SET) < 0)
		return -1;
	for (index = 0; index < info->record_count; index++) {
		uint64_t offset;
		uint32_t length, before_crc, after_crc;

		if (read_full(bundle, record, sizeof(record)))
			return -1;
		offset = get_le64(record);
		length = get_le32(record + 8);
		before_crc = get_le32(record + 12);
		after_crc = get_le32(record + 16);
		if (!length || length > MAX_RECORD_SIZE || length % 512 ||
		    offset % 512 || offset < previous_end ||
		    offset > info->filesystem_size ||
		    length > info->filesystem_size - offset) {
			errno = EINVAL;
			return -1;
		}
		if (read_full(bundle, buffer, length) ||
		    crc32_bytes(buffer, length) != after_crc) {
			errno = EBADMSG;
			return -1;
		}
		if (pread_full(device, buffer, length, offset) ||
		    crc32_bytes(buffer, length) != before_crc) {
			fprintf(stderr, "preimage mismatch at offset %llu length %u\n",
				(unsigned long long)offset, length);
			errno = ESTALE;
			return -1;
		}
		previous_end = offset + length;
		total += length;
	}
	if (total != info->changed_bytes) {
		errno = EINVAL;
		return -1;
	}
	if (read(bundle, record, 1) != 0) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int apply_records(int bundle, int device, const struct bundle_info *info)
{
	unsigned char record[RECORD_HEADER_SIZE];
	uint32_t index;

	if (lseek(bundle, BUNDLE_HEADER_SIZE, SEEK_SET) < 0)
		return -1;
	for (index = 0; index < info->record_count; index++) {
		uint64_t offset;
		uint32_t length;

		if (read_full(bundle, record, sizeof(record)))
			return -1;
		offset = get_le64(record);
		length = get_le32(record + 8);
		if (read_full(bundle, buffer, length) ||
		    pwrite_full(device, buffer, length, offset))
			return -1;
	}
	return fsync(device);
}

static int verify_postimages(int bundle, int device, const struct bundle_info *info)
{
	unsigned char record[RECORD_HEADER_SIZE];
	uint32_t index;

	if (lseek(bundle, BUNDLE_HEADER_SIZE, SEEK_SET) < 0)
		return -1;
	for (index = 0; index < info->record_count; index++) {
		uint64_t offset;
		uint32_t length, after_crc;

		if (read_full(bundle, record, sizeof(record)))
			return -1;
		offset = get_le64(record);
		length = get_le32(record + 8);
		after_crc = get_le32(record + 16);
		if (lseek(bundle, length, SEEK_CUR) < 0 ||
		    pread_full(device, buffer, length, offset) ||
		    crc32_bytes(buffer, length) != after_crc) {
			fprintf(stderr, "postimage mismatch at offset %llu length %u\n",
				(unsigned long long)offset, length);
			errno = EIO;
			return -1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct bundle_info info;
	uint64_t device_size;
	int mounted, bundle, device;

	if (argc != 3) {
		fprintf(stderr, "usage: %s DEVICE REPAIR_BUNDLE\n", argv[0]);
		return 2;
	}
	mounted = device_is_mounted(argv[1]);
	if (mounted < 0) {
		perror("read /proc/mounts");
		return 1;
	}
	if (mounted) {
		fprintf(stderr, "refusing mounted device: %s\n", argv[1]);
		return 1;
	}
	bundle = open(argv[2], O_RDONLY | O_CLOEXEC);
	device = open(argv[1], O_RDWR | O_CLOEXEC);
	if (bundle < 0 || device < 0) {
		perror("open");
		return 1;
	}
	if (read_bundle_header(bundle, &info) ||
	    ioctl(device, BLKGETSIZE64, &device_size) ||
	    device_size != info.device_size) {
		perror("validate bundle/device");
		return 1;
	}
	printf("bundle records=%u changed_bytes=%llu filesystem_bytes=%llu\n",
		info.record_count, (unsigned long long)info.changed_bytes,
		(unsigned long long)info.filesystem_size);
	if (verify_preimages(bundle, device, &info)) {
		perror("verify all preimages");
		return 1;
	}
	puts("all preimages verified; applying bounded repair");
	if (apply_records(bundle, device, &info)) {
		perror("apply repair");
		return 1;
	}
	if (verify_postimages(bundle, device, &info)) {
		perror("verify all postimages");
		return 1;
	}
	puts("repair applied, synced, and read back successfully");
	close(device);
	close(bundle);
	return 0;
}
