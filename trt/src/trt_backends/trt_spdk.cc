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


#include "trt/uapi/trt.hh"
#include "trt_backends/trt_spdk.hh"

#include <rte_lcore.h>

namespace trt {

thread_local SpdkState SpdkState__;

SpdkState *get_tls_SpdkState__() {
    return &SpdkState__;
}

void
SPDK::spdk_io_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
    LocalSingleAsyncObj *lsao = static_cast<LocalSingleAsyncObj *>(ctx);
    SpdkQpair *qp = (SpdkQpair *)lsao->lsao_user_data_;

    // see SpdkQpair::execute_completions()
    qp->npending_dec(1);
    T::io_npending_dec(1);

    // Return whether there was an error or not (not sure if we can get more
    // information out of SPDK).
    RetT val = spdk_nvme_cpl_is_error(cpl) ? -1 : 0;

    // callback is expected to be executed in the context of the poller_task,
    // which we assume it has called T::notify_init() before this function is
    // called, and will call T::notify_submit() after.
    bool ret = T::local_single_notify_add(lsao, val);
    if (!ret) { // flush notifications and retry
        T::local_single_notify_submit();
        T::local_single_notify_init();
        ret = T::local_single_notify_add(lsao, val);
        assert(ret);
    }
}


ssize_t
SPDK::read(SpdkQpair *qp, SpdkPtr &buff, uint64_t lba, uint32_t lba_cnt) {

    LocalSingleAsyncObj lsao;
    lsao.lsao_user_data_ = (uintptr_t)(qp);

    // trt_dmsg("Submitting RD on qp=%p lba=%lu cnt=%u\n", qp, lba, lba_cnt);
    const int rc = qp->submit_read(buff, lba, lba_cnt, spdk_io_cb, &lsao);
    if (0 != rc)
	    return -1;
    T::io_npending_inc(1);
    RetT err = T::local_single_wait(&lsao);
    return err ? -1 : lba_cnt;
}

ssize_t
SPDK::write(SpdkQpair *qp, SpdkPtr &buff, uint64_t lba, uint32_t lba_cnt) {
    LocalSingleAsyncObj lsao;
    lsao.lsao_user_data_ = (uintptr_t)(qp);
    #if !defined(NDEBUG)
    uint64_t sid0 = T::sid();
    #endif
    // trt_dmsg("Submitting WR on qp=%p lba=%lu cnt=%u\n", qp, lba, lba_cnt);
    const int rc = qp->submit_write(buff, lba, lba_cnt, spdk_io_cb, &lsao);
    if (0 != rc)
	    return -1;

    T::io_npending_inc(1);
    RetT err = T::local_single_wait(&lsao);
    #if !defined(NDEBUG)
    uint64_t sid1 = T::sid();
    if (sid0 != sid1) {
        fprintf(stderr, "Woke up on a different scheduler (old:%lu vs new:%lu)!  qp=%p ao.ao_user_data_=%p\n", sid0, sid1, qp, (void *)lsao.lsao_user_data_);
        abort();
    }
    #endif
    return err ? -1 : lba_cnt;
}

//
// Simple (i.e,. allocation + copy) preadv/pwrite implementations
//

static inline size_t
iovec_len(const struct iovec *iov, unsigned iovcnt)
{
    size_t ret = 0;
    for (unsigned i=0; i < iovcnt; i++) {
        ret += iov[i].iov_len;
    }
    return ret;
}

static inline std::tuple<uint64_t, uint64_t>
get_lba_range(uint64_t offset, uint64_t length, size_t block_size)
{
    size_t lba_start = offset / block_size;
    size_t lba_end   = (offset + length + block_size - 1) / block_size;
    return std::make_tuple(lba_start, lba_end);
}

// will allocate a new buffer for SPDK I/O
ssize_t SPDK::preadv(SpdkQpair *qp, const struct iovec *iov, size_t iovcnt, off_t off)
{
    uint64_t b                   = qp->get_sector_size();
    uint64_t iovlen              = iovec_len(iov, iovcnt);
    uint64_t lba_start, lba_end;
    std::tie(lba_start, lba_end) = get_lba_range(off, iovlen, b);
    uint64_t nlbas               = lba_end - lba_start;

    // allocate SPDK buffer
    SpdkPtr buff = std::move(qp->alloc_buffer(nlbas));
    if (!buff.ptr_m) {
        errno = ENOMEM;
        return -1;
    }

    uint64_t nlbas_wr = SPDK::read(qp, buff, lba_start, nlbas);
    if (nlbas_wr != nlbas) {
        // remove this once everything works
        fprintf(stderr, "WARNING: partial read %lu vs %lu LBAS", nlbas, nlbas_wr);
        qp->free_buffer(std::move(buff));
        errno = EIO;
        return -1;
    }

    // where does the first LBA go
    size_t copy_start = off - lba_start*b;
    // copy iovec to the buffer
    assert(copy_start + iovlen <= nlbas*b);
    buff.copy_to_iovec(copy_start, iov, iovcnt);

    qp->free_buffer(std::move(buff));
    return iovlen;
}

// will allocate a new buffer for SPDK I/O
ssize_t
SPDK::pwritev(SpdkQpair *qp, const struct iovec *iov, int iovcnt, off_t off) {
    uint64_t b                   = qp->get_sector_size();
    uint64_t iovlen              = iovec_len(iov, iovcnt);
    uint64_t lba_start, lba_end;
    std::tie(lba_start, lba_end) = get_lba_range(off, iovlen, b);
    uint64_t nlbas               = lba_end - lba_start;

    //trt_dmsg("%s: enter\n", __PRETTY_FUNCTION__);
    // allocate SPDK buffer
    SpdkPtr buff = std::move(qp->alloc_buffer(nlbas));
    if (!buff.ptr_m) {
        errno = ENOMEM;
        return -1;
    }

    // where does the first LBA go in the buffer
    size_t copy_start = off - lba_start*b;
    // RMW the first block if needed
    if (copy_start != 0) {
        fprintf(stderr, "NYI: RMW off=%zd len=%lu block_size=%lu", off, iovlen, b);
        abort();
    }

    // RMW the last block if needed
    if ((lba_start + nlbas)*b != off + iovlen) {
        fprintf(stderr, "NYI: RMW off=%zd len=%lu block_size=%lu", off, iovlen, b);
        abort();
    }

    // copy iovec to the buffer
    assert(copy_start + iovlen <= nlbas*b);
    buff.copy_from_iovec(copy_start, iov, iovcnt);

    uint64_t nlbas_wr = SPDK::write(qp, buff, lba_start, nlbas);
    if (nlbas_wr != nlbas) {
        // remove this once everything works
        fprintf(stderr, "WARNING: partial write %lu vs %lu LBAS\n", nlbas, nlbas_wr);
    }

    // buff.free();
    qp->free_buffer(std::move(buff));
    return iovlen;
}


ssize_t SPDK::pread(SpdkQpair *qp, void *buff, size_t len, off_t off) {
    struct iovec iov = (struct iovec){.iov_base = buff, .iov_len = len};
    return SPDK::preadv(qp, &iov, 1, off);
}

ssize_t SPDK::pwrite(SpdkQpair *qp, const void *buff, size_t len, off_t off) {
    struct iovec iov = (struct iovec){.iov_base = (void *)buff, .iov_len = len};
    return SPDK::pwritev(qp, &iov, 1, off);
}

void *
SPDK::poller_task(void *unused) {

    //trt_dmsg("%s: enter\n", __PRETTY_FUNCTION__);
    while (!SpdkState__.is_done()) {
        T::local_single_notify_init();
        SpdkState__.execute_completions();
        T::local_single_notify_submit();
    }
    //trt_dmsg("%s: exit\n", __PRETTY_FUNCTION__);
    return nullptr;
}

/**
 * RteController
 */

static int
scheduler_rte_thread(void *arg)
{
    Scheduler *scheduler = static_cast<Scheduler *>(arg);
    Scheduler::thread_init(scheduler);
    scheduler->set_state_running();
    scheduler->pthread_barrier_wait();
    scheduler->start_();
	return 0;
}


void
RteController::spawn_scheduler(TaskFn main_fn, TaskFnArg main_arg,
                                TaskType main_type,
	                            unsigned lcore)
{
    Scheduler *s;
    cpu_set_t cpuset;

    // I don't think the cpuset is going to be used, but since we can provide a
    // reasonable argument, let's do it.
    CPU_ZERO(&cpuset);
    CPU_SET(lcore, &cpuset);

    if (0) {
        // TODO: schedulers_ is intended to only grow. If a scheduler
        // exits, it's state will be set to DONE, but its resources will
        // remain available At this point, we can search the schedulers_
        // vector for a DONE scheduler to reset.
    } else {
        schedulers_.emplace_back(*this, cpuset, main_fn, main_arg, main_type);
        s = &schedulers_.back();
    }
    assert(s != NULL);

	rte_eal_remote_launch(scheduler_rte_thread, s, lcore);
	s->pthread_barrier_wait();
}

unsigned
lcore_from_cpuset(cpu_set_t cpuset) {
	int cnt = CPU_COUNT(&cpuset);
	if (cnt != 1) {
		fprintf(stderr, "Unexpected cpuset\n");
		abort();
	}
	for (unsigned i=0; ; i++) {
		if (CPU_ISSET(i, &cpuset))
			return i;
	}
}

void
RteController::wait_for_all(void) {
    for (auto &s: schedulers_) {
        if (s.get_state() == Scheduler::State::RUNNING) {
			unsigned lcore = lcore_from_cpuset(s.get_cpuset());
            //printf("Waiting for scheduler on core: %u (%s)\n", lcore, &s);
			int err = rte_eal_wait_lcore(lcore);
			if (err) {
				fprintf(stderr, "rte_eal_wait_lcore: returned: %d\n", err);
			}
        }
    }
    ctl_done_ = true;
}

RteController::~RteController() {
    if (!ctl_done_) {
        set_exit_all();
        wait_for_all();
    }
}


} // end namespace trt
