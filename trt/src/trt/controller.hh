/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4:

#ifndef TRT_CONTROLLER_HH_
#define TRT_CONTROLLER_HH_

#include <deque>

#include <functional>

#include "trt/scheduler.hh"
#include "trt/task.hh"

// Controllers implement control-path operations, such as spawning schedulers.

namespace trt {

// This is a base controller that other implementations can inherit.
//
// Initially, there was only one implementation (using ptrheads), but it made
// sense to make another one for dpdk's RTE to use with spdk. The separation is
// somewhat ad-hoc and we might want to improve it at some point.
class ControllerBase {
   public:
    ControllerBase() : ctl_done_(false) {}
    virtual ~ControllerBase();
    ControllerBase(const ControllerBase &) = delete;
    void operator=(const ControllerBase &) = delete;

    // Notify (all) schedulers that they can exit. Schedulers will exit if this
    // method is called *and* there are no tasks in their queue. Note that this
    // means if there are long-standing tasks (e.g., pollers) or tasks are kept
    // being created, the schedulers will not exit.
    //
    // This function is called in the Controller's destructor.
    void set_exit_all();

   protected:
    std::deque<Scheduler> schedulers_;
    bool ctl_done_;

   public:
     void on_each_scheduler(std::function<void(Scheduler &)> fn) {
        for (auto s = schedulers_.begin(); s < schedulers_.end(); s++) {
            fn(*s);
        }
     }

};

// pthread controller
class Controller : public ControllerBase {
public:
    void spawn_scheduler(TaskFn main_fn, TaskFnArg main_arg,
                         TaskType main_type, int affinity_core = -1);
    void spawn_scheduler(TaskFn main_fn, TaskFnArg main_arg,
                         TaskType main_type, cpu_set_t cpuset);

    void wait_for_all();
    virtual ~Controller();

};


}

#endif // TRT_CONTROLLER_HH_
