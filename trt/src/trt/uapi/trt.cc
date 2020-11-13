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

#include <cassert>

#include "trt/uapi/trt.hh"
#include "trt/scheduler.hh"
#include "trt/controller.hh"
#include "trt/sync_base_types.hh"


namespace trt {

extern __thread Scheduler *localScheduler__;

void
T::spawn(TaskFn fn, TaskFnArg arg,
         void *caller_ctx, bool detached, TaskType type) {
    Task *t = T::alloc_task(fn,arg,caller_ctx,detached,type);
    T::spawn_task(t);
}

void T::remote_spawn(TaskFn fn, TaskFnArg fn_arg,
                      void *ctx, bool detached,
                     TaskType ty, RemoteSpawnPolicy p) {
    Scheduler *s = localScheduler__;
    s->s_cmd_.set_remote_spawn(fn, fn_arg, ctx, detached, ty, p);
    s->switch_to_sched_();
}

Task *
T::alloc_task(TaskFn fn, TaskFnArg arg, void *caller_ctx,
                    bool detached, TaskType type) {
    Task *t;
    Scheduler *s = localScheduler__;
    if (s) { // we are in trt context
        TaskBase *parent_base = s->s_current_;
        Task *parent = static_cast<Task *>(parent_base);
        assert(dynamic_cast<Task *>(parent_base) != nullptr);
        s->task_alloc(&t, fn, arg, caller_ctx, parent, detached, type);
        assert(!s->s_cmd_.is_set());
    } else {
        // not in trt context. There is at least one case where we want to
        // handle this properly: pushing tasks remotely to a trt scheduler
        // without a trt context.
        Task *parent = nullptr;
        t = TaskAllocationQueue::task_alloc<Task>(fn, arg, caller_ctx, parent, detached, type);
    }
    return t;
}

void
T::free_task(Task *t) {
    Scheduler *s = localScheduler__;
    if (s) {
        s->task_free(t);
    } else {
        TaskAllocationQueue::task_free(t);
    }
}

void
T::spawn_task(Task *t) {
    //trt_dmsg("%s (%p)\n", __PRETTY_FUNCTION__, t);
    Scheduler *s = localScheduler__;
    s->s_cmd_.set_spawn(t);
    // NB: schedulers do not go away (see Controller), so this pointer
    // should always be valid even if the scheduler has stopped.
    s->switch_to_sched_();
}

void
T::spawn_many(Task::List &tl) {
    Scheduler *s = localScheduler__;
    s->s_cmd_.set_spawn_many(tl);
    s->switch_to_sched_();
}

Future *
T::task_wait_(void) {
    // NB: add a check in case we have multiple task types in the future
    assert(dynamic_cast<Task *>(localScheduler__->s_current_) != nullptr);
    Task *self = static_cast<Task *>(localScheduler__->s_current_);
    return self->t_ws_.wait_();
}

std::tuple<RetT, void *>
T::task_wait(void) {
    Future *f = T::task_wait_();
    assert(f->is_ready());
    auto ret = std::make_tuple(f->get_val(), f->get_ctx());
    f->drop_ref();
    return ret;
}

FutureBase *
T::wait_(WaitsetBase *ws) {
    return ws->wait();
}

std::tuple<RetT, void *>
T::wait(WaitsetBase *ws) {
    FutureBase *f = T::wait_(ws);
    assert(f->is_ready());
    auto ret = std::make_tuple(f->get_val(), f->get_ctx());
    f->drop_ref();
    return ret;
}

void T::yield(void) {
    //trt_dmsg("%s\n", __PRETTY_FUNCTION__);
    Scheduler *s = localScheduler__;
    assert(!s->s_cmd_.is_set());
    s->s_cmd_.set_yield();
    s->switch_to_sched_();
}

void T::notify(AsyncObjBase *ao, RetT val, NotifyPolicy p) {
    //trt_dmsg("%s\n", __PRETTY_FUNCTION__);
    Scheduler *s = localScheduler__;
    assert(!s->s_cmd_.is_set());
    s->s_cmd_.set_notify(ao, val, p);
    s->switch_to_sched_();
}

void T::notify_init(void) {
    Scheduler *s = localScheduler__;
    assert(!s->s_cmd_.is_set());
    s->s_cmd_.init_notify();
}

bool T::notify_add(AsyncObjBase *ao, RetT val, NotifyPolicy p) {
    return localScheduler__->s_cmd_.notify_add(ao, val, p);
}

void T::notify_submit(void) {
    //trt_dmsg("%s\n", __PRETTY_FUNCTION__);
    localScheduler__->switch_to_sched_();
}

RetT
T::local_single_wait(LocalSingleAsyncObj *lsao) {

    // printf("%s: ready: %u\n", __PRETTY_FUNCTION__, lsao->is_ready());
    if (!lsao->is_ready()) {
        Scheduler *s = localScheduler__;
        lsao->set_waiter(s->s_current_);
        s->s_cmd_.set_local_single_wait(lsao);
        s->switch_to_sched_();
        // we could loop if the lsao is not ready, but I cannot think of a
        // reason why this would happen.
        assert(lsao->is_ready());
    }

    return lsao->get_ret();
}

void T::local_single_notify_init(void) {
    Scheduler *s = localScheduler__;
    assert(!s->s_cmd_.is_set());
    s->s_cmd_.init_local_single_notify();
}

bool T::local_single_notify_add(LocalSingleAsyncObj *lsao, RetT ret) {
    return localScheduler__->s_cmd_.local_single_notify_add(lsao, ret);
}

void T::local_single_notify_submit(void) {
    localScheduler__->switch_to_sched_();
}

void T::local_single_notify(LocalSingleAsyncObj *lsao, RetT ret) {
    //trt_dmsg("%s\n", __PRETTY_FUNCTION__);
    T::local_single_notify_init();
    T::local_single_notify_add(lsao, ret);
    T::local_single_notify_submit();
}

#if 0
void T::set_done(void) {
    localScheduler__->s_ctl_.set_app_done();
}

bool T::is_done(void) {
    return localScheduler__->s_ctl_.is_app_done();
}
#endif

size_t T::rand(void) {
    return localScheduler__->rand();
}

#if !defined(NDEBUG)
uint64_t T::tid(void) {
    Scheduler *s = localScheduler__;
    if (s == NULL || s->s_current_ == NULL)
        return (uint64_t)-1;
    return s->s_current_->t_dbg_id_;
}

uint64_t T::sid(void) {
    Scheduler *s = localScheduler__;
    return s ? s->s_dbg_id_ : (uint64_t)-1;
}
#endif


void task_return(RetT ret) {
    //trt_dmsg("%s\n", __PRETTY_FUNCTION__);
    Scheduler *s = localScheduler__;
    assert(!s->s_cmd_.is_set());
    s->s_cmd_.set_ret(ret);
    s->switch_to_sched_();
    fprintf(stderr, "Should not reach here");
    abort();
}

void Task::dealloc_task__(AsyncObj *unused, void *t__) {
    Task *t = static_cast<Task *>(t__);
    //printf("%s\n", __PRETTY_FUNCTION__);
    localScheduler__->task_free(t);
}

Task &T::self(void) {
    //printf("===> S=%p\n", localScheduler__);
    //printf("===> T=%p\n", localScheduler__->s_current_);
    assert(dynamic_cast<Task *>(localScheduler__->s_current_) != nullptr);
    Task *self = static_cast<Task *>(localScheduler__->s_current_);
    return *self;
}

void T::set_exit_all(void) {
    localScheduler__->s_controller_.set_exit_all();
}

Scheduler *T::getS(void) {
    return localScheduler__;
}


} // end namespace trt

extern "C" {

// overload jctx_end() (which is a weak symbol)
void jctx_end(void *ret)
{
    //dmsg("implicit task return\n");
    trt::task_return((uint64_t)ret);
}

#if !defined(NDEBUG)
uint64_t trt_dbg_get_tid(void) { return trt::T::tid(); }
uint64_t trt_dbg_get_sid(void) { return trt::T::sid(); }
#endif

} // end extern "C"
