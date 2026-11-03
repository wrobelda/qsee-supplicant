// SPDX-License-Identifier: BSD-2-Clause
#include "rpmb.h"

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned int reads, writes, opens, closes;
static int io_error;
static uint32_t expected_count, expected_batch;

int qs_rpmb_emmc_open(struct qs_rpmb_device *device, const char *path)
{
	assert(!strcmp(path, "/dev/test-rpmb"));
	assert(device->fd == -1);
	*device = (struct qs_rpmb_device) { 42, 16384, 32 };
	opens++;
	return 0;
}

void qs_rpmb_emmc_close(struct qs_rpmb_device *device)
{
	if (device->fd >= 0) {
		assert(device->fd == 42);
		closes++;
	}
	*device = (struct qs_rpmb_device) { .fd = -1 };
}

int qs_rpmb_emmc_read(struct qs_rpmb_device *device,
		      const struct qs_rpmb_frame *request, uint32_t count,
		      struct qs_rpmb_frame *response)
{
	assert(device->fd == 42 && count == expected_count);
	assert(be16toh(request->operation) == (count == 1 ? 2 : 4));
	assert(request->nonce[0] == 0x57);
	reads++;
	memset(response, 0x6b, count * sizeof(*response));
	return io_error;
}

int qs_rpmb_emmc_write(struct qs_rpmb_device *device,
		       const struct qs_rpmb_frame *request, uint32_t count,
		       uint32_t batch, struct qs_rpmb_frame *response)
{
	assert(device->fd == 42 && count == expected_count && batch == expected_batch);
	for (uint32_t i = 0; i < count; i++) {
		assert(be16toh(request[i].operation) == 3);
		assert(request[i].mac[0] == 0xa7);
		assert(request[i].data[0] == i);
	}
	writes++;
	memset(response, 0x7c, sizeof(*response));
	return io_error;
}

static void put32(uint8_t *buffer, uint32_t value)
{
	value = htole32(value);
	memcpy(buffer, &value, sizeof(value));
}

static uint32_t get32(const uint8_t *buffer)
{
	uint32_t value;

	memcpy(&value, buffer, sizeof(value));
	return le32toh(value);
}

static void check_bytes(const uint8_t *buffer, size_t length, uint8_t value)
{
	for (size_t i = 0; i < length; i++)
		assert(buffer[i] == value);
}

static void request_rw(uint8_t *buffer, bool write, uint32_t count, uint32_t batch)
{
	struct qs_rpmb_frame frame = { 0 };

	memset(buffer, 0, QS_RPMB_BUFFER_SIZE);
	put32(buffer, write ? 0x103 : 0x102);
	put32(buffer + 4, count);
	put32(buffer + 8, (write ? count : 1) * 512);
	put32(buffer + 12, 24);
	put32(buffer + 16, 2);
	put32(buffer + 20, batch);
	frame.operation = htobe16(write ? 3 : (count == 1 ? 2 : 4));
	frame.mac[0] = 0xa7;
	frame.nonce[0] = 0x57;
	for (uint32_t i = 0; i < (write ? count : 1); i++) {
		frame.data[0] = i;
		memcpy(buffer + 24 + i * 512, &frame, sizeof(frame));
	}
	expected_count = count;
	expected_batch = batch;
}

static void test_init(void)
{
	uint8_t buffer[QS_RPMB_BUFFER_SIZE];

	for (uint32_t version = 0; version < 4; version++) {
		memset(buffer, 0xa5, sizeof(buffer));
		put32(buffer, 0x101);
		put32(buffer + 4, version);
		assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
		assert(get32(buffer) == 0x101);
		assert(get32(buffer + 4) == (version >= 2 ? 2 : 1));
		assert(get32(buffer + 8) == (version ? 0 : UINT32_MAX));
		assert(get32(buffer + 12) == (version ? 16384 : 0));
		assert(get32(buffer + 16) == (version ? 32 : 0));
		assert(get32(buffer + 20) == (version >= 2 ? 3 : 0));
		check_bytes(buffer + 24, sizeof(buffer) - 24, 0);
	}
}

static void test_io(void)
{
	uint8_t buffer[QS_RPMB_BUFFER_SIZE];
	const uint32_t counts[] = { 1, 24, 39 };

	for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		request_rw(buffer, false, counts[i], 0);
		/* The last read request exercises an unaligned input offset. */
		if (i == 2) {
			memmove(buffer + 25, buffer + 24, 512);
			put32(buffer + 12, 25);
		}
		assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
		assert(get32(buffer) == 0x102 && !get32(buffer + 4));
		assert(get32(buffer + 8) == counts[i] * 512);
		assert(get32(buffer + 12) == 20 && get32(buffer + 16) == 2);
		check_bytes(buffer + 20, counts[i] * 512, 0x6b);
		check_bytes(buffer + 20 + counts[i] * 512,
			    sizeof(buffer) - 20 - counts[i] * 512, 0);
	}
	request_rw(buffer, true, 4, 2);
	put32(buffer + 16, 1);
	assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
	assert(get32(buffer) == 0x103 && !get32(buffer + 4));
	assert(get32(buffer + 8) == 512 && get32(buffer + 16) == 1);
	check_bytes(buffer + 20, 512, 0x7c);
	check_bytes(buffer + 532, sizeof(buffer) - 532, 0);

	for (unsigned int write = 0; write < 2; write++) {
		request_rw(buffer, write, 2, 1);
		io_error = -EIO;
		assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
		assert(get32(buffer + 4) == UINT32_MAX && !get32(buffer + 8));
		check_bytes(buffer + 20, sizeof(buffer) - 20, 0);
		io_error = 0;
	}
}

static void test_rw_versions(void)
{
	const uint32_t versions[] = { 0, 1, 2, 0x100, UINT32_MAX };
	uint8_t buffer[QS_RPMB_BUFFER_SIZE];

	/* Read/write versions are echoed, not negotiated like init versions. */
	for (unsigned int write = 0; write < 2; write++) {
		for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
			request_rw(buffer, write, 2, 1);
			put32(buffer + 16, versions[i]);
			assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
			assert(!get32(buffer + 4));
			assert(get32(buffer + 8) == (write ? 1 : 2) * 512);
			assert(get32(buffer + 16) == versions[i]);
			check_bytes(buffer + 20, (write ? 1 : 2) * 512, write ? 0x7c : 0x6b);
		}
	}
}

static void test_invalid_requests(void)
{
	const struct { unsigned int field; uint32_t value; } invalid[] = {
		{ 4, 0 }, { 4, UINT32_MAX }, { 4, 40 },
		{ 8, 0 }, { 8, UINT32_MAX }, { 8, 511 },
		{ 12, 0 }, { 12, 23 }, { 12, UINT32_MAX }, { 12, 20039 },
	};
	const uint32_t invalid_batches[] = { 0, 3, 33 };
	uint8_t buffer[QS_RPMB_BUFFER_SIZE];
	unsigned int old_reads = reads, old_writes = writes;

	for (unsigned int write = 0; write < 2; write++) {
		for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
			request_rw(buffer, write, 2, 1);
			put32(buffer + invalid[i].field, invalid[i].value);
			assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
			assert(get32(buffer + 4) == UINT32_MAX && !get32(buffer + 8));
			check_bytes(buffer + 20, sizeof(buffer) - 20, 0);
		}
	}
	for (size_t i = 0; i < sizeof(invalid_batches) / sizeof(invalid_batches[0]); i++) {
		request_rw(buffer, true, 2, invalid_batches[i]);
		assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
		assert(get32(buffer + 4) == UINT32_MAX);
	}
	/* Validate every write frame before the first batch reaches eMMC. */
	for (unsigned int frame = 0; frame < 2; frame++) {
		for (uint8_t operation = 0; operation <= 6; operation++) {
			if (operation == 3)
				continue;
			request_rw(buffer, true, 2, 1);
			buffer[24 + frame * 512 + 511] = operation;
			assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
			assert(get32(buffer + 4) == UINT32_MAX);
		}
	}
	for (uint8_t operation = 0; operation <= 6; operation++) {
		if (operation == 4)
			continue;
		request_rw(buffer, false, 2, 1);
		buffer[24 + 511] = operation;
		assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
		assert(get32(buffer + 4) == UINT32_MAX);
	}
	request_rw(buffer, true, 2, 1);
	buffer[24 + 510] = 3;
	buffer[24 + 511] = 0;
	assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
	assert(get32(buffer + 4) == UINT32_MAX);
	assert(reads == old_reads && writes == old_writes);
}

static void test_dispatch(void)
{
	uint8_t buffer[QS_RPMB_BUFFER_SIZE + 1];

	assert(qs_rpmb_service.id == 8192);
	assert(qs_rpmb_service.buffer_size == 20040);
	assert(qs_rpmb_service.dispatch(NULL, NULL, sizeof(buffer)) == -EINVAL);
	for (size_t size = 0; size < QS_RPMB_BUFFER_SIZE; size++) {
		assert(qs_rpmb_service.dispatch(NULL, buffer, size) == -EINVAL);
	}
	memset(buffer, 0xa5, sizeof(buffer));
	put32(buffer, 0x104);
	assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
	assert(get32(buffer + 4) == UINT32_MAX && !get32(buffer + 8));
	assert(get32(buffer + 12) == 16);
	check_bytes(buffer + 16, QS_RPMB_BUFFER_SIZE - 16, 0);
	assert(buffer[QS_RPMB_BUFFER_SIZE] == 0xa5);
	put32(buffer, UINT32_MAX);
	assert(qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)) == -ENOTSUP);
	check_bytes(buffer, QS_RPMB_BUFFER_SIZE, 0);
}

int main(void)
{
	uint8_t buffer[QS_RPMB_BUFFER_SIZE];

	request_rw(buffer, true, 1, 1);
	assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
	assert(get32(buffer + 4) == UINT32_MAX && writes == 0);
	assert(!qs_rpmb_open("/dev/test-rpmb"));
	assert(qs_rpmb_open("/dev/test-rpmb") == -EBUSY);
	test_init();
	test_io();
	test_rw_versions();
	test_invalid_requests();
	test_dispatch();
	qs_rpmb_close();
	qs_rpmb_close();
	assert(opens == 1 && closes == 1);
	put32(buffer, 0x101);
	put32(buffer + 4, 2);
	assert(!qs_rpmb_service.dispatch(NULL, buffer, sizeof(buffer)));
	assert(get32(buffer + 8) == UINT32_MAX && !get32(buffer + 12));
	puts("RPMB protocol tests: OK");
	return 0;
}
