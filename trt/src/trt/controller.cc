/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop:4 shiftwidth:4:

#include <pthread.h>
#include <sched.h>

#include "trt/controller.hh"
#include "trt/scheduler.hh"

namespace trt {

static void *scheduler_thread(void *arg) {
    Scheduler *scheduler = static_cast<Scheduler *>(arg);
    Scheduler::thread_init(scheduler);
    scheduler->sched_setaffinity();
    scheduler->set_state_running();
    scheduler->pthread_barrier_wait();
    scheduler->start_();
    return NULL;
}


void Controller::spawn_scheduler(TaskFn main_fn, TaskFnArg main_arg,
                                 TaskType main_type, int affinity_core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (affinity_core >= 0) {
        CPU_SET(affinity_core, &cpuset);
    } else {
        int err = sched_getaffinity(0, sizeof(cpuset), &cpuset);
        if (err) {
            perror("sched_getaffinity");
            exit(1);
        }
    }

    spawn_scheduler(main_fn, main_arg, main_type, cpuset);
}

void Controller::spawn_scheduler(TaskFn main_fn, TaskFnArg main_arg,
                                 TaskType main_type, cpu_set_t cpuset) {
    pthread_t tid;
    Scheduler *s;
    int err;

    if (0) {
        // TODO: schedulers_ is intended to only grow. If a scheduler
        // exits, it's state will be set to DONE, but its resources will
        // remain available At this point, we can search the schedulers_
        // vector for a DONE scheduler to reset.
    } else {
        schedulers_.emplace_back(*this, cpuset, main_fn, main_arg, main_type);
        s = &schedulers_.back();
    }
    assert(s != NULL);

    err = pthread_create(&tid, NULL, scheduler_thread, s);
    if (err) {
        perror("pthread_create");
        exit(1);
    }

    // wait for scheduler initialization
    s->pthread_barrier_wait();
}

void ControllerBase::set_exit_all(void) {
    // notify all schedulers
    for (auto &s: schedulers_) {
        s.s_ctl_.set_exit();
    }

}

void Controller::wait_for_all(void) {
    // wait for them
    for (auto &s: schedulers_) {
        if (s.get_state() == Scheduler::State::RUNNING) {
            //printf("Waiting for scheduler: %p\n", &s);
            pthread_join(s.get_pthread_tid(), NULL);
        }
    }
    ctl_done_ = true;
}

ControllerBase::~ControllerBase() { }

Controller::~Controller() {
    if (!ctl_done_) {
        set_exit_all();
        wait_for_all();
    }
}

} // end namespace trt
