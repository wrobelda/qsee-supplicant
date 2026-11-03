// SPDX-License-Identifier: BSD-2-Clause
#include "qsee_supplicant.h"
#include "notify.h"
#include "rpmb.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop_handler(int signal_number) { (void)signal_number; stopping = 1; }
static void ready_handler(void *data)
{
	unsigned int *delay = data;

	*delay = 1;
	if (qs_notify("READY=1\nSTATUS=Listener services registered") < 0)
		fprintf(stderr, "event=notify_error errno=%d message=\"%s\"\n",
			errno, strerror(errno));
}

int main(int argc, char **argv)
{
	const struct option options[] = {
		{ "state-dir", required_argument, NULL, 's' },
		{ "rpmb-device", required_argument, NULL, 'r' },
		{ NULL, 0, NULL, 0 },
	};
	const char *state = "/var/lib/qsee-supplicant", *rpmb = NULL;
	struct qs_store store = { .root_fd = -1 };
	struct qs_service *services;
	size_t count = qs_builtin_service_count;
	unsigned int delay = 1;
	int option, ret;

	while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
		switch (option) {
		case 's': state = optarg; break;
		case 'r': rpmb = optarg; break;
		default: goto usage;
		}
	}
	if (optind != argc)
		goto usage;
	services = calloc(count + 1, sizeof(*services));
	if (!services) {
		perror("listener services");
		return 1;
	}
	memcpy(services, qs_builtin_services, count * sizeof(*services));
	umask(0077);
	if (qs_store_open(&store, state, 0600, 0700)) {
		perror("state directory");
		free(services);
		return 1;
	}
	if (rpmb) {
		ret = qs_rpmb_open(rpmb);
		if (ret) {
			fprintf(stderr, "event=rpmb_open_error path=%s errno=%d message=\"%s\"\n",
				rpmb, -ret, strerror(-ret));
			qs_store_close(&store);
			free(services);
			return 1;
		}
		services[count++] = qs_rpmb_service;
	}
	{
		struct sigaction action = { .sa_handler = stop_handler };
		sigemptyset(&action.sa_mask);
		/* No SA_RESTART: a signal must interrupt the blocking receive ioctl. */
		sigaction(SIGINT, &action, NULL);
		sigaction(SIGTERM, &action, NULL);
	}
	while (!stopping) {
		fprintf(stderr, "event=transport_start transport=%s state=%s\n", qs_qseecom_transport.name, state);
		if (!qs_qseecom_transport.serve(&store, services, count, &stopping,
					 ready_handler, &delay)) break;
		fprintf(stderr, "event=transport_error transport=%s errno=%d message=\"%s\" retry=%u\n",
			qs_qseecom_transport.name, errno, strerror(errno), delay);
		for (unsigned int i = 0; i < delay && !stopping; i++) sleep(1);
		if (delay < 30) delay *= 2;
	}
	qs_store_close(&store);
	qs_rpmb_close();
	free(services);
	fprintf(stderr, "event=shutdown\n"); return 0;
usage:
	fprintf(stderr, "usage: %s [--state-dir PATH] [--rpmb-device PATH]\n", argv[0]);
	return 2;
}
