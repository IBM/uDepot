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

#ifndef TRT_WAITSET_HH_
#define TRT_WAITSET_HH_

#include "trt/future.hh"

namespace trt {

class Waitset : public WaitsetBase {

   friend Future;

   protected:
    enum class State {
        INITIAL = 1,
        SCANNING,
        REDO,
        WAITING,
    };

    Task &ws_owner_;
    Future::WsList ws_futures_unchecked_;
    Future::WsList ws_futures_registered_;
    std::atomic<State> ws_state_;

   public:
    Waitset(Waitset const &) = delete;
    void operator=(Waitset const &) = delete;

    Waitset(); /* use T::self() as task */
    Waitset(Task &t) : ws_owner_(t), ws_state_(State::INITIAL) {}

    bool inline cas_state_(State expected, State new_val) {
        return ws_state_.compare_exchange_weak(expected, new_val);
    }

    size_t nfutures(void) {
        return ws_futures_unchecked_.size() + ws_futures_registered_.size();
    }

   protected:
    // add a future to the waitset. Executed by the owner.
    void add_future(FutureBase &f) {
        ws_futures_unchecked_.push_back(f);
    }

    TaskBase *set_ready(void) override;

   public:
    /*
     * Check if there are ready futures. If waitset is empty return nullptr.
     * If no futures are ready, defer execution.
     * Return a ready Future, otherwise.
     *
     * It races with multiple set_ready_() calls.
     */
    Future *wait_(void);

    FutureBase *wait() override { return wait_(); }

    bool try_set_state_to_waiting(void) override {
            return cas_state_(Waitset::State::SCANNING, Waitset::State::WAITING);
    }
};

} // end namespace trt

#endif /* ifndef TRT_WAITSET_HH_ */
