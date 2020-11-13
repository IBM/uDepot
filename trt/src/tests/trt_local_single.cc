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

void *
waiter(void *arg) {
	trt::LocalSingleAsyncObj *ao = static_cast<trt::LocalSingleAsyncObj *>(arg);
	printf("%s: Calling wait\n", __PRETTY_FUNCTION__);
	trt::RetT ret = trt::T::local_single_wait(ao);
	printf("%s: 0x%lx\n", __PRETTY_FUNCTION__, ret);
	return (void *)ret;
}

void *
notifier(void *arg) {
	trt::LocalSingleAsyncObj *ao = static_cast<trt::LocalSingleAsyncObj *>(arg);
	printf("%s: Calling notify\n", __PRETTY_FUNCTION__);
	trt::T::local_single_notify(ao, 0xbeed);
	return nullptr;
}

void *
main_task(void *arg) {
	{
		trt::LocalSingleAsyncObj ao;
		trt::T::spawn(waiter, static_cast<void *>(&ao));
		trt::T::spawn(notifier, static_cast<void *>(&ao));
		trt::T::task_wait();
		trt::T::task_wait();
	}
	printf("--\n");
	{
		trt::LocalSingleAsyncObj ao;
		trt::T::spawn(notifier, static_cast<void *>(&ao));
		trt::T::spawn(waiter, static_cast<void *>(&ao));
		trt::T::task_wait();
		trt::T::task_wait();
	}
	return nullptr;
}

int main(int argc, char *argv[])
{
	trt::Controller c;

	c.spawn_scheduler(main_task, NULL, trt::TaskType::TASK);
	return 0;
}
