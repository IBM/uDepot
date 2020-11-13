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

#ifndef TRT_TASK_QUEUE_H__
#define TRT_TASK_QUEUE_H__

extern "C" {
    #include "trt_util/misc.h"  // spinlock_t
}

#include "trt/task_base.hh"

namespace trt {

class TaskQueueRemote;

// thread-unsafe queue: meant for operations on one core
class TaskQueueThreadUnsafe {
    friend TaskQueueRemote;

    TaskBase::List list_;
    enum class State { RUNNING, STOPPED } state_;

   public:
    TaskQueueThreadUnsafe(const TaskQueueThreadUnsafe &) = delete;
    void operator=(const TaskQueueThreadUnsafe &) = delete;

    TaskQueueThreadUnsafe() : state_(State::RUNNING) {}

    inline size_t size(void) {
        assert(state_ == State::RUNNING);
        return list_.size();
    }

    inline TaskBase *pop_front(void) {
        assert(state_ == State::RUNNING);
        TaskBase *ret = nullptr;
        if (!list_.empty()) {
            ret = &list_.front();
            list_.pop_front();
        }
        return ret;
    }

    inline void push_back(TaskBase &t) {
        assert(state_ == State::RUNNING);
        list_.push_back(t);
    }

    inline void push_back(TaskBase::List &l) {
        assert(state_ == State::RUNNING);
        list_.splice(list_.end(), l);
    }

    inline void push_front(TaskBase &t) {
        assert(state_ == State::RUNNING);
        list_.push_front(t);
    }

    inline void push_front(TaskBase::List &l) {
        assert(state_ == State::RUNNING);
        list_.splice(list_.begin(), l);
    }

    inline void print_tasklist(const char prefix[]="\t") {
        #if !defined(NDEBUG)
        for (auto &t: list_) {
            printf("\t%llu\n", (unsigned long long)t.t_dbg_id_);
        }
        #endif
    }

    inline TaskBase *pop_front_or_stop(void) {
        TaskBase *ret = nullptr;
        if (list_.empty()) {
            state_ = State::STOPPED;
        } else {
            ret = &list_.front();
            list_.pop_front();
        }
        return ret;
    }

    bool push_front_if_running(TaskBase &t) {
        bool ret = false;
        if (state_ == State::RUNNING) {
            ret = true;
            list_.push_front(t);
        }
        return ret;
    }

    bool push_back_if_running(TaskBase &t) {
        bool ret = false;
        if (state_ == State::RUNNING) {
            ret = true;
            list_.push_back(t);
        }
        return ret;
    }
};

// queue with operations protected by a lock
class TaskQueueLocking {
    spinlock_t lock_;
    TaskQueueThreadUnsafe q_;

   public:
    TaskQueueLocking(const TaskQueueLocking &) = delete;
    void operator=(const TaskQueueLocking &) = delete;

    TaskQueueLocking() : q_() { spinlock_init(&lock_); }

    inline void lock() {
        spin_lock(&lock_);
    }
    inline void unlock() {
         spin_unlock(&lock_);
    }

    inline size_t size(void) {
        size_t ret;
        lock();
        ret = q_.size();
        unlock();
        return ret;
    }

    inline TaskBase *pop_front(void) {
        TaskBase *ret;
        lock();
        ret = q_.pop_front();
        unlock();
        return ret;
    }

    inline void push_back(TaskBase &t) {
        lock();
        q_.push_back(t);
        unlock();
    }

    inline void push_back(TaskBase::List &l) {
        lock();
        q_.push_back(l);
        unlock();
    }

    inline void push_front(TaskBase &t) {
        lock();
        q_.push_front(t);
        unlock();
    }

    inline void push_front(TaskBase::List &l) {
        lock();
        q_.push_front(l);
        unlock();
    }

    inline TaskBase *pop_front_or_stop(void) {
        TaskBase *ret = nullptr;
        lock();
        ret = q_.pop_front_or_stop();
        unlock();
        return ret;
    }

    bool push_front_if_running(TaskBase &t) {
        bool ret = false;
        lock();
        ret = q_.push_front_if_running(t);
        unlock();
        return ret;
    }
};

// This is meant as a queue for adding tasks remotely
// The idea is to minimize overhead on the destination when checking for tasks
// in case there are none
class TaskQueueRemote {
    spinlock_t lock_;
    TaskQueueThreadUnsafe q_;
    std::atomic<size_t> q_cnt_; // could, probably, use q_.size()

   public:
    TaskQueueRemote(const TaskQueueRemote &) = delete;
    void operator=(const TaskQueueRemote &) = delete;

    TaskQueueRemote() : q_(), q_cnt_(0) { spinlock_init(&lock_); }

    inline void lock() {
        spin_lock(&lock_);
    }
    inline void unlock() {
         spin_unlock(&lock_);
    }

    bool push_back_if_running(TaskBase &t) {
        bool ret = false;
        lock();
        q_cnt_++;
        ret = q_.push_back_if_running(t);
        unlock();
        return ret;
    }

    inline void push_to_queue_back(TaskQueueThreadUnsafe &in) {
        // NB: if the check shows in the profiles, we could also remove the
        // atomic field and have a normal size_t field.
        if (q_cnt_.load(std::memory_order_relaxed) == 0)
            return;
        lock();
        in.push_back(q_.list_);
        q_cnt_ = 0;
        unlock();
    }
};

}


#endif /* ifndef TRT_TASK_QUEUE_H__ */
