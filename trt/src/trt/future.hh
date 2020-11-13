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

#ifndef TRT_FUTURE_HH_
#define TRT_FUTURE_HH_

#include "trt/async_obj.hh"


namespace trt {

class Waitset;
class Scheduler;
class Task;

// A future is essentially a reference to an asynchronous object (AsyncObj). It
// provides the link between the asynchronous object and a waitset for waiting
// for the asynchronous result to become available.
//
// Futures are only accessible from a single task, and hence do not require
// synchronization.
//
// A future with f_aobj_ == nullptr is invalid
// A valid feature holds a reference to the f_aobj_ object.
//
// In the current implementation, waiting for a future can only be done via a
// waitset. The waitset is specified at construction time.
//
// Once the the waitset returns the future to the user (see Waitset::wait_()) it
// calls Future::wait_completed() that sets f_waitset_ to nullptr.
//
// if f_registered_ is set, the future is registered in f_aobj_
//
// TODO: split this into a FutureBase and two implementations:
//   - Future, which we can wait directly
//   - Waitset::Future, which we can wait only via the waitset
class Future : FutureBase {
    friend AsyncObj;
    friend Waitset;
    friend Scheduler;
    friend Task;

   protected:
    AsyncObj         *f_aobj_; // if null, future is INVALID
    Waitset          *f_waitset_;
    void             *f_caller_ctx_;
    bool              f_registered_;

   public:
    Future(AsyncObj *aobj, Waitset *ws, void *caller_ctx);
    Future() : Future(nullptr, nullptr, nullptr) {};

    void operator=(Future &&f) {
        std::swap(f_aobj_, f.f_aobj_);
        std::swap(f_waitset_, f.f_waitset_);
        std::swap(f_caller_ctx_, f.f_caller_ctx_);
        std::swap(f_registered_, f.f_registered_);
        // For whaterver reason, std::swap() does not work here
        // std::swap(f_lnode_ws_, f.f_lnode_ws_);
        // std::swap(f_lnode_ao_, f.f_lnode_ao_);
        f_lnode_ws_.swap_nodes(f.f_lnode_ws_);
        f_lnode_ao_.swap_nodes(f.f_lnode_ao_);
    };

    //Future(Future const &) { fprintf(stderr, "NYI"); abort(); }
    // We might want to implement (some of) these eventually
    void operator=(Future const &) = delete;
    Future(Future const &) = delete;

    ~Future();

   protected:
    // returns nullptr or a task to schedule. Should be called in scheduler
    inline Task *set_ready_(void);

    virtual bool is_ready_or_register() override;
    virtual void wait_completed(void) override {
        assert(f_waitset_);
        f_waitset_ = nullptr;
    }

   public:
    bool is_invalid(void) { return f_aobj_ == nullptr && f_waitset_ == nullptr; }
    bool is_ready(void) override;
    RetT get_val(void) override;
    void *get_ctx(void) override { return f_caller_ctx_; }
    void drop_ref(void) override ;

    TaskBase *set_ready(void) override;
};

inline bool Future::is_ready() {
    return f_aobj_->is_ready();
}

inline bool Future::is_ready_or_register() {
    assert(!f_registered_);
    bool ret = f_aobj_->is_ready_or_register(*this);
    if (!ret)
        f_registered_ = true;
    return ret;
};


inline RetT Future::get_val() {
    return f_aobj_->get_val();
}

inline void Future::drop_ref() {
    assert(f_waitset_ == nullptr); // wait_completed() should have been called by now
    f_aobj_->unsubscribe_();
    f_aobj_ = nullptr;
}

inline Future::~Future() {
    if (f_aobj_)
        drop_ref();
    // printf("f_aobj_: %p\n", f_aobj_);
    // printf("%s: f_lnode_ws_ linked: %u\n", __FUNCTION__, f_lnode_ws_.is_linked());
    // printf("%s: f_lnode_ao_ linked: %u\n", __FUNCTION__, f_lnode_ao_.is_linked());
}

} // end namespace trt

#endif // TRT_FUTURE_HH_
