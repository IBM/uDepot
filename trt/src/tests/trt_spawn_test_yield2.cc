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

using namespace trt;

// NB: This used to fail because we did not remove futures that we had
// registered and we ended up registering them a second time

#define NUM (0x100)

void *t2(void *arg) {

	trt_dmsg("yielding\n");
	T::yield();
	trt_dmsg("returning\n");
	return nullptr;
}

void *t1(void *arg)
{
	trt_dmsg("spawning t2\n");
	T::spawn(t2, arg, nullptr);
	trt_dmsg("spawning t2 (again)\n");
	T::spawn(t2, arg, nullptr);

	trt_dmsg("waiting #1\n");
	T::task_wait();
	trt_dmsg("waiting #2\n");
	T::task_wait();

	trt_dmsg("returning\n");
	return nullptr;
}

int main(int argc, char *argv[])
{
	Controller c;
	c.spawn_scheduler(t1, (void *)NUM, TaskType::TASK);

	return 0;
}
