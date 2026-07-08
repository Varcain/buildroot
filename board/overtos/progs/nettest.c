/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS Linux-personality socket smoke test (P0 networking).
 *
 * A stock uClibc BSD-socket client: create a TCP socket, connect to <ip> <port>,
 * send a line, read the echo, print it. Exercises the personality's socket bridge
 * on real silicon (socket/connect/send/recv → ove_net → lwIP + the STM32 Ethernet
 * driver), including the coordinator park/retry for the blocking connect + recv.
 *
 *   nettest <ip> <port> [message]      # default message: "hello from overtos"
 *
 * Prints "nettest: DONE" on a successful round-trip, "nettest: FAIL ..." otherwise,
 * so a drive script can grep the console. On a connect error it prints the errno so
 * a link-down / no-peer run still reports the on-silicon syscall path is alive.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 3) {
		printf("nettest: FAIL usage: nettest <ip> <port> [message]\n");
		return 2;
	}
	const char *ip = argv[1];
	int port = atoi(argv[2]);
	const char *msg = (argc > 3) ? argv[3] : "hello from overtos";

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		printf("nettest: FAIL socket errno=%d\n", errno);
		return 1;
	}
	printf("nettest: socket ok fd=%d\n", fd);

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
		printf("nettest: FAIL bad ip '%s'\n", ip);
		close(fd);
		return 1;
	}

	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		printf("nettest: FAIL connect %s:%d errno=%d\n", ip, port, errno);
		close(fd);
		return 1;
	}
	printf("nettest: connect ok %s:%d\n", ip, port);

	size_t mlen = strlen(msg);
	ssize_t s = send(fd, msg, mlen, 0);
	if (s < 0) {
		printf("nettest: FAIL send errno=%d\n", errno);
		close(fd);
		return 1;
	}
	printf("nettest: sent %d bytes\n", (int)s);

	char buf[256];
	ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
	if (r < 0) {
		printf("nettest: FAIL recv errno=%d\n", errno);
		close(fd);
		return 1;
	}
	buf[r > 0 ? r : 0] = '\0';
	printf("nettest: recv %d bytes: %s\n", (int)r, buf);

	close(fd);
	printf("nettest: DONE\n");
	return 0;
}
