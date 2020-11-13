/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

#include "trt/uapi/trt.hh"

static int volatile to_trigger = 0;
static bool volatile done = false;

void *notifier1(void *arg) {
	while (to_trigger != 1) {
		trt_msg("yielding\n");
		trt::T::yield();
	}
	trt::LocalAsyncObj *ao = static_cast<trt::LocalAsyncObj *>(arg);
	trt_msg("notify\n");
	trt::T::notify(ao, 0x01);
	return (void *)0xa;
}

void *notifier2(void *arg) {
	while (to_trigger != 2) {
		trt_msg("yielding\n");
		trt::T::yield();
	}
	trt::LocalAsyncObj *ao = static_cast<trt::LocalAsyncObj *>(arg);
	trt_msg("notify\n");
	trt::T::notify(ao, 0x02);
	return (void *)0xb;
}


void *waiter(void *arg) {
	trt::LocalWaitset ws;
	trt::LocalAsyncObj obj1, obj2;
	trt::LocalFuture f1(&obj1, &ws, (void *)0x01);
	trt::LocalFuture f2(&obj2, &ws, (void *)0x02);
	trt::T::spawn(notifier1, static_cast<void *>(&obj1));
	trt::T::spawn(notifier2, static_cast<void *>(&obj2));

	trt_msg("to_trigger = 1\n");
	to_trigger = 1;
	trt::RetT ret;
	void *ctx;
	trt_msg("waiting (1)\n");
	std::tie(ret, ctx) = trt::T::wait(&ws);
	trt_msg("woke up (1)\n");

	if (ret != 0x1) {
		fprintf(stderr, " ret=%zd =/= %d (expected)", ret, 0x1);
		abort();
	}
	if (ctx != (void *)0x1) {
		fprintf(stderr, " ctx=%p =/= %p (expected)", ctx, (void *)0x1);
		abort();
	}

	trt_msg("to_trigger = 2\n");
	to_trigger = 2;
	trt_msg("waiting (2)\n");
	std::tie(ret, ctx) = trt::T::wait(&ws);
	assert(ret == 0x2 && ctx == (void *)0x2);
	trt_msg("woke up (2)\n");

	trt::T::task_wait();
	trt::T::task_wait();

	trt_msg("DONE\n");
	done = true;

	return nullptr;
}

void *main_task(void *arg) {
	trt::T::spawn(waiter, nullptr);

	// ensure that the scheduelr does not exit by keeping this task always in
	// its queues
	while (!done)
		trt::T::yield();

	trt::T::task_wait();
	return nullptr;
}

int main(int argc, char *argv[])
{
	trt::Controller c;
	c.spawn_scheduler(main_task, NULL, trt::TaskType::TASK);
	return 0;
}
