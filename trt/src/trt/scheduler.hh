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

#ifndef TRT_SCHEDULER_HH_
#define TRT_SCHEDULER_HH_

#include <atomic>
#include <cinttypes>
#include <random>

extern "C" {
    #include "jctx/jctx.h"
}

#include "trt/common.hh"
#include "trt/scheduler_cmd.hh"
#include "trt/task_queue.hh"
#include "trt/task_alloc.hh"
#include "trt/task.hh"

namespace trt {

// forward declerations for befriending
class ControllerBase;
class Waitset;
class LocalWaitset;
class Task;
class T;

void task_return(RetT ret);

// (global) ids for schedulers and tasks.
// Only for debugging
#if !defined(NDEBUG)
uint64_t dbg_sched_id(void);
uint64_t dbg_task_id(void);
#endif

// Shutdown:
//  exit_ is set by the controller to notify that it is done. This might happen
//  implicitly in the Controller's destructor.
//
//  app_done_ is set by the application. It is intended to be used by
//  applications to set and check their own exit conditions. The scheduler does
//  not make use of this value.
struct SchedCtl {
    std::atomic<bool> exit_;
    std::atomic<bool> app_done_;

    SchedCtl() : exit_(false), app_done_(false) {}

    void set_exit(void)     { exit_.store(true); }
    #if 0
    void set_app_done(void) { app_done_.store(true); }
    bool is_app_done(void)  { return app_done_.load(); }
    #endif
};

class Scheduler {
    friend ControllerBase;
    friend Waitset;
    friend LocalWaitset;
    friend Task;
    friend void task_return(RetT ret);
    friend T;

   public:
    using Cmd = SchedulerCmd::Cmd;

    enum class State { INIT = 1, RUNNING, DONE};
   protected:
    State s_state_;

    // unsafe queue for local scheduling:  can only be used by the same core
    TaskQueueThreadUnsafe s_tqs_[nTaskTypes()];
    // safe queue (with locking): can be used for remote queues to add tasks.
    // One use of this is implementing NotifyPolicy::LastTaskScheduler
    TaskQueueRemote      s_remote_tq_;

    SchedulerCmd s_cmd_;
    SchedCtl s_ctl_;
    // Each scheduler has its own context. This allows to perform operations in
    // a different stack than the task's, simplifying synchronization.
    jctx_t s_jctx_;
    // Current task executing
    TaskBase *s_current_;

    #if !defined(NDEBUG)
    uint64_t s_dbg_id_;
    #endif

    pthread_barrier_t s_barrier_;
    pthread_t s_self_tid_;
    cpu_set_t s_cpuset_;

    // task free list
    TaskAllocationQueue s_task_allocq_;

    ControllerBase &s_controller_;

    // random helper
    //std::random_device s_rand_dev_;
    std::default_random_engine s_rand_eng_;
    std::uniform_int_distribution<size_t> s_rand_dist_;

    // see T::io_npending_{inc,dec,get}
    size_t s_io_npending_;


   public:
    Scheduler(const Scheduler &) = delete;
    void operator=(const Scheduler &) = delete;
    Scheduler() = delete;

    Scheduler(ControllerBase &controller, cpu_set_t cpuset,
              TaskFn main_fn, TaskFnArg main_arg, TaskType main_type);

    ~Scheduler();

    void start_(void);
    void handle_cmd_(SchedulerCmd &cmd);
    State get_state(void) { return s_state_; }

    void set_state_running() {
        assert(s_state_ == State::INIT);
        s_state_ = State::RUNNING;
    }
    static void thread_init(Scheduler *s);
    void sched_setaffinity(void);
    pthread_t get_pthread_tid(void) { return s_self_tid_; }
    void pthread_barrier_wait(void) {
        int err = ::pthread_barrier_wait(&s_barrier_);
        if (err != 0 && err != PTHREAD_BARRIER_SERIAL_THREAD) {
            fprintf(stderr, "pthread_barrier returned: %d\n", err);
            abort();
        }
    }
    cpu_set_t get_cpuset() { return s_cpuset_; }


    #if !defined(NDEBUG)
    uint64_t getId(void) const { return s_dbg_id_; }
    #endif

   private:
    TaskQueueThreadUnsafe &get_tq_(TaskType t) { return s_tqs_[static_cast<size_t>(t)]; }
    void schedule_task_(TaskBase *);

    void push_task_front(TaskBase &t) { get_tq_(t.t_type_).push_front(t); }
    void push_task_back(TaskBase &t)  { get_tq_(t.t_type_).push_back(t);  }

   public:
    // remote pushes might fail (e.g., if the scheduler has stopped).
    // Returns true if successful, false otherwise.
    //
    // NB: There is an implication here wrt task allocation. While in our
    // current allocation scheme it's OK if we allocate/free a task from
    // different schedulers, for other implementations this might be
    // problematic.
    __attribute__ ((warn_unused_result))
    bool remote_push_task_front(TaskBase &t) {
        // assume that only normal tasks can be pushed, because it simplifies
        // the exit condition check.
        assert(t.t_type_ == TaskType::TASK);
        return s_remote_tq_.push_back_if_running(t);
    }


   protected:
    // Allocate a task
    // Use a double pointer (instead of returning a pointer) to make
    // instantiation easier.
    // Callers need to check *retp for null.
    template<typename T, typename... Args>
    void task_alloc(T **retp, Args &&...args) {
        *retp = s_task_allocq_.alloc<T, Args...>(std::forward<Args>(args)...);
    }

    void task_free(TaskBase *t) {
        s_task_allocq_.free(t);
    }

   private:
     /* private functions also executed in task context */
     void switch_to_sched_(void);
     void notify_(AsyncObjBase *aio, RetT val, NotifyPolicy p);
   public:
    size_t rand(void) { return s_rand_dist_(s_rand_eng_); }

public:
   // These are various application-specific fields.  It's ugly, but, for now, I
   // prefer it compared to other possible options.
    #if !defined(NDEBUG)
    size_t     rwpf_rb_count; // rollback count
    #endif
};

} // end namespace trt

#endif // TRT_SCHEDULER_HH_
