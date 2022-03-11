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
#include "trt_backends/uring.hh"
#include "trt_backends/trt_iou.hh"

namespace trt {

thread_local UringState IouState__;

UringState *get_tls_IouState__() {
    return &IouState__;
}

int
IouOp::submit() {
    assert(iou_state_ == State::INITIALIZED);
    // NB: directly submitting as done below does not have an effect in
    // performance. Just keeping it here as a reminder.
    //int ret = get_tls_IouState__()->submit(1, &iou_iocbp_);
    struct io_uring_sqe *sqe = get_tls_IouState__()->get_sqe();
    if (nullptr == sqe) {
	errno = ENOMEM;
        fprintf(stderr, "%s: submit I/O failed: %s (%d)\n", __PRETTY_FUNCTION__, strerror(errno), errno);
        iou_state_ = State::IO_ERROR;
        return -1;
    }
    if (op_code_ == IouOpCode::PWRITEV) {
	io_uring_prep_writev(sqe,fd_,iovec_,iovec_cnt_,offset_);
    } else {
	io_uring_prep_readv(sqe,fd_,iovec_,iovec_cnt_,offset_);
    }
    // If fixed file
    // if(FLOW_KNOBS->IO_URING_POLL) sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe, this);
    int ret = get_tls_IouState__()->submit(&iou_iocb_);
    if (ret == -1) { // TODO: we need to properly handle errors here
        fprintf(stderr, "%s: submit I/O failed: %s (%d)\n", __PRETTY_FUNCTION__, strerror(errno), errno);
        iou_state_ = State::IO_ERROR;
        return -1;
    }

    T::io_npending_inc(1);
    iou_state_ = State::IO_SUBMITTED;
    return 0;
}

static ssize_t
iou_op_submit_and_wait(uint16_t op, int fd, struct iovect *iov, int iovcnt, off_t off)
{
    IouOp iou_op {op, fd, iov, iovcnt, off};
    //trt_dmsg("Submitting\n");
    int submit_ret = iou_op.submit();
    if (submit_ret == -1)
        return (ssize_t)-1;

    //trt_dmsg("Waiting...\n");
    RetT ret = iou_op.wait();
    // XXX: is casting going to work as expected here? I can never remember...
    return (ssize_t)ret;
}

// so that we don't have to include the uapi header at .hh
RetT IouOp::wait() {
    return T::local_single_wait(&iou_lsaobj_);
}

void IouOp::complete(RetT val) {
    //trt_dmsg("IouOp\n");
    bool ret = T::local_single_notify_add(&iou_lsaobj_, val);
    if (!ret) { // flush notifications and retry
        T::local_single_notify_submit();
        T::local_single_notify_init();
        ret = T::local_single_notify_add(&iou_lsaobj_, val);
        assert(ret);
    }
}

void IouOpCallback::complete(RetT val) {
    //trt_dmsg("IouOpCallback\n");
    if (iou_callback_) {
        iou_callback_(this, val);
    } else {
        fprintf(stderr, "[%s +%d]: %s: no callback set\n", __FILE__, __LINE__, __FUNCTION__);
        abort();
    }
}


ssize_t
IOU::pread(int fd, void *buff, size_t nbytes, off_t off) {
    //trt_dmsg("pread\n");
    struct iovec iov = (struct iovec){.iov_base = buff, .iov_len = len};
    return IOU::preadv(fd, &iov, 1, off);
}

ssize_t
IOU::pwrite(int fd, const void *buff, size_t nbytes, off_t off) {
    //trt_dmsg("pwrite\n");
    struct iovec iov = (struct iovec){.iov_base = (void *)buff, .iov_len = len};
    return IOU::pwritev(fd, &iov, 1, off);
}

ssize_t
IOU::preadv(int fd, const struct iovec *iov, int iovcnt, off_t off) {
    return iou_op_submit_and_wait(IOCB_CMD_PREADV, fd, (void *)iov, iovcnt, off);
}

ssize_t
IOU::pwritev(int fd, const struct iovec *iov, int iovcnt, off_t off) {
    return iou_op_submit_and_wait(IOCB_CMD_PWRITEV, fd, (void *)iov, iovcnt, off);
}


void *
IOU::poller_task(void *unused) {
    // struct timespec ts_; ts_.tv_nsec = 1; ts_.tv_sec = 0;
    struct timespec *ts = NULL;
    const long max_cqes = IouState::iou_maxio_ / 2;
    struct io_uring_cqe * cqes_batch;

    cqes_batch = (struct io_uring_cqe *) malloc(sizeof(*cqes_batch)*max_cqes);
    if (!events) {
        perror("malloc");
        exit(1);
    }

    trt_dmsg("iou poller starts\n");
    // main polling loop
    while (!IouState__.is_done()) {

        // poll for events
	int cqe_nr = IouState__.peek_batch_cqe(max_events, cqes_batch);
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
        // (IouOps) need to submit events for their completion.
        T::local_single_notify_init();
        for (int i = 0; i < cqe_nr; i++) {
	    struct io_uring_cqe *cqe = cqes_batch[e];
	    // TODO: handle completion
	    IOBlock * const iob = static_cast<IOBlock*>(io_uring_cqe_get_data(cqe));

            RetT val;
            if (cqe->res != 0) {
                assert(cqe->res < 0);
                fprintf(stderr, "iou event error: %s\n", strerror(cqe->res));
                iou->set_error();
                val = (RetT)-1;
            } else {
                iou->set_done();
                val = (RetT)cqe->res;
            }

            iou->complete(val);
	    io_uring_cqe_seen(&ctx.ring, cqe);
        }
        T::io_npending_dec(cqe_nr);

        T::local_single_notify_submit();

    }
    trt_dmsg("iou poller DONE\n");

    free(events);
    return nullptr;
}
// in some cases, we might want to perform a "dummy" read, i.e., memcpy data
// from a buffer istead of actually doing IO. In other words, we run the
// completion in the request, and not in the context of the poller.
void
IouOp::fake_read(const void *src, size_t src_nbytes) {
    if (iou_state_ != State::INITIALIZED) {
        std::cerr << "performing read on an ioop that is not initialized\n";
        abort();
    }
    if (iou_iocb_.iou_lio_opcode != IOCB_CMD_PREAD) {
        std::cerr << "performing read on an opcode that is not read\n";
        abort();
    }

    size_t cp_nbytes = std::min(src_nbytes, (size_t)iou_iocb_.iou_nbytes);
    memcpy((void *)iou_iocb_.iou_buf, src, cp_nbytes);
    set_done();
    T::local_single_notify_init();
    complete(cp_nbytes);
    T::local_single_notify_submit();
}


} // end namespace trt
