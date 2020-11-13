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

void *hello_task(void *arg)
{
	printf("Hello: %p\n", arg);
	return (void *)0xdeed;
}

int main(int argc, char *argv[])
{
	Controller c;
	unsigned nschedulers = argc > 1 ? atol(argv[1]) : 1;

	for (unsigned i=0; i<nschedulers; i++)
		c.spawn_scheduler(hello_task, (void *)(0xbeef + (uintptr_t)i), TaskType::TASK);

	return 0;
}
