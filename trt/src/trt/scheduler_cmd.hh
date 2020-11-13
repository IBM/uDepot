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

#ifndef TRT_SCHEDULER_CMD_HH_
#define TRT_SCHEDULER_CMD_HH_

// tasks commands for the scheduler

#include <tuple>

#include "trt/sync_base_types.hh"
#include "trt/task_base.hh"
#include "trt/local_single_sync.hh"

namespace trt {

// When a task is notified, we need to select a scheduler to "schedule" it.
// Initially, I followed the simplest solution I could think, which was to
// schedule it on the current scheduler that the notification happened.
//
// For cases, however, that the notification happens always on the same
// scheduler this is not ideal, since all tasks will eventually end up
// there. This came up in a test scenario for rwlock_pagefault_trt where the
// grower task run (alone) in its own scheduler so that it could sleep.
//
// An example of this is the page-fault handler, since we handle faults only
// from a single thread.
//
// For now, add an argument to the notify command to select a policy. This
// is obviously not ideal, but it documents the issue, and deals witht he
// rwlock_pagafault_trt test case.
//
// Scheduling is hard... duh!
enum class NotifyPolicy {
    LocalSched,
    LastTaskScheduler,
    /* random ?*/
};

enum class RemoteSpawnPolicy {
    RoundRobin,
    /* Random */
};

union SchedulerCmd {
    enum class Cmd {
        NOP = 1,
        YIELD,
        SPAWN,
        SPAWN_MANY,
        RETURN,
        WAIT,
        NOTIFY,
        REMOTE_SPAWN,
        // optimized versions where everything happens on the same core, and
        // there is a single waiter.
        LOCAL_SINGLE_WAIT,
        LOCAL_SINGLE_NOTIFY,
    };

    struct Nop {
        Cmd cmd;
    } nop;
    struct Yield {
        Cmd cmd;
    } yield;
    struct Spawn {
        Cmd cmd;
        TaskBase *task;
    } spawn;
    struct SpawnMany {
        Cmd cmd;
        TaskBase::List tl;
    } spawn_many;
    struct Ret {
        Cmd cmd;
        RetT val;
    } ret;
    struct Wait {
        Cmd cmd;
        WaitsetBase *ws;
    } wait;

    using NotifyArg = std::tuple<AsyncObjBase *, RetT, NotifyPolicy>;
    //static const size_t max_notify_args = 128;
    #define TRT_MAX_NOTIFY_ARGS 128
    struct Notify {
        Cmd cmd;
        NotifyArg args[TRT_MAX_NOTIFY_ARGS];
        size_t nargs;
    } notify;

    struct RemoteSpawn {
        Cmd                cmd;
        TaskFn             fn;
        TaskFnArg          fn_arg;
        void              *caller_ctx;
        bool               detached;
        TaskType           type;
        RemoteSpawnPolicy  policy;
    } remote_spawn;

    struct LocalSingleWait {
        Cmd cmd;
        LocalSingleAsyncObj *lsao;
    } local_single_wait;

    struct LocalSingleNotify {
        Cmd cmd;
        std::tuple<LocalSingleAsyncObj *, RetT> args[TRT_MAX_NOTIFY_ARGS];
        size_t nargs;
    } local_single_notify;

    // methods

    SchedulerCmd() : nop{Cmd::NOP} {}
    SchedulerCmd(SchedulerCmd const &) = delete;
    void operator=(SchedulerCmd const &) = delete;

    Cmd type() const { return nop.cmd; }
    void reset() { nop.cmd = Cmd::NOP; }
    void set_yield() { yield.cmd = Cmd::YIELD; }
    void set_ret(RetT val) {
        ret.cmd = Cmd::RETURN;
        ret.val = val;
    }
    void set_wait(WaitsetBase *ws) {
        wait.cmd = Cmd::WAIT;
        wait.ws = ws;
    }
    void set_notify(AsyncObjBase *ao, RetT val, NotifyPolicy p = NotifyPolicy::LocalSched) {
        notify.cmd = Cmd::NOTIFY;
        notify.args[0] = std::make_tuple(ao, val, p);
        notify.nargs = 1;
    }

    void init_notify() {
        notify.cmd = Cmd::NOTIFY;
        notify.nargs = 0;
    }

    // returns false if addition failed due to inadequate size
    bool notify_add(AsyncObjBase *ao, RetT val, NotifyPolicy p) {
        assert(notify.cmd == Cmd::NOTIFY); // init_notify should be called first
        size_t idx = notify.nargs;
        if (idx >= TRT_MAX_NOTIFY_ARGS)
            return false;
        notify.args[idx] = std::make_tuple(ao, val, p);
        notify.nargs = idx + 1;
        return true;
    }

    void set_spawn(TaskBase *task) {
        spawn.cmd = Cmd::SPAWN;
        spawn.task = task;
    }
    void set_spawn_many(TaskBase::List &tl) {
        spawn_many.cmd = Cmd::SPAWN_MANY;
        // since this is a union, spawn_many.tl contains invalid data leading to
        // a SEGFAULT when the destructor of the argument is called after the
        // swap. That's the only way i can think of for calling the constructor
        // on spawn_many.tl
        new (&spawn_many.tl) TaskBase::List();
        spawn_many.tl = std::move(tl);
        //spawn_many.tl.swap(tl);
    }

    void set_remote_spawn(TaskFn fn, TaskFnArg fn_arg, void *ctx, bool detached,
                          TaskType ty, RemoteSpawnPolicy p) {
        remote_spawn.cmd = Cmd::REMOTE_SPAWN;
        remote_spawn.fn = fn;
        remote_spawn.fn_arg = fn_arg;
        remote_spawn.caller_ctx = ctx;
        remote_spawn.detached = detached;
        remote_spawn.type = ty;
        remote_spawn.policy = p;
    }

    void init_local_single_notify(void) {
        local_single_notify.cmd = Cmd::LOCAL_SINGLE_NOTIFY;
        local_single_notify.nargs = 0;
    }

    void set_local_single_wait(LocalSingleAsyncObj *lsao) {
        local_single_wait.cmd = Cmd::LOCAL_SINGLE_WAIT;
        local_single_wait.lsao = lsao;
    }

    // returns false if addition failed due to inadequate size
    bool local_single_notify_add(LocalSingleAsyncObj *lsao, RetT val) {
        assert(local_single_notify.cmd == Cmd::LOCAL_SINGLE_NOTIFY);
        size_t idx = local_single_notify.nargs;
        if (idx >= TRT_MAX_NOTIFY_ARGS)
            return false;
        local_single_notify.args[idx] = std::make_tuple(lsao, val);
        local_single_notify.nargs = idx + 1;
        return true;
    }

    bool is_set() {
        return nop.cmd != Cmd::NOP;
    }

    ~SchedulerCmd() {
        switch (nop.cmd) {
            case Cmd::NOP:
                nop.~Nop();
                break;
            case Cmd::YIELD:
                yield.~Yield();
                break;
            case Cmd::SPAWN:
                spawn.~Spawn();
                break;
            case Cmd::SPAWN_MANY:
                spawn_many.~SpawnMany();
                break;
            case Cmd::RETURN:
                ret.~Ret();
                break;
            case Cmd::WAIT:
                wait.~Wait();
                break;
            case Cmd::NOTIFY:
                notify.~Notify();
                break;
            case Cmd::REMOTE_SPAWN:
                remote_spawn.~RemoteSpawn();
                break;
            case Cmd::LOCAL_SINGLE_WAIT:
                local_single_wait.~LocalSingleWait();
                break;
            case Cmd::LOCAL_SINGLE_NOTIFY:
                local_single_notify.~LocalSingleNotify();
                break;
        }
    }
};

} // end namespace trt

#endif /* TRT_SCHEDULER_CMD_HH_ */
