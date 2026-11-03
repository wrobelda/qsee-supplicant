// SPDX-License-Identifier: BSD-2-Clause
#include "rpmb.h"

#include <ctype.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/mmc/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define MMC_READ_MULTIPLE_BLOCK 18u
#define MMC_WRITE_MULTIPLE_BLOCK 25u
#define MMC_RSP_R1_ADTC 0x35u
#define MMC_RELIABLE_WRITE (1u << 31)

static int read_attribute(int dir, const char *name)
{
	char buffer[32], *end;
	unsigned long number;
	ssize_t size;
	int fd, ret;

	fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	do {
		size = read(fd, buffer, sizeof(buffer) - 1);
	} while (size < 0 && errno == EINTR);
	ret = size < 0 ? -errno : -EINVAL;
	close(fd);
	if (size <= 0 || (size_t)size == sizeof(buffer) - 1)
		return ret;
	buffer[size] = '\0';
	if (!isdigit((unsigned char)buffer[0]))
		return -EINVAL;
	errno = 0;
	number = strtoul(buffer, &end, 0);
	if (errno || end == buffer || number > UINT8_MAX)
		return -EINVAL;
	while (isspace((unsigned char)*end))
		end++;
	if (*end)
		return -EINVAL;
	return number;
}

int qs_rpmb_emmc_open(struct qs_rpmb_device *device, const char *path)
{
	char sysfs[64], subsystem[256], *name;
	struct stat st;
	uint32_t multiplier, reliable, enhanced;
	int fd, dir = -1, ret;
	ssize_t length;

	if (device->fd >= 0)
		return -EBUSY;
	fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st)) {
		ret = -errno;
		goto out;
	}
	if (!S_ISCHR(st.st_mode)) {
		ret = -ENODEV;
		goto out;
	}
	snprintf(sysfs, sizeof(sysfs), "/sys/dev/char/%u:%u",
		 major(st.st_rdev), minor(st.st_rdev));
	dir = open(sysfs, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dir < 0) {
		ret = -errno;
		goto out;
	}
	length = readlinkat(dir, "subsystem", subsystem, sizeof(subsystem) - 1);
	if (length < 0) {
		ret = -errno;
		goto out;
	}
	subsystem[length] = '\0';
	name = strrchr(subsystem, '/');
	if ((size_t)length == sizeof(subsystem) - 1 || !name || strcmp(name, "/mmc_rpmb")) {
		ret = -ENODEV;
		goto out;
	}

	/* The RPMB character device's parent is the eMMC card. */
	ret = read_attribute(dir, "../raw_rpmb_size_mult");
	if (ret < 0)
		goto out;
	multiplier = ret;
	ret = read_attribute(dir, "../rel_sectors");
	if (ret < 0)
		goto out;
	reliable = ret;
	ret = read_attribute(dir, "../enhanced_rpmb_supported");
	if (ret == -ENOENT)
		ret = 0;
	else if (ret < 0)
		goto out;
	enhanced = ret;
	if (enhanced)
		reliable = 32;
	if (enhanced > 1 || !multiplier || !reliable) {
		ret = -EINVAL;
		goto out;
	}
	if (flock(fd, LOCK_EX | LOCK_NB)) {
		ret = -errno;
		goto out;
	}
	device->fd = fd;
	device->sectors = multiplier * 256;
	device->reliable_frames = reliable;
	close(dir);
	return 0;
out:
	if (dir >= 0)
		close(dir);
	close(fd);
	return ret;
}

void qs_rpmb_emmc_close(struct qs_rpmb_device *device)
{
	if (device->fd >= 0)
		close(device->fd);
	*device = (struct qs_rpmb_device) { .fd = -1 };
}

static void setup_command(struct mmc_ioc_cmd *command, int write,
			  uint32_t count, const void *data)
{
	command->write_flag = write;
	command->opcode = write ? MMC_WRITE_MULTIPLE_BLOCK : MMC_READ_MULTIPLE_BLOCK;
	command->flags = MMC_RSP_R1_ADTC;
	command->blksz = QS_RPMB_FRAME_SIZE;
	command->blocks = count;
	mmc_ioc_cmd_set_data((*command), data);
}

int qs_rpmb_emmc_read(struct qs_rpmb_device *device,
		      const struct qs_rpmb_frame *request, uint32_t count,
		      struct qs_rpmb_frame *response)
{
	struct mmc_ioc_multi_cmd *commands;
	int ret = 0;

	commands = calloc(1, sizeof(*commands) + 2 * sizeof(commands->cmds[0]));
	if (!commands)
		return -ENOMEM;
	commands->num_of_cmds = 2;
	setup_command(&commands->cmds[0], 1, 1, request);
	setup_command(&commands->cmds[1], 0, count, response);
	if (ioctl(device->fd, MMC_IOC_MULTI_CMD, commands))
		ret = -errno;
	free(commands);
	return ret;
}

int qs_rpmb_emmc_write(struct qs_rpmb_device *device,
		       const struct qs_rpmb_frame *request, uint32_t count,
		       uint32_t batch, struct qs_rpmb_frame *response)
{
	struct qs_rpmb_frame result_request = {
		.operation = htobe16(QS_RPMB_READ_RESULT),
	};
	struct mmc_ioc_multi_cmd *commands;
	int ret = 0;

	commands = calloc(1, sizeof(*commands) + 3 * sizeof(commands->cmds[0]));
	if (!commands)
		return -ENOMEM;
	commands->num_of_cmds = 3;
	setup_command(&commands->cmds[1], 1, 1, &result_request);
	setup_command(&commands->cmds[2], 0, 1, response);
	for (uint32_t i = 0; i < count; i += batch) {
		setup_command(&commands->cmds[0], 1 | MMC_RELIABLE_WRITE, batch, request + i);
		memset(response, 0, sizeof(*response));
		if (ioctl(device->fd, MMC_IOC_MULTI_CMD, commands)) {
			/* A failed write may have reached the device. Do not retry it. */
			ret = -errno;
			break;
		}
		if (be16toh(response->operation) != QS_RPMB_AUTH_WRITE << 8 ||
		    be16toh(response->result)) {
			ret = -EIO;
			break;
		}
	}
	free(commands);
	return ret;
}
