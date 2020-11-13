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

#include "trt/local_sync.hh"
#include "trt/scheduler.hh"
#include "uapi/trt.hh"

namespace trt {

extern __thread Scheduler *localScheduler__;

// LocalWaitset

LocalWaitset::LocalWaitset() : LocalWaitset(T::self()) {}

void
LocalWaitset::add_future(FutureBase &f) {
    FutureBase *f_ptr = &f;
    assert(dynamic_cast<LocalFutureBase *>(f_ptr) != nullptr);
    lws_futures_unchecked_.push_back(f);
}

void
LocalWaitset::remove_future(FutureBase &f) {
    FutureBase *f_ptr = &f;
    assert(dynamic_cast<LocalFutureBase *>(f_ptr) != nullptr);

    auto iter = lws_futures_registered_.iterator_to(f);
    if (iter != lws_futures_registered_.end()) {
        lws_futures_registered_.erase(iter);
        return;
    }

    iter = lws_futures_unchecked_.iterator_to(f);
    if (iter != lws_futures_unchecked_.end()) {
        lws_futures_unchecked_.erase(iter);
        return;
    }

    fprintf(stderr, "%s: Future %p not found in registered or unchecked list\n", __PRETTY_FUNCTION__, f_ptr);
    abort();
}

bool
LocalWaitset::try_set_state_to_waiting(void) {
    if (lws_state_ == State::INITIAL) {
        lws_state_ = State::WAITING;
        return true;
    } else {
        return false;
    }
}

TaskBase *
LocalWaitset::set_ready() {
    assert(lws_state_ == State::WAITING);
    lws_state_ = State::INITIAL;
    return &lws_owner_;
}

LocalFutureBase *
LocalWaitset::wait_() {
    if (nfutures() == 0)
        return nullptr;

    LocalFutureBase *ready_future;
    assert(lws_state_ == State::INITIAL);
	for (;;) {
        for (FutureBase &f_base : lws_futures_registered_) {
            LocalFutureBase *lf = static_cast<LocalFutureBase *>(&f_base);
            //assert(dynamic_cast<LocalFutureBase *>(&f_base) != nullptr);
            if (lf->is_ready()) {
				lws_futures_registered_.erase(lws_futures_registered_.iterator_to(f_base));
                ready_future = lf;
                goto ready;
            }
        }

        while (lws_futures_unchecked_.size() > 0) {
            FutureBase &f_base = lws_futures_unchecked_.front();
            LocalFutureBase *lf = static_cast<LocalFutureBase *>(&f_base);
            lws_futures_unchecked_.pop_front();
            if (lf->is_ready_or_register()) {
                ready_future = lf;
                goto ready;
            } else { // future not ready: registered
                lws_futures_registered_.push_back(f_base);
            }
        }

		// switch to the scheduler and wait
		localScheduler__->s_cmd_.set_wait(this);
		localScheduler__->switch_to_sched_();
		//trt_dmsg("WOKE UP FROM WAIT\n");
	}

ready:
    ready_future->wait_completed();
    return ready_future;
}

FutureBase *LocalWaitset::wait(void) {
    return wait_();
}

// LocalFuture

LocalFuture::LocalFuture(LocalAsyncObj *laobj, LocalWaitset *lws, void *ctx)
    : lf_laobj_(laobj), lf_lws_(lws), lf_caller_ctx_(ctx), lf_registered_(false) {
    lf_laobj_->subscribe(); // grab a reference to the object
    lf_lws_->add_future(*this);  // add future to waitset
}

LocalFuture::~LocalFuture() {
    if (lf_laobj_)
        drop_ref();
}

void
LocalFuture::operator=(LocalFuture &&f) {
    std::swap(lf_laobj_, f.lf_laobj_);
    std::swap(lf_lws_, f.lf_lws_);
    std::swap(lf_caller_ctx_, f.lf_caller_ctx_);
    std::swap(lf_registered_, f.lf_registered_);
}

void
LocalFuture::wait_completed() {
    assert(lf_lws_);
    lf_lws_ = nullptr;
}


bool LocalFuture::is_ready_or_register() {
    assert(!lf_registered_);
    bool ret = lf_laobj_->is_ready_or_register(*this);
    if (!ret)
        lf_registered_ = true;
    return ret;
}

void LocalFuture::drop_ref() {
    assert(lf_lws_ == nullptr); // wait_completed() should have been called by now
    lf_laobj_->unsubscribe();
    lf_laobj_ = nullptr;
}

RetT LocalFuture::get_val() {
    return lf_laobj_->get_val();
}

bool LocalFuture::is_ready() {
    return lf_laobj_->is_ready();
}

void *
LocalFuture::get_ctx() {
    return lf_caller_ctx_;
}

TaskBase *
LocalFuture::set_ready() {
    return lf_lws_->set_ready();
}


// LocalAsyncObj

LocalAsyncObj::LocalAsyncObj(deallocFn de_fn, deallocFnArg de_fn_arg)
    : lao_state_(State::IN_PROGRESS)
    , lao_refcnt_(1)  // the creator holds a reference
    , lao_futures_()
    , lao_dealloc_fn_(de_fn)
    , lao_dealloc_fn_arg_(de_fn_arg) {}

FutureBase::AoList
LocalAsyncObj::set_ready(RetT val) {
    assert(lao_state_ == State::IN_PROGRESS);
    lao_ret_ = val;
    lao_state_ = State::READY;
    FutureBase::AoList l = std::move(lao_futures_);
    assert(lao_futures_.size() == 0);
    unsubscribe();
    return l;
}

void
LocalAsyncObj::unsubscribe() {
    if (--lao_refcnt_ == 0) {
        if (lao_dealloc_fn_)
            lao_dealloc_fn_(this, lao_dealloc_fn_arg_);
    }
}

void
LocalAsyncObj::subscribe() {
    assert(lao_refcnt_ > 0);
    ++lao_refcnt_;
}

bool
LocalAsyncObj::is_ready() const {
    return lao_state_ == State::READY;
}

RetT
LocalAsyncObj::get_val() const {
    assert(is_ready());
    return lao_ret_;
}

bool
LocalAsyncObj::is_ready_or_register(LocalFuture &lf) {
    if (is_ready()) {
        return true;
    } else {
        lao_futures_.push_back(lf);
        return false;
    }
}

// LocalFutureMany

LocalFutureMany::LocalFutureMany(LocalAsyncObjMany *laobj, LocalWaitset *lws, void *ctx)
    : lfm_laobj_(laobj)
    , lfm_lws_(lws)
    , lfm_caller_ctx_(ctx)
    , lfm_registered_(false)
    , lfm_last_version_read_(0) {
    // printf("%s: %p ws:%p\n", __PRETTY_FUNCTION__, this, lws);
    lfm_laobj_->subscribe(); // grab a reference to the object
    lfm_lws_->add_future(*this);  // add future to waitset
}

LocalFutureMany::LocalFutureMany(LocalFutureMany &&f) {
    // printf("%s: this:%p (ws: %p)\n", __PRETTY_FUNCTION__, this, this->lfm_lws_);
    *this = std::move(f);
}

void
LocalFutureMany::reset() {
    if (lfm_lws_) {
        lfm_lws_->remove_future(*this);
        lfm_lws_ = nullptr;
    }

    if (lfm_laobj_) {
        lfm_laobj_->unregister_maybe(*this);
        drop_ref();
        lfm_laobj_ = nullptr;
    }
}

LocalFutureMany::~LocalFutureMany() {
    reset();
}

void
LocalFutureMany::operator=(LocalFutureMany &&f) {
    // printf("%s: this:%p (ws: %p) %p f:(ws: %p)\n", __PRETTY_FUNCTION__, this, this->lfm_lws_, &f, &f.lfm_lws_);
    std::swap(lfm_laobj_, f.lfm_laobj_);
    std::swap(lfm_lws_, f.lfm_lws_);
    std::swap(lfm_caller_ctx_, f.lfm_caller_ctx_);
    std::swap(lfm_registered_, f.lfm_registered_);
    std::swap(lfm_last_version_read_, f.lfm_last_version_read_);
    f_lnode_ws_.swap_nodes(f.f_lnode_ws_);
    f_lnode_ao_.swap_nodes(f.f_lnode_ao_);
    // printf("%s: this:%p (ws: %p) %p f:(ws: %p)\n", __PRETTY_FUNCTION__, this, this->lfm_lws_, &f, &f.lfm_lws_);
}

void
LocalFutureMany::wait_completed() {
    assert(lfm_lws_);
    // re-add future to the waitset
    lfm_registered_ = false;
    lfm_lws_->add_future(*this);  // add future to waitset
}

bool LocalFutureMany::is_ready_or_register() {
    assert(!lfm_registered_);
    bool ret = lfm_laobj_->is_ready_or_register(lfm_last_version_read_, *this);
    if (!ret)
        lfm_registered_ = true;
    return ret;
}

void LocalFutureMany::drop_ref() {
    assert(lfm_lws_ == nullptr); // wait_completed() should have been called by now
    lfm_laobj_->unsubscribe();
    lfm_laobj_ = nullptr;
}

std::tuple<uint64_t, RetT>
LocalFutureMany::get_versioned_val() {
    auto ret = lfm_laobj_->get_val();
    assert(std::get<0>(ret) >= lfm_last_version_read_);
    lfm_last_version_read_ = std::get<0>(ret);
    return ret;
}

RetT LocalFutureMany::get_val() {
    return std::get<1>(get_versioned_val());
}

bool LocalFutureMany::is_ready() {
    return lfm_laobj_->is_ready(lfm_last_version_read_);
}

void *
LocalFutureMany::get_ctx() {
    return lfm_caller_ctx_;
}

TaskBase *
LocalFutureMany::set_ready() {
    return lfm_lws_->set_ready();
}

// LocalAsyncObjMany

LocalAsyncObjMany::LocalAsyncObjMany(deallocFn defn, deallocFnArg defnarg)
    : lao_refcnt_(1)
    , lao_ret_()
    , lao_futures_()
    , lao_dealloc_fn_(defn)
    , lao_dealloc_fn_arg_(defnarg)
    , lao_version_(0)
    {}

FutureBase::AoList
LocalAsyncObjMany::set_ready(RetT val) {
    lao_ret_ = val;
    lao_version_++;
    FutureBase::AoList l = std::move(lao_futures_);
    assert(lao_futures_.size() == 0);
    return l;
}

void
LocalAsyncObjMany::unsubscribe() {
    if (--lao_refcnt_ == 0) {
        if (lao_dealloc_fn_)
            lao_dealloc_fn_(this, lao_dealloc_fn_arg_);
    }
}

void
LocalAsyncObjMany::subscribe() {
    assert(lao_refcnt_ > 0);
    ++lao_refcnt_;
}

bool
LocalAsyncObjMany::is_ready(uint64_t version) const {
    return version < lao_version_;
}

bool
LocalAsyncObjMany::is_ready_or_register(uint64_t version, LocalFutureMany &lf) {
    if (is_ready(version)) {
        return true;
    } else {
        lao_futures_.push_back(lf);
        return false;
    }
}

void
LocalAsyncObjMany::unregister_maybe(LocalFutureMany &f) {
    auto iter = lao_futures_.iterator_to(f);
    if (iter != lao_futures_.end()) {
        lao_futures_.erase(iter);
    }
}

std::tuple<uint64_t, RetT>
LocalAsyncObjMany::get_val() const {
    assert(lao_version_ > 0);
    return std::make_tuple(lao_version_, lao_ret_);
}

} // end namespace trt
