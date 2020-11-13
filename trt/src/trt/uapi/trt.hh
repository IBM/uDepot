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

#ifndef TRT_UAPI_TRT_HH_
#define TRT_UAPI_TRT_HH_

#include "trt/common.hh"
#include "trt/async_obj.hh"
#include "trt/task.hh"
#include "trt/scheduler.hh"
#include "trt/controller.hh"
#include "trt/local_sync.hh"

namespace trt {


// TODO: move this to T
void task_return(void *ret) __attribute__((noreturn));

// task interface
//
// NB: using static functions in a class instead of a namespace to allow for
// easy befriending.
class T {
    T() = delete;
    T(T const &) = delete;
    void operator=(T const &) = delete;

public:
    // interface for multiple notifications
    static void notify_init(void);
    static bool notify_add(AsyncObjBase *ao, RetT val, NotifyPolicy p = NotifyPolicy::LocalSched);
    static void notify_submit(void);
    // simple interface for a single notification
    static void notify(AsyncObjBase *ao, RetT val, NotifyPolicy p = NotifyPolicy::LocalSched);

    static void yield(void);
    static Task *alloc_task(TaskFn fn, TaskFnArg arg,
                            void *caller_ctx = nullptr, bool detached = false,
                            TaskType type = TaskType::TASK);


    static void spawn_task(Task *t);
    static void spawn_many(Task::List &tl);

    static void spawn(TaskFn fn, TaskFnArg arg, void *caller_ctx = nullptr,
                      bool detached = false, TaskType type = TaskType::TASK);

    // NYI!
    static void remote_spawn(TaskFn fn, TaskFnArg, void *caller_ctx = nullptr,
                             bool detached = false, TaskType ty = TaskType::TASK,
                             RemoteSpawnPolicy p = RemoteSpawnPolicy::RoundRobin);

    static void free_task(Task *t);


    // wait on the specified waitset
    static FutureBase *wait_(WaitsetBase *ws);
    static std::tuple<RetT, void *> wait(WaitsetBase *ws);

    // wait on the tasks internal waitset
    static Future *task_wait_(void);
    static std::tuple<RetT, void *> task_wait(void);

    // local/single wait/notify
    static RetT local_single_wait(LocalSingleAsyncObj *);
    static void local_single_notify_init(void);
    static bool local_single_notify_add(LocalSingleAsyncObj *, RetT);
    static void local_single_notify_submit();
    static void local_single_notify(LocalSingleAsyncObj *lsao, RetT val);

    // helper rand() function
    static size_t rand(void);

    // application termination helper
    #if 0
    static void set_done(void); // TODO: we might want to have a global vs local flag here
    static bool is_done(void);
    #endif

    static Task &self(void);

    #if !defined(NDEBUG)
    static uint64_t tid(void); // current task id
    static uint64_t sid(void); // scheduler id
    #endif

    static void set_exit_all(void);

    // intentended for debugging and hacks
    static Scheduler *getS(void);
    static bool in_trt() { return getS() != nullptr; }

    // Simple mechanism to support sleeping.
    //
    // If we have multiple IO backends (e.g., one for storage and one for
    // network), it's not trivial to decide when to sleep in a poller (if the
    // backend supports it).
    //
    // We can distinguish between two types of events IO pollers receive:
    // requests and replies. Replies can be network requests to other machines
    // or IO requests to the storage. We can count the number of reply events we
    // expect, based on how many requests we have issued. If there is only one
    // backend/poller for receiving reqeusts (typically epoll), then it may
    // sleep if no pending replies exist.
    //
    // Another way to think of this is a reference count on sleeping.
    static void io_npending_inc(size_t x)   {
        getS()->s_io_npending_ += x;
    }
    static void io_npending_dec(size_t x)   {
        assert(getS()->s_io_npending_ >= x);
        getS()->s_io_npending_ -= x;
    }
    static size_t io_npending_get() { return getS()->s_io_npending_; }
};

} // end namespace trt

//#define trt_dbg_print_str__ "S%-4ld:T%-4ld>>>>> %s() [%s +%d]"
//#define trt_dbg_print_arg__ trt::T::sid(), trt::T::tid(), __FUNCTION__, __FILE__, __LINE__
#if !defined(NDEBUG)
#define trt_dbg_print_str__ "S%-4ld:T%-4ld %20s()"
#define trt_dbg_print_arg__ ::trt::T::sid(), ::trt::T::tid(), __FUNCTION__
#else
#define trt_dbg_print_str__ "%20s()"
#define trt_dbg_print_arg__	__FUNCTION__
#endif
#define trt_dbg_print(msg, fmt, args...) \
    printf(trt_dbg_print_str__ " " msg fmt , trt_dbg_print_arg__ , ##args)

#if !defined(NDEBUG)
    #define trt_dmsg(fmt,args...) trt_dbg_print("", fmt, ##args)
#else
    #define trt_dmsg(fmt,args...) do { } while (0)
#endif
#define trt_msg(fmt,args...) trt_dbg_print("", fmt, ##args)
#define trt_err(fmt,args...) \
    fprintf(stderr, trt_dbg_print_str__ " " fmt , trt_dbg_print_arg__ , ##args)

#endif
