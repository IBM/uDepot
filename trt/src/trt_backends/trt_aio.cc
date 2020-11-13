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

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_aio.hh"

namespace trt {

thread_local AioState AioState__;

AioState *get_tls_AioState__() {
    return &AioState__;
}

int
AioOpBase::submit() {
    assert(aio_state_ == State::INITIALIZED);
    // NB: directly submitting as done below does not have an effect in
    // performance. Just keeping it here as a reminder.
    //int ret = get_tls_AioState__()->submit(1, &aio_iocbp_);
    int ret = get_tls_AioState__()->submit(&aio_iocb_);
    if (ret == -1) { // TODO: we need to properly handle errors here
        fprintf(stderr, "%s: submit I/O failed: %s (%d)\n", __PRETTY_FUNCTION__, strerror(errno), errno);
        aio_state_ = State::IO_ERROR;
        return -1;
    }

    T::io_npending_inc(1);
    aio_state_ = State::IO_SUBMITTED;
    return 0;
}

static ssize_t
aio_op_submit_and_wait(uint16_t op, int fd, void *buff, size_t n, off_t off)
{
    AioOp aio_op {op, fd, buff, n, off};
    //trt_dmsg("Submitting\n");
    int submit_ret = aio_op.submit();
    if (submit_ret == -1)
        return (ssize_t)-1;

    //trt_dmsg("Waiting...\n");
    RetT ret = aio_op.wait();
    // XXX: is casting going to work as expected here? I can never remember...
    return (ssize_t)ret;
}

// so that we don't have to include the uapi header at .hh
RetT AioOp::wait() {
    return T::local_single_wait(&aio_lsaobj_);
}

void AioOp::complete(RetT val) {
    //trt_dmsg("AioOp\n");
    bool ret = T::local_single_notify_add(&aio_lsaobj_, val);
    if (!ret) { // flush notifications and retry
        T::local_single_notify_submit();
        T::local_single_notify_init();
        ret = T::local_single_notify_add(&aio_lsaobj_, val);
        assert(ret);
    }
}

void AioOpCallback::complete(RetT val) {
    //trt_dmsg("AioOpCallback\n");
    if (aio_callback_) {
        aio_callback_(this, val);
    } else {
        fprintf(stderr, "[%s +%d]: %s: no callback set\n", __FILE__, __LINE__, __FUNCTION__);
        abort();
    }
}


ssize_t
AIO::pread(int fd, void *buff, size_t nbytes, off_t off) {
    //trt_dmsg("pread\n");
    return aio_op_submit_and_wait(IOCB_CMD_PREAD, fd, buff, nbytes, off);
}

ssize_t
AIO::pwrite(int fd, const void *buff, size_t nbytes, off_t off) {
    //trt_dmsg("pwrite\n");
    return aio_op_submit_and_wait(IOCB_CMD_PWRITE, fd, (void *)buff, nbytes, off);
}
ssize_t
AIO::preadv(int fd, const struct iovec *iov, int iovcnt, off_t off) {
    return aio_op_submit_and_wait(IOCB_CMD_PREADV, fd, (void *)iov, iovcnt, off);
}

ssize_t
AIO::pwritev(int fd, const struct iovec *iov, int iovcnt, off_t off) {
    return aio_op_submit_and_wait(IOCB_CMD_PWRITEV, fd, (void *)iov, iovcnt, off);
}


void *
AIO::poller_task(void *unused) {
    // struct timespec ts_; ts_.tv_nsec = 1; ts_.tv_sec = 0;
    struct timespec *ts = NULL;
    const long min_events = 1;
    const long max_events = 8; //AioState::aio_maxio_ / 2;
    struct io_event *events;
    int nevents;

    events = (struct io_event *)malloc(sizeof(*events)*max_events);
    if (!events) {
        perror("malloc");
        exit(1);
    }

    trt_dmsg("aio poller starts\n");
    // main polling loop
    while (!AioState__.is_done()) {
        // poll for events
        nevents = AioState__.getevents(min_events, max_events, events, ts);
        //trt_dmsg("nevents=%d\n", nevents);
        if (nevents < 0) {
            perror("io_getevents");
            exit(1);
        } else if (nevents == 0) {
            T::yield();
            continue;
        }

        // we have some events, run their completions
        //
        // NB: we need to call local_single_notify_init() because some events
        // (AioOps) need to submit events for their completion.
        T::local_single_notify_init();
        for (int i=0; i<nevents; i++) {
            struct io_event *ev = &events[i];
            AioOpBase *aio = (AioOpBase *)ev->data;

            RetT val;
            if (ev->res2 != 0) {
                assert(ev->res2 < 0);
                fprintf(stderr, "aio event error: %s\n", strerror(ev->res2));
                aio->set_error();
                val = (RetT)-1;
            } else {
                aio->set_done();
                val = (RetT)ev->res;
            }

            aio->complete(val);
        }
        T::local_single_notify_submit();

        T::io_npending_dec(nevents);
    }
    trt_dmsg("aio poller DONE\n");

    free(events);
    return nullptr;
}
// in some cases, we might want to perform a "dummy" read, i.e., memcpy data
// from a buffer istead of actually doing IO. In other words, we run the
// completion in the request, and not in the context of the poller.
void
AioOpBase::fake_read(const void *src, size_t src_nbytes) {
    if (aio_state_ != State::INITIALIZED) {
        std::cerr << "performing read on an ioop that is not initialized\n";
        abort();
    }
    if (aio_iocb_.aio_lio_opcode != IOCB_CMD_PREAD) {
        std::cerr << "performing read on an opcode that is not read\n";
        abort();
    }

    size_t cp_nbytes = std::min(src_nbytes, (size_t)aio_iocb_.aio_nbytes);
    memcpy((void *)aio_iocb_.aio_buf, src, cp_nbytes);
    set_done();
    T::local_single_notify_init();
    complete(cp_nbytes);
    T::local_single_notify_submit();
}


} // end namespace trt
