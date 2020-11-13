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


#include "trt/sync_base_types.hh"
#include "trt/task_base.hh"
#include "trt/local_single_sync.hh"
#include "trt/scheduler_cmd.hh"
#include "trt/scheduler.hh"
#include "trt/uapi/trt.hh"

#define xdbg_print_str__ "S%-10ld %20s()"
#define xdbg_print_arg__ trt::T::sid(), __FUNCTION__
#define xdbg_print(msg, fmt, args...) \
    printf(xdbg_print_str__ " " msg fmt , xdbg_print_arg__ , ##args)

#if !defined(NDEBUG)
    #define dmsg(fmt,args...) xdbg_print("",fmt, ##args)
#else
    #define dmsg(fmt,args...) do { } while (0)
#endif


namespace trt {

__thread Scheduler *localScheduler__ = nullptr;

#if !defined(NDEBUG)
uint64_t dbg_sched_id(void) {
    static std::atomic<uint64_t> cnt(0);
    return cnt.fetch_add(1);
}

uint64_t dbg_task_id(void) {
    static std::atomic<uint64_t> cnt(0);
    return cnt.fetch_add(1);
}
#endif

Scheduler::Scheduler(ControllerBase &controller, cpu_set_t cpuset,
                     TaskFn main_fn, TaskFnArg main_arg, TaskType main_type)
    : s_state_(State::INIT),
      s_current_(nullptr),
      s_cpuset_(cpuset),
      s_controller_(controller),
      s_rand_eng_(time(NULL)),
      s_io_npending_(0) {
    // Barrier is used for the controller to wait on the scheduler to
    // complete initialization of the new thread.
    pthread_barrier_init(&s_barrier_, NULL, 2);

    #if !defined(NDEBUG)
    s_dbg_id_ = dbg_sched_id();
    #endif

    Task *main_task;
    task_alloc(&main_task, main_fn, main_arg, nullptr, nullptr, true, main_type);
    if (main_task == nullptr) {
        fprintf(stderr, "Could not allocate main task. Dying ungracfully.\n");
        abort();
    }
    main_task->set_state(Task::State::READY);
    push_task_front(*main_task);
}

Scheduler::~Scheduler() {
    // TODO
    //dmsg("Scheduler destructor\n");
}

void Scheduler::schedule_task_(TaskBase *t) {
    //dmsg("Switching to task: %p\n", t);
    //assert(t->t_state_ == Task::State::READY);
    assert(t != nullptr);
    s_current_ = t;
    t->t_last_scheduler = this;
    jctx_switch(&this->s_jctx_, &t->t_jctx_);
    //dmsg("returned from task: %p\n", t);
    //assert(s_cmd_.is_set());
    handle_cmd_(s_cmd_);
    s_cmd_.reset();
}

void Scheduler::switch_to_sched_() {
    jctx_switch(&s_current_->t_jctx_, &this->s_jctx_);
}

// post-conditions: task->current should be NULL
void Scheduler::handle_cmd_(SchedulerCmd &cmd) {
    TaskBase *prev = s_current_;
    s_current_ = nullptr;

    switch (cmd.type()) {
        case Cmd::NOP:
            fprintf(stderr, "Unexpected error");
            abort();

        case Cmd::YIELD: {
            push_task_back(*prev);
        } break;

        // add child task to the queue
        case Cmd::SPAWN: {
            TaskBase *t_new = cmd.spawn.task;
            assert(prev == t_new->t_parent_);
            t_new->set_state(Task::State::READY);

            switch (prev->t_type_) {
                case TaskType::TASK:
                push_task_front(*prev);
                break;

                case TaskType::POLL:
                push_task_back(*prev);
                break;

                default:
                fprintf(stderr, "Unexpected error");
                abort();
            }

            push_task_front(*t_new);
        } break;

        // add tasks to the queue
        case Cmd::SPAWN_MANY: {
            Task::List &tl = cmd.spawn_many.tl;
            //printf("SPAWN_MANY: tl.size()=%zd\n", tl.size());
            // NB: There is a potential issue, since we enqueue everything (even
            // poll tasks) in the task queue. But it's only for the first time
            // they are scheduled, and it optimizes the common case.
            auto &q = get_tq_(TaskType::TASK);
            q.push_back(tl);
            // The previous tas has run already, push it back to its queue
            // (we push it after the list so that if they share the same queue
            // the children will be executed first)
            push_task_back(*prev);
        } break;

        case Cmd::WAIT: {
            WaitsetBase *ws = cmd.wait.ws;
            // This is the bottom-half of wait_() executed in scheduler context
            // After state is set to WAITING the task will be picked up by the
            // first set_ready_() call that transitions from WAITING to READY.
            // Try to move to WAITING state. If it fails, reschedule the task
            if (!ws->try_set_state_to_waiting())
                push_task_back(*prev);
        } break;

        case Cmd::RETURN: {
            if (prev->t_detached_) {
                // Fast path for deallocating detached tasks:
                //  This is a detached task, so we just need to decrease the
                //  reference count of the async object (no notification
                //  needed).  However, because the task is detached, there is
                //  only one reference, the one that we hold, and decreasing the
                //  reference count will call the AsyncObj deallocate callback
                //  and deallocate the task. We deallocate the task directly,
                //  instead.
                //prev->t_ret_ao_.unsubscribe_();
                task_free(prev);
            } else {
                AsyncObjBase *ao = prev->get_ret_ao();
                notify_(ao, cmd.ret.val, NotifyPolicy::LocalSched);
                // At this point the task is no longer needed. However, we
                // inline the async object for its return value in the task
                // structure. So we need to keep the task around until we are
                // done with the async object.
                //assert(prev->t_ws_.nfutures() == 0 && "Waitset contains futures");
            }
        } break;

        case Cmd::NOTIFY: {
            // This task executed its notifications, and so we can push it back
            // and execute other tasks.
            //dmsg("Cmd::NOTIFY nargs=%zd\n", cmd.notify.nargs);
            for (size_t i=0; i<cmd.notify.nargs; i++) {
                AsyncObjBase *ao;
                RetT val;
                NotifyPolicy p;
                std::tie(ao, val, p) = cmd.notify.args[i];
                notify_(ao, val, p);
            }
            //dmsg("Cmd::NOTIFY DONE: pushing task %p back\n", prev);
            push_task_back(*prev);
        } break;

        case Cmd::REMOTE_SPAWN: {
            fprintf(stderr, "%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
            abort();
        } break;

        // We 've reached this point only of the object is not ready and we need
        // to wait (see T::local_single_wait()).
        // There is no race, so there is nothing we need to do.
        // Just do some sanity checks.
        case Cmd::LOCAL_SINGLE_WAIT: {
            //fprintf(stderr, "%s:%d: Task to wait: %p lsao:%p\n", __PRETTY_FUNCTION__, __LINE__, cmd.local_single_wait.lsao->lsao_task_, cmd.local_single_wait.lsao);
            assert(cmd.local_single_wait.lsao->lsao_task_ == prev);
        } break;

        case Cmd::LOCAL_SINGLE_NOTIFY: {
            // push back the task
            push_task_back(*prev);
            //fprintf(stderr, "%s:%d: Nargs: %zd\n", __PRETTY_FUNCTION__, __LINE__, cmd.local_single_notify.nargs);
            for (size_t i=0; i<cmd.local_single_notify.nargs; i++) {
                LocalSingleAsyncObj *lsao;
                RetT val;
                std::tie(lsao, val) = cmd.local_single_notify.args[i];
                lsao->set_val(val);
                //fprintf(stderr, "%s:%d: Task to wakeup: %p lsao:%p\n", __PRETTY_FUNCTION__, __LINE__, lsao->lsao_task_, lsao);
                if (lsao->lsao_task_ != nullptr)
                    push_task_front(*(lsao->lsao_task_));
            }
        } break;

        default:
        /* not supposed to happen */
        abort();
    }
}

void Scheduler::notify_(AsyncObjBase *ao, RetT val, NotifyPolicy p) {
        FutureBase::AoList fl = ao->set_ready(val);
        //dmsg("notify_ queue size: %zd\n", fl.size());
        while (fl.size() > 0) {
            FutureBase &f = fl.front();
            fl.pop_front();
            TaskBase *t = f.set_ready();
            // dmsg("set_ready() future: %p returned %p\n", &f, t);
            if (t == nullptr)
                continue;

            switch (p) {
                case NotifyPolicy::LastTaskScheduler: {
                    if (t->t_last_scheduler == this) {
                        push_task_front(*t);
                        break;
                    }
                    bool ok = t->t_last_scheduler->remote_push_task_front(*t);
                    //printf("notify: tried to push task: %zd at scheduler %zd ok=%u\n", t->t_dbg_id_, t->t_last_scheduler->s_dbg_id_, ok);
                    if (ok)
                        break;

                    // remote push failed (e.g., the scheduler might be
                    // stopping). Not sure what more we can do here other than
                    // propagate an error or try to schedule it to the local
                    // queue. We choose the latter.
                    [[gnu::fallthrough]];

                }
                case NotifyPolicy::LocalSched:
                //dmsg("pushing task %p back\n", t);
                push_task_back(*t);
                break;

                default:
                abort();
            }
        }
}

void Scheduler::sched_setaffinity() {
    // initalization
    int err;
    err = ::sched_setaffinity(0, sizeof(s_cpuset_), &s_cpuset_);
    if (err) {
        perror("sched_setaffinity");
        exit(1);
    }
}

void Scheduler::start_() {
    assert(s_state_ == State::RUNNING); // This needs to be set before calling start_()
    auto &task_q = get_tq_(TaskType::TASK);
    auto &poll_q = get_tq_(TaskType::POLL);
    auto &remote_q = s_remote_tq_;

    const bool print_ctx_switches = false;

    // main loop
    while (true) {
        // NB: remote_q should only contain TaskType::Task
        remote_q.push_to_queue_back(task_q);

        // First if we run low on tasks (see watermarks below), try to schedule
        // some pollers to generate more tasks.
        const size_t ntasks_low = 20, ntasks_high = 25;
        if (task_q.size() < ntasks_low) {
            for (size_t p = 0; p < poll_q.size(); ++p) {
                TaskBase *t = poll_q.pop_front();
                if (t == nullptr)
                    break;
                if (print_ctx_switches)
                    dmsg("Scheduling polling task: %lu (%p) (taskq size: %zd pollq size: %zd)\n", t->t_dbg_id_, t, task_q.size(), poll_q.size());
                schedule_task_(t);
                if (task_q.size() >= ntasks_high)
                    break;
            }
        }

        // if queue is (still) empty, try to steal from other schedulers
        if (task_q.size() == 0) {
            // or maybe not.... :)
        }

        // if we have something to schedule, do so
        TaskBase *t = task_q.pop_front();
        if (t != nullptr) {
            if (print_ctx_switches)
                dmsg("Scheduling work task: %lu (%p) (taskq size: %zd pollq size: %zd)\n", t->t_dbg_id_, t, task_q.size(), poll_q.size());
            schedule_task_(t);
        } else if (poll_q.size() == 0 && s_ctl_.exit_.load()) {
            // try to exit: make sure that there is nothing in the queue
            // (something might have been added by another scheduler)
            t = task_q.pop_front_or_stop();
            if (t != nullptr) {
                // we got a task! schedule it
                if (print_ctx_switches)
                    dmsg("Scheduling work task: %lu (%p) (taskq size: %zd pollq size: %zd)\n", t->t_dbg_id_, t, task_q.size(), poll_q.size());
                schedule_task_(t);
            } else {
                // queues are empty, and exit was set. This is not a perfect
                // exit condition since not all tasks are in queues, but it
                // should be good enough for now...
                #if !defined(NDEBUG)
                s_state_ = State::DONE;
                printf("S%zd: Nothing to schedule, exiting\n", s_dbg_id_);
                #endif
                break;
            }
        }
    }
}

void Scheduler::thread_init(Scheduler *s) {
    assert(localScheduler__ == nullptr);
    localScheduler__ = s;
    s->s_self_tid_ = pthread_self();
}

} // end namespace trt
