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

// Local (i.e., across a single core) synchronization primitives.

#ifndef TRT_LOCAL_SYNC_HH_
#define TRT_LOCAL_SYNC_HH_

#include <inttypes.h>

#include "trt/sync_base_types.hh"
#include "trt/task.hh"

namespace trt {

class LocalWaitset;
class LocalFuture;
class LocalAsyncObj;

// TODO: pull some of the common functionality of LocalFuture* and AsyncObj*
// here or in a new WaitsetCommon class
class LocalWaitsetBase  : public WaitsetBase {};
class LocalFutureBase   : public FutureBase  { friend LocalWaitset; };
class LocalAsyncObjBase : public AsyncObjBase {};


class LocalWaitset : public LocalWaitsetBase {
   public:
    enum class State {
        INITIAL = 1,
        WAITING,
    };

    LocalWaitset(LocalWaitset const &) = delete;
    void operator=(LocalWaitset const &) = delete;

    LocalWaitset(); /* use T::self() as task */
    LocalWaitset(Task &t) : lws_owner_(t), lws_state_(State::INITIAL) {}

    // add a future to the waitset.
    //  used by the owner.
    void add_future(FutureBase &f);
    void remove_future(FutureBase &f);

    // used by scheduler to make the transition to the waiting state
    // return value:
    //  true:  transition succesfull, do nothing
    //  false: transition failed, please reschedule task
    virtual bool try_set_state_to_waiting(void) override;

    virtual TaskBase *set_ready(void);

    FutureBase *wait(void);
    LocalFutureBase *wait_(void);

    size_t nfutures(void) {
        return lws_futures_unchecked_.size() + lws_futures_registered_.size();
    }

   protected:
    Task &lws_owner_;
    FutureBase::WsList lws_futures_unchecked_;
    FutureBase::WsList lws_futures_registered_;
    State lws_state_;

};

class LocalFuture : public LocalFutureBase {
    friend LocalWaitset;

   public:
    void operator=(LocalFuture const &) = delete;
    LocalFuture(LocalFuture const &) = delete;
    LocalFuture(LocalAsyncObj *laobj, LocalWaitset *lws, void *ctx);
    void operator=(LocalFuture &&f);

    virtual RetT get_val() override;
    virtual bool is_ready() override;
    virtual void *get_ctx() override;
    virtual void drop_ref() override;

    void wait_completed(void) override;

    virtual ~LocalFuture();

   protected:
    LocalAsyncObj *lf_laobj_;
    LocalWaitset *lf_lws_;
    void *lf_caller_ctx_;
    bool lf_registered_;

protected:
    bool is_ready_or_register() override;
    TaskBase *set_ready() override;
};

class LocalAsyncObj : public LocalAsyncObjBase  {
    friend LocalFuture;
public:
    enum class State {
        // INVALID,      // when the default constructor is used
        IN_PROGRESS,
        READY,
    };

    using deallocFnArg = void *;
    using deallocFn = void(*)(LocalAsyncObj *ao, void *);

    LocalAsyncObj(LocalAsyncObj const&) = delete;
    void operator=(LocalAsyncObj const&) = delete;

    LocalAsyncObj(deallocFn de_fn, deallocFnArg de_fn_arg);
    LocalAsyncObj() : LocalAsyncObj(nullptr, nullptr) {}

    virtual FutureBase::AoList set_ready(RetT val) override;

protected:
    void unsubscribe();
    void subscribe();

    bool is_ready() const;
    bool is_ready_or_register(LocalFuture &lf);
    RetT get_val() const;

private:
    State lao_state_;
    uint32_t lao_refcnt_;
    RetT  lao_ret_;
    LocalFuture::AoList lao_futures_;
    deallocFn    lao_dealloc_fn_;
    deallocFnArg lao_dealloc_fn_arg_;
};

// Asynchronous objects that can  hold many values over the lifetime, and their
// corresponding futures.
//
// In the asyncornous object, we maintain a version number to distinguish
// between different values. In contrast with LocalAsyncObj, the updater
// reference does not go away when "notifying" the object (internally this
// happens by calling ->set_ready()). Instead, the user needs to explicitly call
// release()
//
// The future object holds the last value read, and uses it to check for new
// values.
//
class LocalAsyncObjMany;
class LocalFutureMany;

class LocalFutureMany : public LocalFutureBase {
    friend LocalWaitset;

   public:
    void operator=(LocalFutureMany const &) = delete;
    LocalFutureMany(LocalFutureMany const &) = delete;
    LocalFutureMany(LocalAsyncObjMany *laobj, LocalWaitset *lws, void *ctx);
    LocalFutureMany() : LocalFutureMany(nullptr, nullptr, nullptr) {}
    LocalFutureMany(LocalFutureMany &&);
    void operator=(LocalFutureMany &&f);

    virtual RetT get_val() override;
    virtual bool is_ready() override;
    virtual void *get_ctx() override;
    virtual void drop_ref() override;

    void wait_completed(void) override;

    std::tuple<uint64_t, RetT> get_versioned_val();

    void reset();

    virtual ~LocalFutureMany();

   protected:
    LocalAsyncObjMany *lfm_laobj_;
    LocalWaitset *lfm_lws_;
    void *lfm_caller_ctx_;
    bool lfm_registered_;
    uint64_t lfm_last_version_read_;

protected:
    bool is_ready_or_register() override;
    TaskBase *set_ready() override;
};

class LocalAsyncObjMany : public LocalAsyncObjBase  {
    friend LocalFutureMany;

public:
    using deallocFnArg = void *;
    using deallocFn = void(*)(LocalAsyncObjMany *ao, void *);

    LocalAsyncObjMany(deallocFn de_fn, deallocFnArg de_fn_arg);
    LocalAsyncObjMany() : LocalAsyncObjMany(nullptr, nullptr) {}

    LocalAsyncObjMany(LocalAsyncObjMany const&) = delete;
    void operator=(LocalAsyncObjMany const&) = delete;

    // For updated, object is not released after notification, because there
    // might be multiple notifications (one per different value). Updaters need
    // to releas the object explicitly.
    void release() { unsubscribe(); }

protected:
    virtual FutureBase::AoList set_ready(RetT val) override;

    void unsubscribe();
    void subscribe();

    bool is_ready(uint64_t version) const;
    bool is_ready_or_register(uint64_t version, LocalFutureMany &lf);
    std::tuple<uint64_t, RetT> get_val() const;

    void unregister_maybe(LocalFutureMany &f);

private:
    uint32_t lao_refcnt_;
    RetT  lao_ret_;
    LocalFuture::AoList lao_futures_;
    deallocFn    lao_dealloc_fn_;
    deallocFnArg lao_dealloc_fn_arg_;
    uint64_t lao_version_;
};

// Allows defining a timeout.
class LocalWaitsetTimeout : public LocalWaitset {
};


} // end namespace trt

#endif // end TRT_LOCAL_MANY_SYNC_HH_
