/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *           Nikolas Ioannou (nio@zurich.ibm.com, nicioan@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4:
#ifndef IOU_HH_
#define IOU_HH_

/*
 * Simple helper for liburing
 */


// use kernel-based liburing, rather than libc's aio(7) which is implemented in
// user-space using threads. We use the kernel ABI directly.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/syscall.h>
#include <liburing.h>


// This is intended to be accesses by different tasks on the same thread. Hence,
// no synchronization is performed.
struct UringState {
    enum class State {UNINITIALIZED, READY, DRAINING, DONE};
    size_t              iou_pending_;
    State               iou_state_;
    struct io_uring     ring_;
    static const size_t iou_maxio_ = 1024;

    static const size_t iou_ioq_size_ = 128;

    struct io_uring_sqe *iou_ioq_[iou_ioq_size_];
    size_t       iou_ioq_nr_;

    UringState():
        iou_pending_(0),
        iou_state_(State::UNINITIALIZED),
        iou_ioq_nr_(0) {}

    ~UringState() {
        if (iou_state_ != State::DONE && iou_state_ != State::UNINITIALIZED) {
            fprintf(stderr, "%s: **** Did not finalize correctly\n", __PRETTY_FUNCTION__);
	    io_uring_queue_exit(&ring_);
        }
    }

    bool is_initialized(void) { return iou_state_ != State::UNINITIALIZED; }

    const char *state_str(State s) {
        switch(s) {
            case State::UNINITIALIZED:
            return "UNINITIALIZED";

            case State::READY:
            return "READY";

            case State::DRAINING:
            return "DRAINING";

            case State::DONE:
            return "DONE";

            default:
            return "UKNOWN STATE!";
        }
    }

    void init(void) {
        //printf("%s: Entry (%p)\n", __FUNCTION__, &iou_state_);
        if (iou_state_ != State::UNINITIALIZED) {
            fprintf(stderr, "%s: Invalid state: %s\n", __PRETTY_FUNCTION__, state_str(iou_state_));
            abort();
        }
	// TODO: ADD polling, other parameters. e.g.
	/*
			struct io_uring_params params;
			memset(&params, 0, sizeof(params));
			params.flags |= IORING_SETUP_SQPOLL;
			params.sq_thread_idle = 2000;
			rc = io_uring_queue_init_params(FLOW_KNOBS->MAX_OUTSTANDING, &ctx.ring, &params);*/
	int rc = io_uring_queue_init(iou_maxio_, &ring_, 0);
        if (rc < 0) {
            perror("io_queue_init failed\n");
            exit(1);
        }
        iou_state_ = State::READY;
        //printf("%s: (%p) Exit state:%s\n", __FUNCTION__, &iou_state_, state_str(iou_state_));
    }

    struct io_uring_sqe *get_sqe() { return io_uring_get_sqe(&ring_);}

    void flush_ioq_(void) {

        if (iou_ioq_nr_ == 0)
            return;

	int ret = ::io_uring_submit(&ring_);
        if (ret < 0) {
            perror("io_submit");
            abort();
        } else if ((unsigned) ret != iou_ioq_nr_) {
            perror("io_submit (we do not handle partial success)");
            abort();
        }

        iou_ioq_nr_ = 0;
    }

    // directly submit
    // int submit(size_t nr, struct iocb **iocbpp) {
    //     iou_pending_ += nr;
    //     flush_ioq_();
    //     return io_submit(aio_ctx_, nr,  iocbpp);
    // }
 
    // -1 on error. Sets errno.
    int submit(struct io_uring_sqe *sqe) {
        // TODO: return an error here if state is DRAINING
        if (iou_state_ != State::READY) {
            fprintf(stderr, "%s: invalid state: %s\n", __PRETTY_FUNCTION__, state_str(iou_state_));
            abort();
        }

        iou_ioq_[iou_ioq_nr_++] = sqe;
        iou_pending_++;
        //printf("%s: (0x%lx) iou_pending_: %zd\n", __FUNCTION__, pthread_self(), iou_pending_);

        if (iou_ioq_nr_ == iou_ioq_size_)
            flush_ioq_();

        return 0;
    }

    int peek_batch_cqe(int max_events, struct io_uring_cqe **cqes_batch) {
      if (iou_pending_ > 0) flush_ioq_();
      return ::io_uring_peek_batch_cqe(&ring_, cqes_batch, max_events);
    }

    int peek_cqe(struct io_uring_cqe **cqe) {
      if (iou_pending_ > 0) flush_ioq_();
      return ::io_uring_peek_cqe(&ring_, cqe);
    }

    // int peek_batch_cqe(int count, struct io_uring_cqe **cqes_batch) {
    //   return io_uring_peek_batch_cqe(&ring_, cqes_batch, count);
    // }

    int wait_cqe_nr(int count, struct io_uring_cqe **cqes) {
      if (iou_pending_ > 0) flush_ioq_();
      return ::io_uring_wait_cqe_nr(&ring_, cqes, count);
    }

    void cqe_seen(struct io_uring_cqe *cqe) {
       ::io_uring_cqe_seen(&ring_, cqe);
      iou_pending_--;
      if (iou_pending_ == 0 && iou_state_ == State::DRAINING) {
	done_();
      }
    }

    void stop(void) {
        if (iou_state_ != State::READY) {
            fprintf(stderr, "%s: invalid state\n", __PRETTY_FUNCTION__);
            abort();
        }

        iou_state_ = State::DRAINING;
        if (iou_pending_ == 0)
            done_();
	else {
          //printf("iou_pending: %zd\n", iou_pending_);
	  flush_ioq_();
	}
    }

    void done_(void) {
        assert(iou_state_ == State::DRAINING);
        assert(iou_pending_ == 0);
        iou_state_ = State::DONE;
	io_uring_queue_exit(&ring_);
    }

    bool is_done(void) {
        return iou_state_ == State::DONE;
    }

};

#endif /* IOU_HH_ */
