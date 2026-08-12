// SPDX-License-Identifier: MIT
/* FIFO-controlled absolute touchscreen used to automate the LVGL Play tap. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEVICE_NAME "native-linux-hammer-uinput"

static int safe_runtime_path(const char *path)
{
	return path != NULL && strncmp(path, "/run/", 5) == 0 &&
	       path[5] != '\0';
}

static int emit_event(int fd, unsigned short type, unsigned short code,
		      int value)
{
	struct input_event event;

	memset(&event, 0, sizeof(event));
	event.type = type;
	event.code = code;
	event.value = value;
	return write(fd, &event, sizeof(event)) == sizeof(event) ? 0 : -1;
}

static int sync_event(int fd)
{
	return emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

static int tap(int fd, int x, int y, unsigned int hold_ms)
{
	struct timespec hold = {
		.tv_sec = hold_ms / 1000U,
		.tv_nsec = (long)(hold_ms % 1000U) * 1000000L,
	};

	if (x < 0 || x >= 480 || y < 0 || y >= 272)
		return -1;
	if (emit_event(fd, EV_ABS, ABS_X, x) != 0 ||
	    emit_event(fd, EV_ABS, ABS_Y, y) != 0 ||
	    emit_event(fd, EV_KEY, BTN_TOUCH, 1) != 0 || sync_event(fd) != 0)
		return -1;
	while (nanosleep(&hold, &hold) != 0 && errno == EINTR)
		;
	if (emit_event(fd, EV_KEY, BTN_TOUCH, 0) != 0 || sync_event(fd) != 0)
		return -1;
	return 0;
}

static int device_name_matches(const char *event)
{
	char path[256];
	char name[128];
	FILE *stream;

	if (snprintf(path, sizeof(path), "/sys/class/input/%s/device/name", event) >=
	    (int)sizeof(path))
		return 0;
	stream = fopen(path, "r");
	if (stream == NULL)
		return 0;
	if (fgets(name, sizeof(name), stream) == NULL) {
		fclose(stream);
		return 0;
	}
	fclose(stream);
	name[strcspn(name, "\r\n")] = '\0';
	return strcmp(name, DEVICE_NAME) == 0;
}

static int find_event(char *path, size_t capacity)
{
	struct timespec delay = { .tv_nsec = 20000000L };
	int attempt;

	for (attempt = 0; attempt < 500; ++attempt) {
		DIR *directory = opendir("/sys/class/input");
		struct dirent *entry;

		if (directory != NULL) {
			while ((entry = readdir(directory)) != NULL) {
				if (strncmp(entry->d_name, "event", 5) == 0 &&
				    device_name_matches(entry->d_name)) {
					int length = snprintf(path, capacity, "/dev/input/%s",
							      entry->d_name);
					closedir(directory);
					return length > 0 && (size_t)length < capacity ? 0 : -1;
				}
			}
			closedir(directory);
		}
		nanosleep(&delay, NULL);
	}
	return -1;
}

static int write_event_path(const char *file, const char *event)
{
	FILE *stream = fopen(file, "w");

	if (stream == NULL)
		return -1;
	if (fprintf(stream, "%s\n", event) < 0 || fclose(stream) != 0)
		return -1;
	return 0;
}

static int create_device(void)
{
	struct uinput_setup setup;
	struct uinput_abs_setup absolute;
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

	if (fd < 0)
		return -1;
	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) != 0 ||
	    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) != 0 ||
	    ioctl(fd, UI_SET_EVBIT, EV_ABS) != 0) {
		close(fd);
		return -1;
	}
	memset(&absolute, 0, sizeof(absolute));
	absolute.code = ABS_X;
	absolute.absinfo.maximum = 479;
	if (ioctl(fd, UI_ABS_SETUP, &absolute) != 0) {
		close(fd);
		return -1;
	}
	absolute.code = ABS_Y;
	absolute.absinfo.maximum = 271;
	if (ioctl(fd, UI_ABS_SETUP, &absolute) != 0) {
		close(fd);
		return -1;
	}
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x0483;
	setup.id.product = 0x0746;
	strncpy(setup.name, DEVICE_NAME, UINPUT_MAX_NAME_SIZE - 1);
	if (ioctl(fd, UI_DEV_SETUP, &setup) != 0 || ioctl(fd, UI_DEV_CREATE) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char **argv)
{
	char event[128];
	char command[128];
	FILE *control;
	int device;
	int fifo;

	if (argc != 3 || !safe_runtime_path(argv[1]) ||
	    !safe_runtime_path(argv[2])) {
		fprintf(stderr, "usage: hammer-uinput /run/event-file /run/control-fifo\n");
		return EXIT_FAILURE;
	}
	device = create_device();
	if (device < 0) {
		perror("hammer-uinput: create /dev/uinput");
		return EXIT_FAILURE;
	}
	if (find_event(event, sizeof(event)) != 0) {
		fprintf(stderr, "hammer-uinput: event node did not appear\n");
		close(device);
		return EXIT_FAILURE;
	}
	unlink(argv[1]);
	unlink(argv[2]);
	if (mkfifo(argv[2], 0600) != 0 || write_event_path(argv[1], event) != 0) {
		perror("hammer-uinput: publish control paths");
		close(device);
		return EXIT_FAILURE;
	}
	fifo = open(argv[2], O_RDWR);
	if (fifo < 0 || (control = fdopen(fifo, "r")) == NULL) {
		perror("hammer-uinput: open control FIFO");
		close(device);
		return EXIT_FAILURE;
	}
	printf("hammer-uinput: event=%s control=%s\n", event, argv[2]);
	fflush(stdout);
	while (fgets(command, sizeof(command), control) != NULL) {
		int x;
		int y;
		unsigned int hold_ms;

		if (sscanf(command, "tap %d %d %u", &x, &y, &hold_ms) == 3) {
			if (hold_ms > 10000U || tap(device, x, y, hold_ms) != 0)
				fprintf(stderr, "hammer-uinput: rejected tap: %s", command);
		} else if (strncmp(command, "quit", 4) == 0) {
			break;
		} else {
			fprintf(stderr, "hammer-uinput: unknown command: %s", command);
		}
		fflush(stderr);
	}
	fclose(control);
	ioctl(device, UI_DEV_DESTROY);
	close(device);
	unlink(argv[1]);
	unlink(argv[2]);
	return EXIT_SUCCESS;
}
