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
	printf("%s\n", __PRETTY_FUNCTION__);
	return nullptr;
}

void *t1(void *arg)
{
	printf("%s\n", __PRETTY_FUNCTION__);
	T::spawn(t2, nullptr, nullptr, true);
	return nullptr;
}

int main(int argc, char *argv[])
{
	Controller c;
	c.spawn_scheduler(t1, (void *)NUM, TaskType::TASK);

	return 0;
}
