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

#ifndef TRT_ASYNC_OBJ_HH_
#define TRT_ASYNC_OBJ_HH_

#include <atomic>
#include <utility>
#include <new>

#include <boost/intrusive/list.hpp>
namespace bi = boost::intrusive;

extern "C" {
    #include "trt_util/misc.h"  // spinlock_t
}

#include "trt/sync_base_types.hh"

namespace trt {

class AsyncObj : public AsyncObjBase {
public:
    enum class State {
        INVALID,      // when the default constructor is used
        IN_PROGRESS,
        READY,
    };

    using deallocFnArg = void *;
    using deallocFn = void(*)(AsyncObj *ao, void *);

protected:
    // An async object represents an asynchronous computation. The computation
    // is either IN_PROGRESS, or READY in which case its return value is
    // available.
    //
    // The asynchronous result is accessed via futures (see Future). There are
    // two concerns when dealing with Futures:
    //  1. The result should be available until all interested Futures get it
    //  2. A task might want to defer its execution until the result becomes
    //  available. In this case, when the result becomes available the tasks
    //  need to be notified.
    //
    // We deal with (1) with a reference count, and with (2) by keeping a list
    // of futures for waking up the corresponding tasks.
    //
    // What happens when the reference count reaches zero? Typically we need to
    // free resources. AsyncObj's constructor accepts a deallocFn function to
    // implement this resource deallocation.

    // state and result. aio_val_ is valid only if ao_state_ == READY
    std::atomic<State> ao_state_;
    std::atomic<RetT>  ao_ret_;

    // total number of futures that depend on our value
    // (plus promises, but currently we assume a single promise)
    // TODO: Can we use a std::shared_ptr instead of manual refcounting?
    std::atomic_uint_fast32_t ao_refcnt_;
    // ao_lock_ protects ao_futures_
    spinlock_t ao_lock_;
    // futures that we need to wakeup when the result becomes ready.
    FutureBase::AoList ao_futures_;

    deallocFn    ao_dealloc_fn_;
    deallocFnArg ao_dealloc_fn_arg_;

   public:
    uintptr_t    ao_user_data_;

   public:
    void operator=(AsyncObj const &) = delete;
    AsyncObj(AsyncObj const &)  = delete;

    AsyncObj() : ao_state_(State::INVALID) {}
    AsyncObj(deallocFn dealloc_fn, deallocFnArg dealloc_fn_arg)
        : ao_state_(State::IN_PROGRESS),
          ao_refcnt_(1),
          ao_dealloc_fn_(dealloc_fn),
          ao_dealloc_fn_arg_(dealloc_fn_arg) {
        spinlock_init(&ao_lock_);
    }

    // increase the reference count of the object
    void subscribe_() {
        assert(ao_state_ != State::INVALID);
        ++ao_refcnt_;
    }

    // I guess this is OK:
    void unsubscribe_(void) {
        assert(ao_state_ != State::INVALID);
        if (--ao_refcnt_ == 0) {
            ao_dealloc_fn_(this, ao_dealloc_fn_arg_);
        }
    }

    //  is_ready() check if the value is ready
    //  get_val() return the value: should be called only if is_ready() is true
    //
    // http://en.cppreference.com/w/cpp/atomic/memory_order:
    // memory_order_consume:
    //   A load operation with this memory order performs a consume
    //   operation on the affected memory location: no reads in the current
    //   thread dependent on the value currently loaded can be reordered
    //   before this load. This ensures that writes to data-dependent
    //   variables in other threads that release the same atomic variable
    //   are visible in the current thread. On most platforms, this affects
    //   compiler optimizations only.
    inline bool is_ready() const {
        assert(ao_state_ != State::INVALID);
        return ao_state_.load(std::memory_order_consume) == State::READY;
    }

    inline bool is_invalid() const {
        return ao_state_.load(std::memory_order_consume) == State::INVALID;
    }

    inline RetT get_val() const {
        assert(is_ready());
        return ao_ret_.load();
    }

    inline uintptr_t get_user_data() const {
        assert(is_ready());
        return ao_user_data_;
    }

    // races against set_ready()
    bool is_ready_or_register(FutureBase &f) {
        assert(ao_state_ != State::INVALID);
        if (is_ready())
            return true;
        // grab a lock to serialize with set_ready() that sets the value and
        // extracts the list
        bool ret;
        spin_lock(&ao_lock_);
        if (is_ready()) {
            //printf("---> future %p ready\n", &f);
            ret = true;
        } else { // value not ready, register
            //printf("---> future %p is going to be pushed\n", &f);
            ret = false;
            ao_futures_.push_back(f);
        }
        spin_unlock(&ao_lock_);

        return ret;
    }

    // races against get_val_or_register_
    FutureBase::AoList set_ready(RetT val) override {
        assert(ao_state_ == State::IN_PROGRESS);
        spin_lock(&ao_lock_);
        ao_ret_.store(val);
        // http://en.cppreference.com/w/cpp/atomic/memory_order:
        // memory_order_release:
        //   A store operation with this memory order performs the release
        //   operation: no memory accesses in the current thread can be reordered
        //   after this store. This ensures that all writes in the current thread
        //   are visible in other threads that acquire the same atomic variable
        //   and writes that carry a dependency into the atomic variable become
        //   visible in other threads that consume the same atomic.
        ao_state_.store(State::READY, std::memory_order_release);
        FutureBase::AoList l = std::move(ao_futures_);
        assert(ao_futures_.size() == 0);
        spin_unlock(&ao_lock_);

        // Once the value is set, the writer loses its reference.
        unsubscribe_();
        return l;
    }

    // This will allocate and initialize an asynchronous object.
    // The object will be deallocated when its reference count reaches zero.
    static AsyncObj *allocate() {
        void *p = std::malloc(sizeof(AsyncObj));
        if (p == nullptr) {
            return nullptr;
        }

        auto dealloc_fn = [] (AsyncObj *o, void *unused) {
            o->~AsyncObj();
            std::free(o);
        };

        AsyncObj *ret = new(p) AsyncObj(dealloc_fn, nullptr);
        return ret;
    }
};

} // end namespace trt


#endif // TRT_ASYNC_OBJ_HH_
