// SPDX-License-Identifier: BSD-2-Clause
#include "rpmb.h"

#include <endian.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static struct qs_rpmb_device device = { .fd = -1 };

int qs_rpmb_open(const char *path)
{
	if (device.fd >= 0)
		return -EBUSY;
	return qs_rpmb_emmc_open(&device, path);
}

void qs_rpmb_close(void)
{
	qs_rpmb_emmc_close(&device);
}

static int init(void *buffer)
{
	struct qs_rpmb_init_request request;
	struct qs_rpmb_init_response response = { 0 };
	uint32_t version;

	memcpy(&request, buffer, sizeof(request));
	version = le32toh(request.version);
	response.command = request.command;
	response.version = htole32(version >= 2 ? 2 : 1);
	response.status = htole32(UINT32_MAX);
	if (version && device.fd >= 0) {
		response.status = 0;
		response.sectors = htole32(device.sectors);
		response.reliable_frames = htole32(device.reliable_frames);
		if (version >= 2)
			response.device_type = htole32(QS_RPMB_EMMC);
	}
	memset(buffer, 0, QS_RPMB_BUFFER_SIZE);
	memcpy(buffer, &response, sizeof(response));
	return 0;
}

static int rw(void *buffer)
{
	struct qs_rpmb_rw_request request;
	struct qs_rpmb_rw_response response = { 0 };
	struct qs_rpmb_frame *frames = NULL, *output;
	uint32_t command, count, length, offset, batch;
	int ret = -EINVAL;

	memcpy(&request, buffer, sizeof(request));
	command = le32toh(request.command);
	count = le32toh(request.count);
	length = le32toh(request.length);
	offset = le32toh(request.offset);
	batch = le32toh(request.reliable_frames);
	response.command = request.command;
	response.version = request.version;
	response.offset = htole32(sizeof(response));

	if (device.fd < 0 || !count ||
	    count > (QS_RPMB_BUFFER_SIZE - sizeof(response)) / QS_RPMB_FRAME_SIZE ||
	    offset < sizeof(request) || offset > QS_RPMB_BUFFER_SIZE ||
	    length > QS_RPMB_BUFFER_SIZE - offset ||
	    length != (command == QS_RPMB_WRITE ? count : 1) * QS_RPMB_FRAME_SIZE)
		goto out;
	if (command == QS_RPMB_WRITE &&
	    (!batch || batch > device.reliable_frames || count % batch))
		goto out;

	/* The response overlaps the request in the listener's shared buffer. */
	frames = malloc(length);
	if (!frames)
		goto out;
	memcpy(frames, (uint8_t *)buffer + offset, length);
	if (command == QS_RPMB_WRITE) {
		for (uint32_t i = 0; i < count; i++)
			if (be16toh(frames[i].operation) != QS_RPMB_AUTH_WRITE)
				goto out;
	} else if (be16toh(frames->operation) != QS_RPMB_AUTH_READ &&
		   !(be16toh(frames->operation) == QS_RPMB_READ_COUNTER && count == 1)) {
		goto out;
	}

	memset(buffer, 0, QS_RPMB_BUFFER_SIZE);
	output = (void *)((uint8_t *)buffer + sizeof(response));
	if (command == QS_RPMB_WRITE)
		ret = qs_rpmb_emmc_write(&device, frames, count, batch, output);
	else
		ret = qs_rpmb_emmc_read(&device, frames, count, output);
out:
	if (frames) {
		explicit_bzero(frames, length);
		free(frames);
	}
	if (ret) {
		memset(buffer, 0, QS_RPMB_BUFFER_SIZE);
		response.status = htole32(UINT32_MAX);
	} else {
		response.length = htole32((command == QS_RPMB_WRITE ? 1 : count) *
					 QS_RPMB_FRAME_SIZE);
	}
	memcpy(buffer, &response, sizeof(response));
	return 0;
}

static int dispatch(struct qs_store *store, void *buffer, size_t size)
{
	struct qs_rpmb_partition_response partition = {
		.command = htole32(QS_RPMB_PARTITION),
		.status = htole32(UINT32_MAX),
		.offset = htole32(sizeof(partition)),
	};
	uint32_t command;

	(void)store;
	if (!buffer || size < QS_RPMB_BUFFER_SIZE)
		return -EINVAL;
	memcpy(&command, buffer, sizeof(command));
	switch (le32toh(command)) {
	case QS_RPMB_INIT:
		return init(buffer);
	case QS_RPMB_READ:
	case QS_RPMB_WRITE:
		return rw(buffer);
	case QS_RPMB_PARTITION:
		/* Partition-table provisioning is not part of this listener. */
		memset(buffer, 0, QS_RPMB_BUFFER_SIZE);
		memcpy(buffer, &partition, sizeof(partition));
		return 0;
	default:
		memset(buffer, 0, QS_RPMB_BUFFER_SIZE);
		return -ENOTSUP;
	}
}

const struct qs_service qs_rpmb_service = {
	QS_RPMB_SERVICE_ID, QS_RPMB_BUFFER_SIZE, dispatch, NULL,
};
