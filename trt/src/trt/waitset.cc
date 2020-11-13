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

#include "trt/task_base.hh"
#include "trt/scheduler.hh"
#include "trt/uapi/trt.hh"

namespace trt {

extern __thread Scheduler *localScheduler__;

Waitset::Waitset() : Waitset(T::self()) {};

// This should be called after an asynchronous object pointed by this
// waitsets futures becomes availabe. Hence, if this is called at least one
// of the @ws_futures_ is ready.
//
// This should typically called in scheduler context. If it returns
// non-nullptr Task, then the task should be woken up.
//
// races with itself (multiple futures) and wait_()
TaskBase *
Waitset::set_ready(void) {
    for (;;) {
        switch (ws_state_.load(std::memory_order_relaxed)) {
            case State::WAITING:
                // The task is sleeping. Let's try to see if we are the ones
                // to wake it up. Only one of the wakers will manage to do
                // the CAS.
                if (cas_state_(State::WAITING, State::INITIAL)) {
                    // inform the caller that they need to wakeup the task
                    return &ws_owner_;
                }
                break; /* retry */

            case State::SCANNING:
                // racing with wait_(): notify the task that something just
                // became available.
                if (cas_state_(State::SCANNING, State::REDO))
                    return nullptr;
                break; /* retry */

            case State::REDO:
                 // I think it should be OK to return here. The assumption is
                 // that the wakeup code does:
                 //   - atomically set object state to ready
                 //   - atomically check waitset state
                 // If there is a memory barrier between these two, the next
                 // scan on the wakeup will catch the new state. My
                 // assumption is that using a strong model implies the
                 // proper memory barrier, but I haven't looked at C++'s
                 // memory model in detail. In any case, it should be OK for
                 // x86.
                return nullptr;

            case State::INITIAL:
                // No wait so far, do nothing
                return nullptr;

        }
    }
}


Future *
Waitset::wait_(void) {
    //trt_dmsg("ENTER nfutures=%zd\n", nfutures());
    if (nfutures() == 0) {
        //trt_dmsg("EXIT: empty\n");
        return nullptr;
    }

    assert(ws_state_.load(std::memory_order_relaxed) == State::INITIAL);
    Future *ready_future;
    while (true) {
        ws_state_.store(State::SCANNING,std::memory_order_relaxed);
        // Try registered futures
        //trt_dmsg("Registered futures: %zd\n", ws_futures_registered_.size());
        for (FutureBase &f_base : ws_futures_registered_) {
            Future *f = static_cast<Future *>(&f_base);
            // see assertion below
            //assert(dynamic_cast<Future *>(&f_base) != nullptr);

            //trt_dmsg("Checking registered future: %p\n", &f);
            if (f->is_ready()) {
                ws_futures_registered_.erase(ws_futures_registered_.iterator_to(f_base));
                ready_future = f;
                //trt_dmsg("Future ready!\n");
                goto ready;
            }
        }

        // check all unchecked futures:
        //  - if the future is ready, return it
        //  - if the future is not ready, register for notification
        while (ws_futures_unchecked_.size() > 0) {
            // remove future from unchecked list
            FutureBase &f_base = ws_futures_unchecked_.front();
            Future *f = static_cast<Future *>(&f_base);
            //
            // NB: The assertion below fails, but it shouldn't.
            // For some reason dynamic_cast<> returns null, but I don't
            // understand why. There should be no way to instantiate a
            // FutureBase class since it has pure methods, and there are no
            // other derived classes other than FutureBase. Also the vtable of
            // the object seems fine:
            //
            //   (gdb) frame 4
            //   #4  0x000000000040353b in trt::Waitset::wait_
            //   (this=0x7ffff00419f8) at src/trt/waitset.cc:118
            //   118                 assert(dynamic_cast<Future *>(&f_base) != nullptr);
            //   (gdb) info vtbl &f_base
            //   vtable for 'trt::FutureBase' @ 0x4151e0 (subobject @ 0x7ffff0049cf8):
            //   [0]: 0x405cae <trt::Future::is_ready()>
            //   [1]: 0x402364 <trt::Future::set_ready()>
            //   [2]: 0x405ccc <trt::Future::get_val()>
            //   [3]: 0x405c9c <trt::Future::get_ctx()>
            //   [4]: 0x405cea <trt::Future::drop_ref()>
            //   (gdb) print dynamic_cast<Future *>(&f_base)
            //   $1 = (trt::Future *) 0x0
            //
            // I disable the asertion(s) for now...
            // assert(dynamic_cast<Future *>(&f_base) != nullptr);
            ws_futures_unchecked_.pop_front();

            // check if async object is available
            //trt_dmsg("Checking future: %p\n", &f);
            if (f->is_ready_or_register()) {
                ready_future = f;
                goto ready;
            } else { // future not ready: registered
                ws_futures_registered_.push_back(f_base);
            }
        }

        // No available futures were found. Switch to the scheduler to
        // perform the atomic transition to State::WAITING.
        if (ws_state_.load(std::memory_order_relaxed) == State::SCANNING) {
            //trt_dmsg("%s: SLEEPING\n", __PRETTY_FUNCTION__);
            localScheduler__->s_cmd_.set_wait(this);
            localScheduler__->switch_to_sched_();
            //trt_dmsg("WOKEUP\n");
        }
    }

ready:
    ws_state_.store(State::INITIAL, std::memory_order_relaxed);
    ready_future->wait_completed();
    return ready_future;
}


} // end namespace trt
