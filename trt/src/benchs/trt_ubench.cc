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
extern "C" {
	#include "trt_util/timer.h"
}

#define TRT_MTASKS 20 // million of tasks

// This ubencmark measures the throughput of spawning tasks. We spawn the tasks
// detached, which is meant to emulate a network server that spawns one new
// task per connection.

void *
t_worker(void *arg) {
	size_t *completed = static_cast<size_t *>(arg);
	*completed += 1;
	return nullptr;
}

static void *
t_spawner(void *arg__)
{
	size_t tasks_spawned          = 0;
	size_t tasks_total            = TRT_MTASKS*1000*1000;
	const size_t tasks_batch      = 64;
	const size_t tasks_runing_max = 1024;
	size_t trt_tasks_completed = 0;

	while (tasks_spawned < tasks_total) {
		assert(trt_tasks_completed <= tasks_spawned);
		size_t tasks_running = tasks_spawned - trt_tasks_completed;
		if (tasks_running >= tasks_runing_max) {
			trt::T::yield();
			continue;
		}

		#if 0
		for (size_t i=0; i<tasks_batch; i++) {
			trt::T::spawn(t_worker, nullptr, nullptr, true);
		}
		#else
		trt::Task::List tl;
		for (size_t i=0; i<tasks_batch; i++) {
			trt::Task *t = trt::T::alloc_task(t_worker, &trt_tasks_completed, nullptr, true);
			tl.push_front(*t);
		}
		trt::T::spawn_many(tl);
		#endif

		tasks_spawned += tasks_batch;
		trt::T::yield();
	}

	while (trt_tasks_completed != tasks_spawned)
		trt::T::yield();

	return nullptr;
}

// main trt task
void *t_main(void *arg__) {

	xtimer_t t; timer_init(&t); timer_start(&t);
    trt::T::spawn(t_spawner, nullptr, nullptr, false, trt::TaskType::POLL);
    trt::T::task_wait();
	timer_pause(&t);
    double s = timer_secs(&t);
    printf("time=%lfs Mtasks/sec=%lf\n", s, TRT_MTASKS/s);
	trt::T::set_exit_all(); // notify trt schedulers that we are done
	trt_dmsg("%s: DONE\n", __FUNCTION__);
	return nullptr;
}

int main(int argc, char *argv[])
{
	trt::Controller c;
	c.spawn_scheduler(t_main, nullptr, trt::TaskType::TASK);
	c.wait_for_all();

	return 0;
}
