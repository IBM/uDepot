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

#ifndef TRT_TASK_BASE_HH
#define TRT_TASK_BASE_HH

#include <cstdint>
#include <boost/intrusive/list.hpp>
namespace bi = boost::intrusive;

extern "C" {
    #include "jctx/jctx.h"
}

namespace trt {

class Scheduler;
class AsyncObjBase;
class TaskQueueThreadUnsafe;
class T;

// Task types
// POLL:   Tasks that poll I/O devices (e.g., network or storage). They are
//         responsible for creating new tasks.
// TASK:  short-running tasks, typically created to serve requests
//
// The distinction between these types is not fundamental. It is made in the
// hope that it will be usefull for tuning the scheduler (e.g., throughput vs
// latency). I believe we should rethink/generalize this.
enum class TaskType {POLL, TASK, NR_};

// number of task types
constexpr size_t nTaskTypes() { return static_cast<size_t>(TaskType::NR_); }

class TaskBase {
    friend Scheduler;
    friend T;
    friend TaskQueueThreadUnsafe;

public:
    enum class State {
        INITIALIZED = 1,
        READY,
        WAITING,
        FINISHED,
    };

protected:
    bi::list_member_hook<> t_lhook_; // list hook for scheduler
    TaskType t_type_;
    Scheduler *t_last_scheduler;     // last scheduler that scheduled the task
    jctx_t t_jctx_;
    TaskBase *t_parent_;             // parent task (or NULL)
    bool t_detached_;                // is task detached? (i.e., noone will wait for it)
    #if !defined(NDEBUG)
    State t_state_;
    uint64_t t_dbg_id_;
    #endif

public:
    using List = bi::list<TaskBase,
                          bi::member_hook<TaskBase,
                                          bi::list_member_hook<>,
                                          &TaskBase::t_lhook_>>;

    void inline set_state(State s) {
        #if !defined(NDEBUG)
        t_state_ = s;
        #endif
    }

protected:
    TaskBase(TaskBase *parent, bool t_detached, TaskType type);
public:
    TaskBase() = delete;
    TaskBase(TaskBase const &) = delete;
    void operator=(TaskBase const &) = delete;

    virtual ~TaskBase();
    // return pointer to the underlying async object for the return value of
    // this task
    virtual AsyncObjBase *get_ret_ao() = 0;
    // To make some operations easier, we assume that each task holds its own
    // (default) waitset.
    // (not yet used)
    // virtual WaitsetBase  *get_default_waitset();
};

} // end trt namespace

#endif /* ifndef TRT_TASK_BASE_HH */
