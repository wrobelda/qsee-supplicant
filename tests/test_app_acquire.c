// SPDX-License-Identifier: BSD-2-Clause
#include "app_acquire.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct test_context {
	const int *attach_results;
	size_t attach_count;
	size_t attach_index;
	int load_result;
	unsigned int load_calls;
};

static int attach(void *opaque, uint32_t *session)
{
	struct test_context *context = opaque;
	int result;

	assert(context->attach_index < context->attach_count);
	result = context->attach_results[context->attach_index++];
	if (!result)
		*session = 41;
	return result;
}

static int load(void *opaque, uint32_t *session)
{
	struct test_context *context = opaque;

	context->load_calls++;
	if (!context->load_result)
		*session = 42;
	return context->load_result;
}

static void test_resident_application(void)
{
	const int attach_results[] = { 0 };
	struct test_context context = {
		.attach_results = attach_results,
		.attach_count = 1,
	};
	const struct qs_app_acquire_ops ops = { attach, load };
	enum qs_app_acquisition acquisition;
	uint32_t session = 0;

	assert(!qs_app_acquire(&ops, &context, &acquisition, &session));
	assert(acquisition == QS_APP_ATTACHED);
	assert(session == 41);
	assert(context.load_calls == 0);
}

static void test_absent_application(void)
{
	const int attach_results[] = { -ENOENT };
	struct test_context context = {
		.attach_results = attach_results,
		.attach_count = 1,
	};
	const struct qs_app_acquire_ops ops = { attach, load };
	enum qs_app_acquisition acquisition;
	uint32_t session = 0;

	assert(!qs_app_acquire(&ops, &context, &acquisition, &session));
	assert(acquisition == QS_APP_LOADED);
	assert(session == 42);
	assert(context.load_calls == 1);
}

static void test_load_race(void)
{
	const int attach_results[] = { -ENOENT, 0 };
	struct test_context context = {
		.attach_results = attach_results,
		.attach_count = 2,
		.load_result = -EINVAL,
	};
	const struct qs_app_acquire_ops ops = { attach, load };
	enum qs_app_acquisition acquisition;
	uint32_t session = 0;

	assert(!qs_app_acquire(&ops, &context, &acquisition, &session));
	assert(acquisition == QS_APP_ATTACHED);
	assert(session == 41);
	assert(context.attach_index == 2);
	assert(context.load_calls == 1);
}

static void test_errors(void)
{
	const int unavailable[] = { -EIO };
	const int absent[] = { -ENOENT, -ENOENT };
	const int race_lost[] = { -ENOENT, -ENOENT };
	const struct qs_app_acquire_ops ops = { attach, load };
	enum qs_app_acquisition acquisition;
	uint32_t session;
	struct test_context context = {
		.attach_results = unavailable,
		.attach_count = 1,
	};

	assert(qs_app_acquire(&ops, &context, &acquisition, &session) == -EIO);
	assert(context.load_calls == 0);

	context = (struct test_context) {
		.attach_results = absent,
		.attach_count = 2,
		.load_result = -EACCES,
	};
	assert(qs_app_acquire(&ops, &context, &acquisition, &session) == -EACCES);
	assert(context.load_calls == 1);
	assert(context.attach_index == 2);

	context = (struct test_context) {
		.attach_results = race_lost,
		.attach_count = 2,
		.load_result = -EINVAL,
	};
	assert(qs_app_acquire(&ops, &context, &acquisition, &session) == -EINVAL);
	assert(context.attach_index == 2);
}

int main(void)
{
	test_resident_application();
	test_absent_application();
	test_load_race();
	test_errors();
	puts("application loader tests: OK");
	return 0;
}
