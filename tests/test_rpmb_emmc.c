// SPDX-License-Identifier: BSD-2-Clause
#include "rpmb.h"

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/mmc/ioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

enum fault { NONE, OPEN, STAT, NOT_CHAR, SYSFS, SUBSYSTEM, WRONG_SUBSYSTEM,
	    TRUNCATED_SUBSYSTEM, ATTRIBUTE, MISSING_ENHANCED, LOCK };

static enum fault fault;
static bool opened[3];
static unsigned int locks, calls, fail_call;
static int io_error;
static uint16_t write_result, write_operation = 0x300;
static const char *attributes[] = { "0x40\n", "0x1\n", "0x1\n" };
static const char *current_attribute;
static const struct qs_rpmb_frame *expected_input;
static struct qs_rpmb_frame *expected_output;
static uint32_t expected_count, expected_batch;

int __wrap_open(const char *path, int flags, ...)
{
	int index;

	if (!strcmp(path, "/dev/test-rpmb")) {
		assert(flags == (O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (fault == OPEN) { errno = EACCES; return -1; }
		index = 0;
	} else {
		assert(!strcmp(path, "/sys/dev/char/240:9"));
		assert(flags == (O_RDONLY | O_DIRECTORY | O_CLOEXEC));
		if (fault == SYSFS) { errno = ENOENT; return -1; }
		index = 1;
	}
	assert(!opened[index]);
	opened[index] = true;
	return 40 + index;
}

int __wrap_openat(int dir, const char *path, int flags, ...)
{
	assert(dir == 41 && opened[1]);
	assert(flags == (O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
	if (fault == ATTRIBUTE) { errno = EACCES; return -1; }
	if (!strcmp(path, "../raw_rpmb_size_mult"))
		current_attribute = attributes[0];
	else if (!strcmp(path, "../rel_sectors"))
		current_attribute = attributes[1];
	else {
		assert(!strcmp(path, "../enhanced_rpmb_supported"));
		if (fault == MISSING_ENHANCED) { errno = ENOENT; return -1; }
		current_attribute = attributes[2];
	}
	assert(!opened[2]);
	opened[2] = true;
	return 42;
}

int __wrap_fstat(int fd, struct stat *st)
{
	assert(fd == 40 && opened[0]);
	if (fault == STAT) { errno = EIO; return -1; }
	memset(st, 0, sizeof(*st));
	st->st_mode = (fault == NOT_CHAR ? S_IFREG : S_IFCHR) | 0600;
	st->st_rdev = makedev(240, 9);
	return 0;
}

ssize_t __wrap_readlinkat(int dir, const char *path, char *buffer, size_t size)
{
	const char *subsystem = "../../../../bus/mmc_rpmb";

	assert(dir == 41 && opened[1] && !strcmp(path, "subsystem"));
	if (fault == SUBSYSTEM) { errno = ENOENT; return -1; }
	if (fault == WRONG_SUBSYSTEM)
		subsystem = "../../../../class/mem";
	if (fault == TRUNCATED_SUBSYSTEM) {
		memset(buffer, 'x', size);
		return size;
	}
	assert(size >= strlen(subsystem));
	memcpy(buffer, subsystem, strlen(subsystem));
	return strlen(subsystem);
}

ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
	size_t length = strlen(current_attribute);

	assert(fd == 42 && opened[2]);
	if (length > size)
		length = size;
	memcpy(buffer, current_attribute, length);
	return length;
}

int __wrap_close(int fd)
{
	assert(fd >= 40 && fd <= 42 && opened[fd - 40]);
	opened[fd - 40] = false;
	return 0;
}

int __wrap_flock(int fd, int operation)
{
	assert(fd == 40 && opened[0] && operation == (LOCK_EX | LOCK_NB));
	if (fault == LOCK) { errno = EWOULDBLOCK; return -1; }
	locks++;
	return 0;
}

static void check_command(const struct mmc_ioc_cmd *command, int write,
			  uint32_t count, const void *data)
{
	assert(command->write_flag == write);
	assert(command->opcode == (write ? 25 : 18));
	assert(command->flags == 0x35 && command->arg == 0);
	assert(command->blksz == 512 && command->blocks == count);
	assert(command->data_ptr == (uintptr_t)data);
}

int __wrap_ioctl(int fd, unsigned long operation, ...)
{
	struct mmc_ioc_multi_cmd *commands;
	va_list ap;

	assert(fd == 40 && opened[0] && operation == MMC_IOC_MULTI_CMD);
	va_start(ap, operation);
	commands = va_arg(ap, struct mmc_ioc_multi_cmd *);
	va_end(ap);
	if (commands->num_of_cmds == 2) {
		check_command(&commands->cmds[0], 1, 1, expected_input);
		check_command(&commands->cmds[1], 0, expected_count, expected_output);
		memset(expected_output, 0x87, expected_count * sizeof(*expected_output));
	} else {
		const struct qs_rpmb_frame *result_request;
		const struct qs_rpmb_frame expected_result = { .operation = htobe16(5) };

		assert(commands->num_of_cmds == 3);
		check_command(&commands->cmds[0], (int)(1u | (1u << 31)),
			      expected_batch, expected_input + calls * expected_batch);
		result_request = (void *)(uintptr_t)commands->cmds[1].data_ptr;
		check_command(&commands->cmds[1], 1, 1, result_request);
		assert(!memcmp(result_request, &expected_result, sizeof(expected_result)));
		check_command(&commands->cmds[2], 0, 1, expected_output);
		memset(expected_output, 0, sizeof(*expected_output));
		expected_output->operation = htobe16(write_operation);
		expected_output->result = htobe16(write_result);
	}
	calls++;
	if (calls == fail_call) { errno = io_error; return -1; }
	return 0;
}

static void check_closed(void)
{
	assert(!opened[0] && !opened[1] && !opened[2]);
}

static void test_open(void)
{
	struct qs_rpmb_device device = { .fd = -1 };
	const char *invalid[] = { "", "-1", "+1", "1garbage", "4294967296",
		"99999999999999999999999999999999999999", "0x100", "0" };

	assert(!qs_rpmb_emmc_open(&device, "/dev/test-rpmb"));
	assert(device.fd == 40 && device.sectors == 16384 && device.reliable_frames == 32);
	assert(locks == 1 && opened[0] && !opened[1] && !opened[2]);
	assert(qs_rpmb_emmc_open(&device, "/dev/test-rpmb") == -EBUSY);
	qs_rpmb_emmc_close(&device);
	qs_rpmb_emmc_close(&device);
	assert(device.fd == -1 && !device.sectors && !device.reliable_frames);
	check_closed();
	for (fault = OPEN; fault <= LOCK; fault++) {
		if (fault == MISSING_ENHANCED) {
			assert(!qs_rpmb_emmc_open(&device, "/dev/test-rpmb"));
			assert(device.reliable_frames == 1);
			qs_rpmb_emmc_close(&device);
		} else {
			assert(qs_rpmb_emmc_open(&device, "/dev/test-rpmb") < 0);
			assert(device.fd == -1 && !device.sectors && !device.reliable_frames);
		}
		check_closed();
	}
	fault = NONE;
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		attributes[0] = invalid[i];
		assert(qs_rpmb_emmc_open(&device, "/dev/test-rpmb") == -EINVAL);
		check_closed();
	}
	attributes[0] = "0x40\n";
	attributes[1] = "0x100\n";
	assert(qs_rpmb_emmc_open(&device, "/dev/test-rpmb") == -EINVAL);
	check_closed();
	attributes[1] = "0x1\n";
	attributes[2] = "2\n";
	assert(qs_rpmb_emmc_open(&device, "/dev/test-rpmb") == -EINVAL);
	check_closed();
	attributes[2] = "0\n";
	assert(!qs_rpmb_emmc_open(&device, "/dev/test-rpmb"));
	assert(device.reliable_frames == 1);
	qs_rpmb_emmc_close(&device);
	attributes[2] = "1\n";
}

static void test_io(void)
{
	struct qs_rpmb_device device = { .fd = -1 };
	struct qs_rpmb_frame request[6] = { 0 }, response[3];

	assert(!qs_rpmb_emmc_open(&device, "/dev/test-rpmb"));
	expected_input = request;
	expected_output = response;
	expected_count = 3;
	assert(!qs_rpmb_emmc_read(&device, request, 3, response));
	assert(calls == 1 && response[2].nonce[0] == 0x87);
	calls = 0;
	fail_call = 1;
	io_error = EIO;
	assert(qs_rpmb_emmc_read(&device, request, 3, response) == -EIO);
	assert(calls == 1);
	fail_call = 0;
	calls = 0;
	expected_batch = 2;
	assert(!qs_rpmb_emmc_write(&device, request, 6, 2, response));
	assert(calls == 3);
	for (fail_call = 1; fail_call <= 3; fail_call++) {
		calls = 0;
		io_error = EINTR;
		assert(qs_rpmb_emmc_write(&device, request, 6, 2, response) == -EINTR);
		assert(calls == fail_call);
	}
	fail_call = 0;
	for (unsigned int result = 1; result <= 0x100; result <<= 1) {
		calls = 0;
		write_result = result;
		assert(qs_rpmb_emmc_write(&device, request, 6, 2, response) == -EIO);
		assert(calls == 1);
	}
	write_result = 0;
	write_operation = 0x100;
	calls = 0;
	assert(qs_rpmb_emmc_write(&device, request, 6, 2, response) == -EIO);
	assert(calls == 1);
	qs_rpmb_emmc_close(&device);
	check_closed();
}

int main(void)
{
	test_open();
	test_io();
	puts("RPMB eMMC tests: OK");
	return 0;
}
