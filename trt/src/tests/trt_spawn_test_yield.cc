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

#define NUM (0x100)

void *t2(void *arg) {
	trt_dmsg("yielding\n");
	T::yield();
	uintptr_t x = (uintptr_t)arg;
	trt_dmsg("returning\n");
	return (void *)(x+1);
}

void *t1(void *arg)
{

	trt_dmsg("spawning t2\n");
	T::spawn(t2, arg, (void *)0xf11f11);
	trt_dmsg("waiting t2\n");
	std::tuple<trt::RetT, void *> ret __attribute__((unused)) = T::task_wait();
	assert(std::get<0>(ret) == (NUM+1));
	assert(std::get<1>(ret) == (void *)0xf11f11);
	trt_dmsg("returning\n");

	return nullptr;
}

int main(int argc, char *argv[])
{
	Controller c;
	c.spawn_scheduler(t1, (void *)NUM, TaskType::TASK);

	return 0;
}
