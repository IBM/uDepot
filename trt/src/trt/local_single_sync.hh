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

// Our original asynchrnous objects work across multiple cores and can support
// multiple waiters via waitsets. LocalSingleAsyncObj is a simplified version
// where, everything happens on one core and there is a single task waiting on
// the result.

#ifndef TRT_LOCAL_SINGLE_SYNC_HH_
#define TRT_LOCAL_SINGLE_SYNC_HH_

#include <cassert>
#include <cinttypes>
#include <tuple>

#include "trt/common.hh"
#include "trt/task_base.hh"

class Scheduler;

namespace trt {

class LocalSingleAsyncObj {
    friend Scheduler;

   public:
    enum class State {
        INVALID, // lsao_ret_ is invalid
        READY,   // lsao_ret_ is valid
    };

   protected:
    State     lsao_state_;
    RetT      lsao_ret_;       // valid if ao_state_ == READY
    TaskBase *lsao_task_;      // task waiting on value (or nullptr)
   public:
    uintptr_t lsao_user_data_;

   public:
    void operator=(LocalSingleAsyncObj const &) = delete;
    LocalSingleAsyncObj(LocalSingleAsyncObj const &) = delete;

    LocalSingleAsyncObj(LocalSingleAsyncObj &&o)
        : lsao_state_(std::move(o.lsao_state_))
        , lsao_ret_(std::move(o.lsao_ret_))
        , lsao_task_(std::move(o.lsao_task_))
        , lsao_user_data_(std::move(o.lsao_user_data_))
        {}

    void operator=(LocalSingleAsyncObj &&o) {
        std::swap(lsao_state_, o.lsao_state_);
        std::swap(lsao_ret_, o.lsao_ret_);
        std::swap(lsao_task_, o.lsao_task_);
    }

    // initialize at invalid state
    LocalSingleAsyncObj()
        : lsao_state_(State::INVALID),
          lsao_ret_(-666), // avoid unitialized warnings :/
          lsao_task_(nullptr) {}

    void set_val(RetT val) {
        assert(lsao_state_ == State::INVALID);
        lsao_ret_ = val;
        lsao_state_ = State::READY;
    }

    void set_waiter(TaskBase *t) {
        assert(lsao_task_ == nullptr);
        lsao_task_ = t;
    }

    bool is_ready(void) {
        return lsao_state_ == State::READY;
    }

    RetT get_ret(void) {
        assert(lsao_state_ == State::READY);
        return lsao_ret_;
    }
};

} // end namespace trt

#endif // TRT_LOCAL_SINGLE_SYNC_HH_
