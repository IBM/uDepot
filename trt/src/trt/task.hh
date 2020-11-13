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

#ifndef TRT_TASK_HH_
#define TRT_TASK_HH_

#include <setjmp.h> // used for sigjmp_buf (rwlpf_rb_jmp)

#include "trt/task_base.hh"
#include "trt/task_mem.hh"
#include "trt/waitset.hh"
#include "trt/async_obj.hh"

namespace trt {

class Scheduler;
class AsyncObj;
union SchedulerCmd;

// NB: We use void * for return values, arguments, etc. to simplify things.
// At some point, I might try to figure out how to use types.
class Task : public TaskMem<16*1024,16*1024>, public TaskBase {
    friend Scheduler;
    friend TaskQueueThreadUnsafe;
    friend SchedulerCmd;
    friend T;

   private:
    spinlock_t t_lock_;
   protected:
    Waitset  t_ws_;
    AsyncObj t_ret_ao_;
    Future   t_parent_future_; // parent's future

   // These are rwpf (read-write-lock-with-page-faults) fields needed in Task.
   // It's ugly, but, for now, I prefer it compared to other possible options.
   public:
    // flag and context for rwlock_pagafault_trt
    int        rwpf_rb_set;
    sigjmp_buf rwpf_rb_jmp;

   public:
    Task() = delete;
    Task(Task const &) = delete;
    void operator=(Task const &) = delete;

    static void dealloc_task__(AsyncObj *ao, void *t);

    Task(TaskFn fn, TaskFnArg arg, void *caller_ctx = nullptr, Task *t_parent = nullptr,
         bool t_detached = false, TaskType type = TaskType::TASK)
     : TaskMem()
     , TaskBase(t_parent, t_detached, type)
     // if no parent exists, task is always detached
     , t_ws_(*this)
     , t_ret_ao_(dealloc_task__, this)
     , t_parent_future_(t_detached_ ? nullptr : &t_ret_ao_,
                        t_detached_ ? nullptr : &t_parent->t_ws_,
                        caller_ctx)
     , rwpf_rb_set(0) {
        spinlock_init(&t_lock_);
        jctx_init__(&t_jctx_, get_stack_bottom(), get_stack_size(), (void *)fn, arg);
        //jctx_init(&t_jctx_, 16384, (void *)fn, arg);
    }

    inline void reset(TaskFn fn, TaskFnArg arg) {
        jctx_reset(&t_jctx_, (void *)fn, arg);
    }

    virtual ~Task() {
        //fprintf(stderr, "t_lhook_ linked: %u\n", t_lhook_.is_linked());
        //jctx_destroy(&t_jctx_);
    }

    AsyncObjBase *get_ret_ao(void) override {
        return &t_ret_ao_;
    }

    #if 0
    AbstractWaitset *get_default_waitset(void) override {
        return &t_ws_;
    }
    #endif
};


} // end namespace trt

#endif // TRT_TASK_HH_
