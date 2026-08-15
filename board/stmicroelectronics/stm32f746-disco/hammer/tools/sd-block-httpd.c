// SPDX-License-Identifier: MIT
/*
 * Tiny single-client HTTP block proxy for low-memory maintenance.
 *
 * The target block device must be unmounted.  Requests are deliberately
 * bounded to 1 MiB and the daemon binds an explicit benchmark-side address.
 */
#define _FILE_OFFSET_BITS 64

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define HEADER_SIZE 4096
#define IO_SIZE 16384
#define MAX_REQUEST (1024U * 1024U)

static volatile sig_atomic_t stopping;

static void stop_handler(int signal_number)
{
	(void)signal_number;
	stopping = 1;
}

static int install_stop_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = stop_handler;
	sigemptyset(&action.sa_mask);
	/* Keep accept(2) interruptible so SIGTERM can stop an idle server. */
	if (sigaction(SIGINT, &action, NULL) ||
	    sigaction(SIGTERM, &action, NULL))
		return -1;
	return 0;
}

static int send_all(int fd, const void *data, size_t length)
{
	const unsigned char *cursor = data;

	while (length) {
		ssize_t sent = send(fd, cursor, length, 0);

		if (sent < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!sent)
			return -1;
		cursor += sent;
		length -= (size_t)sent;
	}
	return 0;
}

static int response(int client, int code, const char *reason,
			const char *type, const void *body, size_t body_length)
{
	char header[256];
	int length = snprintf(header, sizeof(header),
		"HTTP/1.0 %d %s\r\nContent-Type: %s\r\n"
		"Content-Length: %zu\r\nConnection: close\r\n\r\n",
		code, reason, type, body_length);

	if (length < 0 || (size_t)length >= sizeof(header))
		return -1;
	if (send_all(client, header, (size_t)length))
		return -1;
	return body_length ? send_all(client, body, body_length) : 0;
}

static int error_response(int client, int code, const char *reason)
{
	return response(client, code, reason, "text/plain", reason,
			strlen(reason));
}

static int parse_u64(const char *path, const char *name, uint64_t *value)
{
	char needle[32];
	const char *cursor;
	char *end;
	unsigned long long parsed;

	if (snprintf(needle, sizeof(needle), "%s=", name) >= (int)sizeof(needle))
		return -1;
	cursor = strstr(path, needle);
	if (!cursor)
		return -1;
	cursor += strlen(needle);
	errno = 0;
	parsed = strtoull(cursor, &end, 10);
	if (errno || end == cursor || (*end && *end != '&'))
		return -1;
	*value = (uint64_t)parsed;
	return 0;
}

static int read_request(int client, char *header, size_t capacity,
			unsigned char **body, size_t *body_length)
{
	size_t used = 0;

	while (used + 1 < capacity) {
		ssize_t received = recv(client, header + used, capacity - used - 1, 0);
		char *end;

		if (received < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!received)
			return -1;
		used += (size_t)received;
		header[used] = '\0';
		end = strstr(header, "\r\n\r\n");
		if (end) {
			size_t offset = (size_t)(end + 4 - header);

			*body = (unsigned char *)header + offset;
			*body_length = used - offset;
			return 0;
		}
	}
	return -1;
}

static int stream_read(int client, int device, uint64_t offset, uint64_t length)
{
	unsigned char buffer[IO_SIZE];
	char header[256];
	int header_length = snprintf(header, sizeof(header),
		"HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
		"Content-Length: %llu\r\nConnection: close\r\n\r\n",
		(unsigned long long)length);

	if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
	    send_all(client, header, (size_t)header_length))
		return -1;
	while (length) {
		size_t amount = length > sizeof(buffer) ? sizeof(buffer) : (size_t)length;
		ssize_t received = pread(device, buffer, amount, (off_t)offset);

		if (received < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if ((size_t)received != amount || send_all(client, buffer, amount))
			return -1;
		offset += amount;
		length -= amount;
	}
	return 0;
}

static void handle_client(int client, int device, uint64_t device_size)
{
	char header[HEADER_SIZE];
	char method[8];
	char path[1024];
	unsigned char *ignored_body;
	size_t ignored_body_length;
	uint64_t offset, length;

	if (read_request(client, header, sizeof(header), &ignored_body,
			 &ignored_body_length)) {
		error_response(client, 400, "bad request");
		return;
	}
	if (sscanf(header, "%7s %1023s", method, path) != 2) {
		error_response(client, 400, "bad request line");
		return;
	}
	if (!strcmp(method, "GET") && !strcmp(path, "/health")) {
		response(client, 200, "OK", "text/plain", "ready\n", 6);
		return;
	}
	if (!strcmp(method, "GET") && !strcmp(path, "/size")) {
		char body[32];
		int body_length = snprintf(body, sizeof(body), "%llu\n",
			(unsigned long long)device_size);

		response(client, 200, "OK", "text/plain", body,
			 (size_t)body_length);
		return;
	}
	if (parse_u64(path, "offset", &offset) ||
	    parse_u64(path, "length", &length) || !length ||
	    length > MAX_REQUEST || offset > device_size ||
	    length > device_size - offset) {
		error_response(client, 416, "invalid block range");
		return;
	}
	if (!strcmp(method, "GET") && !strncmp(path, "/read?", 6)) {
		stream_read(client, device, offset, length);
		return;
	}
	error_response(client, 404, "not found");
}

int main(int argc, char **argv)
{
	struct sockaddr_in address = { .sin_family = AF_INET };
	struct in_addr trusted_peer;
	uint64_t device_size;
	int device, listener, one = 1;
	long port;
	char *port_end;

	if (argc != 5) {
		fprintf(stderr, "usage: %s DEVICE BIND_ADDRESS PORT TRUSTED_PEER\n",
			argv[0]);
		return 2;
	}
	errno = 0;
	port = strtol(argv[3], &port_end, 10);
	if (errno || *port_end || port < 1 || port > 65535 ||
	    inet_pton(AF_INET, argv[2], &address.sin_addr) != 1 ||
	    inet_pton(AF_INET, argv[4], &trusted_peer) != 1) {
		fprintf(stderr, "invalid bind address or port\n");
		return 2;
	}
	address.sin_port = htons((uint16_t)port);
	device = open(argv[1], O_RDONLY | O_CLOEXEC);
	if (device < 0) {
		perror("open block device");
		return 1;
	}
	if (ioctl(device, BLKGETSIZE64, &device_size)) {
		perror("BLKGETSIZE64");
		close(device);
		return 1;
	}
	listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one,
				       sizeof(one)) ||
	    bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
	    listen(listener, 4)) {
		perror("listen");
		close(device);
		return 1;
	}
	if (install_stop_handlers()) {
		perror("install signal handlers");
		close(listener);
		close(device);
		return 1;
	}
	fprintf(stderr, "ready: %s:%ld %s %llu bytes\n", argv[2], port, argv[1],
		(unsigned long long)device_size);
	while (!stopping) {
		struct sockaddr_in peer;
		socklen_t peer_length = sizeof(peer);
		int client = accept(listener, (struct sockaddr *)&peer, &peer_length);

		if (client < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			break;
		}
		if (peer.sin_family != AF_INET ||
		    peer.sin_addr.s_addr != trusted_peer.s_addr) {
			error_response(client, 403, "untrusted peer");
			close(client);
			continue;
		}
		handle_client(client, device, device_size);
		close(client);
	}
	close(listener);
	close(device);
	return 0;
}
