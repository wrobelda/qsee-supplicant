// SPDX-License-Identifier: BSD-2-Clause
#include "notify.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void check_socket(int abstract)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	char name[sizeof(address.sun_path)];
	char message[64] = {};
	socklen_t length;
	int fd;

	snprintf(name, sizeof(name), "%sqs-notify-test-%ld",
		 abstract ? "@" : "/tmp/", (long)getpid());
	strcpy(address.sun_path, name);
	if (abstract) {
		address.sun_path[0] = '\0';
		length = offsetof(struct sockaddr_un, sun_path) + strlen(name);
	} else {
		length = offsetof(struct sockaddr_un, sun_path) + strlen(name) + 1;
		unlink(name);
	}

	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	assert(fd >= 0);
	assert(bind(fd, (struct sockaddr *)&address, length) == 0);
	assert(setenv("NOTIFY_SOCKET", name, 1) == 0);
	assert(qs_notify("READY=1") == 0);
	assert(recv(fd, message, sizeof(message), 0) == 7);
	assert(memcmp(message, "READY=1", 7) == 0);
	close(fd);
	if (!abstract)
		unlink(name);
}

int main(void)
{
	unsetenv("NOTIFY_SOCKET");
	assert(qs_notify("READY=1") == 0);
	check_socket(0);
	check_socket(1);
	puts("notify tests: OK");
	return 0;
}
