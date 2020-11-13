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

#ifndef TRT_AIO_HH_
#define TRT_AIO_HH_

#include <inttypes.h>
#include <string.h> // memset()
#include <sys/uio.h> // iovec
#include <functional>
#include <iostream>

#include "trt/local_single_sync.hh"
#include "trt_util/aio.hh"

namespace trt {

//extern thread_local AioState AioState__;
AioState *get_tls_AioState__();

class AIO;


// base class for an AIO operation.
class AioOpBase {
    friend AIO;

public:

    // states:
    //  INVALID
    //    |
    //    v
    //  INITIALIZED --submit()---> IO_SUBMITTED --- wait_on_pending_io()
    //    ^                                            |
    //    |                                            |
    //    +--- reset() ----- IO_DONE <-----------------|
    //
    // If an error happens, we move to the ERROR state from which we can move
    // to READY using .reset().
    enum class State {
        INVALID, INITIALIZED, IO_SUBMITTED, IO_ERROR, IO_DONE,
    };

    static std::string state_to_str(State &s) {
        switch (s) {
         case State::INVALID: return "INVALID";
         case State::INITIALIZED: return "INITIALIZED";
         case State::IO_SUBMITTED: return "IO_SUBMITTED";
         case State::IO_ERROR: return "IO_ERROR";
         case State::IO_DONE: return "IO_DONE";
         default: return "__INVALID__";
        }
    }

protected:
    struct iocb          aio_iocb_;
    struct iocb         *aio_iocbp_;
    State                aio_state_;

public:
    AioOpBase(const AioOpBase&) = delete;
    AioOpBase& operator=(const AioOpBase&) = delete;

    AioOpBase(AioOpBase &&op)
        : aio_iocb_(std::move(op.aio_iocb_))
        , aio_iocbp_(&aio_iocb_)
        , aio_state_(std::move(op.aio_state_))
        {}

     void operator=(AioOpBase &&op) {
        std::swap(aio_iocb_, op.aio_iocb_);
        aio_iocbp_ = &aio_iocb_;
        std::swap(aio_state_, op.aio_state_);
     }

     virtual ~AioOpBase() {
        // I cannot think any legit case where this happens since the poller
        // will hold a reference to this op.
        if (aio_state_ == State::IO_SUBMITTED) {
            fprintf(stderr, "[%s +%d] ***** destructor (%s) called while state being IO_SUBMITTED\n", __FILE__, __LINE__, __FUNCTION__);
            abort();
        }
     };

    AioOpBase() : aio_state_(State::INVALID) { // invalid
        memset(&aio_iocb_, 0, sizeof(aio_iocb_));
    }


    AioOpBase(uint16_t opcode, int fd, void *buff, size_t nbytes, off_t offset)
    : AioOpBase() {
        init_op(opcode, fd, buff, nbytes, offset);
    }

    __attribute__((warn_unused_result))
    int submit();

    virtual void init_op(uint16_t opcode, int fd, void *buff, size_t nbytes, off_t offset) {
        // I cannot think any legit case where this happens since the poller
        // will hold a reference to this op.
        if (aio_state_ == State::IO_SUBMITTED) {
            fprintf(stderr, "[%s +%d] ***** initializing op (%s) while state is IO_SUBMITTED\n", __FILE__, __LINE__, __FUNCTION__);
            abort();
        }

        aio_state_ = State::INITIALIZED;
        memset(&aio_iocb_, 0, sizeof(aio_iocb_));
        aio_iocb_.aio_fildes     = fd;
        aio_iocb_.aio_lio_opcode = opcode;
        aio_iocb_.aio_reqprio    = 0;
        aio_iocb_.aio_buf        = (uintptr_t)buff;
        aio_iocb_.aio_nbytes     = nbytes;
        aio_iocb_.aio_offset     = offset;
        aio_iocb_.aio_data       = (uintptr_t)this;
        aio_iocbp_               = &aio_iocb_;
    }

    size_t nbytes(void) const {
        assert(aio_state_ != State::INVALID);
        // guard against other opcodes that have different semantics
        // (e.g., for P{READ,WRITE}V, nbytes is the size of iovs)
        assert(aio_iocb_.aio_lio_opcode == IOCB_CMD_PREAD || aio_iocb_.aio_lio_opcode == IOCB_CMD_PWRITE);
        return aio_iocb_.aio_nbytes;
    }

    State state() { return aio_state_; }
    bool is_invalid() { return aio_state_ == State::INVALID; }
    bool is_initialized() { return aio_state_ == State::INITIALIZED; }
    bool is_io_submitted() { return aio_state_ == State::IO_SUBMITTED; }
    bool is_io_done() { return aio_state_ == State::IO_DONE; }
    bool is_in_error() { return aio_state_ == State::IO_ERROR; }

    void expect_state(State s) {
#if !defined(NDEBUG)
        if (s != aio_state_) {
            std::cerr << "Expected state:" << state_to_str(s)
                      << " but got:" << state_to_str(aio_state_) << std::endl;
            abort();
        }
#endif
    }
    void expect_invalid() { expect_state(State::INVALID); }
    void expect_io_done() { expect_state(State::IO_DONE); }

    // interface for the poller:
    void set_error(void) { aio_state_ = State::IO_ERROR; }
    void set_done(void)  { aio_state_ = State::IO_DONE; }
    virtual void complete(RetT val) = 0;


    // Executed by the initiator of the request
    virtual bool is_ready() { return is_io_done(); }
    virtual RetT wait() = 0;

    // in some cases, we might want to perform a "dummy" read, i.e., memcpy data
    // from a buffer istead of actually doing IO. In other words, we run the
    // completion in the request, and not in the context of the poller.
    void fake_read(const void *src, size_t src_nbytes);
};

// AIO operation that completes via a LocalSingleAsyncObj
class AioOp : public AioOpBase {
    friend AIO;

protected:
    LocalSingleAsyncObj  aio_lsaobj_;

public:
    AioOp(const AioOp&) = delete;
    AioOp& operator=(const AioOp&) = delete;

    AioOp(AioOp &&op)
    : AioOpBase::AioOpBase(std::move(op))
    , aio_lsaobj_(std::move(op.aio_lsaobj_)) {}

     void operator=(AioOp &&op) {
        aio_lsaobj_ = std::move(op.aio_lsaobj_);
        AioOpBase::operator=(std::move(op));
     }

    AioOp(uint16_t opcode, int fd, void *buff, size_t nbytes, off_t offset)
    : AioOpBase(opcode, fd, buff, nbytes, offset)
    , aio_lsaobj_() {}

    AioOp() : AioOpBase(), aio_lsaobj_() {} //invalid

    virtual void init_op(uint16_t opcode, int fd, void *buff, size_t nbytes, off_t offset) override {
        AioOpBase::init_op(opcode, fd, buff, nbytes, offset);
        new (&aio_lsaobj_) LocalSingleAsyncObj();
    }

    // Executed in poller
    virtual void complete(RetT val) override;

    // API to be executed by the initiator of the request
    virtual bool is_ready(void) {
		bool ret = aio_lsaobj_.is_ready();
		assert(ret == AioOpBase::is_ready());
		return ret;
	}
    virtual RetT wait(void);
};

// AIO operation that completes via a callback executed by the poller
class AioOpCallback : public AioOpBase {
    friend AIO;
    using CallBackFn = std::function<void(AioOpCallback *op, RetT ret)>;

    CallBackFn aio_callback_;
public:

    using AioOpBase::AioOpBase; // ctors that use an empty callback

    template<typename... Args>
    AioOpCallback(CallBackFn cb, Args... args)
    : AioOpBase(std::forward<Args>(args)...)
    , aio_callback_(cb) {}

    virtual void complete(RetT val) override;

    void set_callback(CallBackFn cbfn) {
        aio_callback_ = cbfn;
    }

    virtual RetT wait(void) override {
        fprintf(stderr, "[%s +%d]: %s: wait not supported on AioOpCallback\n", __FILE__, __LINE__, __FUNCTION__);
        abort();
    }
};

class AIO { // using class instead of namespace for easy befriending
   public:
    // Do we want a different context per thread? we probably do.  This should be
    // called for each thread. The idea is that it will be called by the main task
    // on each scheduler.
    static inline bool is_initialized(void) { return get_tls_AioState__()->is_initialized(); }
    static inline bool is_done(void) { return get_tls_AioState__()->is_done(); }
    static inline void init(void) { get_tls_AioState__()->init(); }
    // notify aio to drain queues and not accept any more requests
    static void stop(void) { get_tls_AioState__()->stop(); }

    static ssize_t pread(int fd, void *buff, size_t nbytes, off_t off);
    static ssize_t pwrite(int fd, const void *buff, size_t nbytes, off_t off);

    static ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t off);
    static ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t off);

    static void *poller_task(void *unused);
};

} // end namespace trt;

#endif /* TRT_AIO_HH_ */
