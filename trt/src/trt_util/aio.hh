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
#ifndef AIO_HH_
#define AIO_HH_

/*
 * Simple helper for libaio
 */


// use kernel-based libaio, rather than libc's aio(7) which is implemented in
// user-space using threads. We use the kernel ABI directly.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/syscall.h>
#include <linux/aio_abi.h>

// syscall wrappers
static inline int
io_setup(unsigned maxevents, aio_context_t *ctx) {
    return syscall(SYS_io_setup, maxevents, ctx);
}

static inline int
io_submit(aio_context_t ctx, long nr, struct iocb **iocbpp) {
    return syscall(SYS_io_submit, ctx, nr, iocbpp);
}

static inline int
io_getevents(aio_context_t ctx, long min_nr, long nr,
                 struct io_event *events, struct timespec *timeout) {
    return syscall(SYS_io_getevents, ctx, min_nr, nr, events, timeout);
}

static inline int
io_destroy(aio_context_t ctx)
{
    return syscall(SYS_io_destroy, ctx);
}

/*
 * user-space (undocumented) getevents()
 */

struct aio_ring {
	unsigned id;		 /** kernel internal index number */
	unsigned nr;		 /** number of io_events */
	unsigned head;
	unsigned tail;

	unsigned magic;
	unsigned compat_features;
	unsigned incompat_features;
	unsigned header_length;	/** size of aio_ring */

	struct io_event events[0];
};

#define AIO_RING_MAGIC	0xa10a10a1


static inline bool aio_user_check(aio_context_t aio_ctx)
{
	// NB: When debugging use the system call to make our lives easier
	#if defined(NDEBUG)
	return ((struct aio_ring *)(aio_ctx))->magic == AIO_RING_MAGIC;
	#else
	return false;
	#endif
}

static inline int aio_user_getevents(aio_context_t aio_ctx, unsigned int max, struct io_event *events)
{
	long i = 0;
	unsigned head;
	struct aio_ring *ring = (struct aio_ring*) aio_ctx;

	while (i < max) {
		head = ring->head;

		if (head == ring->tail) {
			/* There are no more completions */
			break;
		} else {
			/* There is another completion to reap */
			events[i] = ring->events[head];
			// read barrier
			__asm__ __volatile__("lfence":::"memory");
			ring->head = (head + 1) % ring->nr;
			i++;
		}
	}

	return i;
}

// This is intended to be accesses by different tasks on the same thread. Hence,
// no synchronization is performed.
struct AioState {
    enum class State {UNINITIALIZED, READY, DRAINING, DONE};
    size_t              aio_pending_;
    State               aio_state_;
    aio_context_t       aio_ctx_;
    static const size_t aio_maxio_ = 1024;

    static const size_t aio_ioq_size_ = 4;
    struct iocb *aio_ioq_[aio_ioq_size_];
    size_t       aio_ioq_nr_;

    AioState():
        aio_pending_(0),
        aio_state_(State::UNINITIALIZED),
        aio_ctx_(0),
        aio_ioq_nr_(0) {}

    bool is_initialized(void) { return aio_state_ == State::UNINITIALIZED; }

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
        //printf("%s: Entry (%p)\n", __FUNCTION__, &aio_state_);
        if (aio_state_ != State::UNINITIALIZED) {
            fprintf(stderr, "%s: Invalid state: %s\n", __PRETTY_FUNCTION__, state_str(aio_state_));
            abort();
        }
        if (io_setup(aio_maxio_, &aio_ctx_) < 0) {
            perror("io_queue_init failed\n");
            exit(1);
        }
        aio_state_ = State::READY;
        //printf("%s: (%p) Exit state:%s\n", __FUNCTION__, &aio_state_, state_str(aio_state_));
    }

    void flush_ioq_(void) {

        if (aio_ioq_nr_ == 0)
            return;

        int ret = io_submit(aio_ctx_, aio_ioq_nr_,  aio_ioq_);
        if (ret < 0) {
            perror("io_submit");
            abort();
        } else if ((unsigned)ret != aio_ioq_nr_) {
            perror("io_submit (we do not handle partial success)");
            abort();
        }

        aio_ioq_nr_ = 0;
    }

    // directly submit
    int submit(size_t nr, struct iocb **iocbpp) {
        aio_pending_ += nr;
        flush_ioq_();
        return io_submit(aio_ctx_, nr,  iocbpp);
    }

    // -1 on error. Sets errno.
    int submit(struct iocb *iocb) {
        // TODO: return an error here if state is DRAINING
        if (aio_state_ != State::READY) {
            fprintf(stderr, "%s: invalid state: %s\n", __PRETTY_FUNCTION__, state_str(aio_state_));
            abort();
        }

        aio_ioq_[aio_ioq_nr_++] = iocb;
        aio_pending_++;
        //printf("%s: (0x%lx) aio_pending_: %zd\n", __FUNCTION__, pthread_self(), aio_pending_);

        if (aio_ioq_nr_ == aio_ioq_size_)
            flush_ioq_();

        return 0;
    }

    int getevents(long min_nr, long nr, struct io_event *events, struct timespec *ts) {
        if (aio_state_ != State::READY && aio_state_ != State::DRAINING) {
            fprintf(stderr, "%s: invalid state: (%p) %s\n", __PRETTY_FUNCTION__, &aio_state_, state_str(aio_state_));
            abort();
        }

        //printf("%s: (0x%lx) aio_pending_: %zd\n", __FUNCTION__, pthread_self(), aio_pending_);
        if (aio_pending_ == 0)
            return 0;

        if (aio_ioq_nr_  > 0)
            flush_ioq_();

        int nevents;
        if (aio_user_check(aio_ctx_))
            nevents = aio_user_getevents(aio_ctx_, nr, events);
        else
            nevents = io_getevents(aio_ctx_, min_nr, nr, events, ts);
        //printf("pending: %zd nevents: %d\n", aio_pending_, nevents);
        if (nevents < 0) {
            perror("io_getevents");
            exit(1);
        }

        assert(aio_pending_ >= (unsigned)nevents);
        aio_pending_ -= nevents;
        if (aio_pending_ == 0 && aio_state_ == State::DRAINING) {
            done_();
        }

        return nevents;
    }

    void stop(void) {
        if (aio_state_ != State::READY) {
            fprintf(stderr, "%s: invalid state\n", __PRETTY_FUNCTION__);
            abort();
        }

        aio_state_ = State::DRAINING;
        //printf("aio_pending: %zd\n", aio_pending_);
        if (aio_pending_ == 0)
            done_();
    }

    void done_(void) {
        assert(aio_state_ == State::DRAINING);
        assert(aio_pending_ == 0);
        aio_state_ = State::DONE;
        io_destroy(aio_ctx_); // Maybe move this to dtor?
    }

    bool is_done(void) {
        return aio_state_ == State::DONE;
    }

    ~AioState() {
        if (aio_state_ != State::DONE && aio_state_ != State::UNINITIALIZED) {
            fprintf(stderr, "%s: **** Did not finalize correctly\n", __PRETTY_FUNCTION__);
            io_destroy(aio_ctx_); // do what we can...
        }
    }
};

#endif /* AIO_HH_ */
