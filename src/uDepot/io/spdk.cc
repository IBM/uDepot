/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
 *           Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

#include "uDepot/io.hh"
#include "uDepot/io/spdk.hh"

#include <atomic>
#include <array>
#include <algorithm>
#include <iostream>
#include <set>
#include <chrono>
#include <thread>
#include <mutex>
#include <set>
#include <sched.h>
#include <linux/futex.h>

#include "util/types.h"
#include "util/debug.h"
#include "trt_backends/trt_spdk.hh"
#include "trt_util/spdk.hh"

// SPDK IO backend without TRT
//
// XXX: Not tested with latest SPDK/DPDK versions.
//      Not sure if we want to keep this around.

// Wrapper for the futex system call.
// We use this to put threads to sleep when a resize operation occurs.
static inline long
sys_futex(void *addr1, int op, int v1, struct timespec *t, void *addr2, int v3)
{
	return syscall(SYS_futex, addr1, op, v1, t, addr2, v3);
}


#include <rte_mempool.h>

namespace udepot {
class SpdkIOMgr;
class SpdkIOThreadState;
static void * spdk_io_scheduler(void *arg);
class SpdkIOMgrThreadState {
	friend SpdkIO;
	friend SpdkIOMgr;
	friend SpdkIOThreadState;
public:
	SpdkIOMgrThreadState() : spdk_qps_nr_m (0) {
		std::fill(spdk_qps_m.begin(), spdk_qps_m.end(), nullptr);
	};

	bool is_initialized() { return spdk_qps_nr_m  > 0; }

	SpdkQpair *offset_to_qpair(off_t start, bool read) {
		const off_t chunk_in_stripe_idx = (start / blk_m) % spdk_qps_nr_m;
		const off_t stripe_idx = start / ((u64) blk_m * spdk_qps_nr_m);
		const off_t qpair_idx = (chunk_in_stripe_idx + stripe_idx) % spdk_qps_nr_m;
		if (read)
			read_io_m[qpair_idx]++;
		else
			write_io_m[qpair_idx]++;
		return spdk_qps_m[qpair_idx].get();
	}

	off_t dev_offset(off_t start) {
		assert(0 < spdk_qps_nr_m);
		const off_t offset = start % blk_m;
		const off_t page_stripe_idx  = start / ((u64) blk_m * spdk_qps_nr_m);
		return page_stripe_idx * blk_m + offset;
	}

	u64 get_size(void) {
		u64 min_size = (u64) -1;
		for (int i = spdk_qps_nr_m - 1; 0 <= i; --i)
			min_size = std::min(spdk_qps_m[i]->get_size(), min_size);
		return min_size * spdk_qps_nr_m;
	}

	int init(void) {
		assert(!is_initialized());
		std::vector<std::string> namespaces = trt::SPDK::getNamespaceNames();
		std::sort(namespaces.begin(), namespaces.end());
		for (auto &ns: namespaces) {
			assert(nullptr == spdk_qps_m[spdk_qps_nr_m]);
			spdk_qps_m[spdk_qps_nr_m] = trt::SPDK::getQpair(ns);
			if (nullptr == spdk_qps_m[spdk_qps_nr_m]) {
				trt_dmsg("Unable to get an SPDK queue pair for namespace \"%s\"\n", ns.c_str());
				for (int i = spdk_qps_nr_m - 1; 0 <= i; --i)
					spdk_qps_m[spdk_qps_nr_m] = nullptr;
				spdk_qps_nr_m = 0;
				errno = EIO;
				return -1;
			}

			trt_dmsg("Got spdk QPair %p namespace \"%s\"\n",
				spdk_qps_m[spdk_qps_nr_m].get(), ns.c_str());
			spdk_qps_nr_m++;
			if (spdk_qps_m.size() <= spdk_qps_nr_m)
				break;
		}
		trt_dmsg("Initialized %u nvme namespaces %luGiB\n",
			spdk_qps_nr_m.load(), get_size() >> 30);
		return 0;
	}

	int close(void) {
		if (!is_initialized()) {
			errno = EIO;
			return -1;
		}

		for (int i = spdk_qps_nr_m - 1; 0 <= i; --i) {
			trt_dmsg("nvme namespace %d Reads=%u Writes=%u\n", i, read_io_m[i].load(), write_io_m[i].load());
			spdk_qps_m[i] = nullptr;
		}
		spdk_qps_nr_m = 0;
		return 0;
	}

	~SpdkIOMgrThreadState() {
		close();
	}
private:
	#define _UDEPOT_TRT_SPDK_ARRAY_IO_MAX	256
	std::array<std::shared_ptr<SpdkQpair>, _UDEPOT_TRT_SPDK_ARRAY_IO_MAX> spdk_qps_m;
	std::atomic<u32> spdk_qps_nr_m;
	SpdkIOMgrThreadState(SpdkIOMgrThreadState const &) = delete;
	void operator=(SpdkIOMgrThreadState const &) = delete;
protected:
	static constexpr u32 blk_m = 8192U;

	std::array<std::atomic<u32>, _UDEPOT_TRT_SPDK_ARRAY_IO_MAX> read_io_m;
	std::array<std::atomic<u32>, _UDEPOT_TRT_SPDK_ARRAY_IO_MAX> write_io_m;
};

struct SpdkIOctx {
	std::atomic<int> err;
	std::atomic<u32> outstanding;
	pthread_cond_t  *cond;
	pthread_mutex_t *mtx;
	SpdkIOctx(): err(-1), outstanding(-1), cond(nullptr), mtx(nullptr) {}
	SpdkIOctx(pthread_cond_t *cond, pthread_mutex_t *mtx) :
		err (0), outstanding(1), cond(cond), mtx(mtx) {}
};

enum SpdkIODirection {
	SPDK_READ  = 0,
	SPDK_WRITE = 1,
	SPDK_LAST
};

class SpdkIOMgr {
#define	_SPDKIOMGR_MAX_QD	(1024U)
#define	_SPDKIOMGR_MAX_SCHED	(6U)
public:
	SpdkIOMgr() : id_gen_m(0), reg_nr_m(0) , exit_m(0), init_m(false), size_m(0), sched_nr_m(0) {
		SpdkIOSlot dummy;
		std::fill(io_queue_m.begin(), io_queue_m.end(), dummy);
		std::fill(ids_m.begin(), ids_m.end(), 0);
		pthread_mutex_init(&mtx_m, nullptr);
	}
	~SpdkIOMgr() {
		exit_m = 1;
		c_m.wait_for_all();
		pthread_mutex_destroy(&mtx_m);
	}

	void init(void) {
		pthread_mutex_lock(&mtx_m);
		if (init_m) {
			pthread_mutex_unlock(&mtx_m);
			return;
		}
		const unsigned lcores_nr = rte_lcore_count();
		if (lcores_nr == 0) {
			UDEPOT_ERR("Did you initialize RTE?");
			pthread_mutex_unlock(&mtx_m);
			abort();
		}
		unsigned lcore;
		sched_nr_m = std::min(_SPDKIOMGR_MAX_SCHED, lcores_nr);
		pthread_barrier_init(&barrier_m, NULL, sched_nr_m + 1);

		int i = -1;
		RTE_LCORE_FOREACH_SLAVE(lcore) {
			if (_SPDKIOMGR_MAX_SCHED <= (u32) ++i)
				break;
			UDEPOT_MSG("spawning scheduler on lcore=%u i=%d", lcore, i);
			c_m.spawn_scheduler(&spdk_io_scheduler, (void *) (u64) i, trt::TaskType::TASK, lcore);
		}
		u32 idxs[sched_nr_m];
		memset(idxs, 0, sizeof(*idxs) * sched_nr_m);
		u32 ids_per_sched = _SPDKIOMGR_MAX_QD / sched_nr_m;
		u32 ids_per_sched_rem = _SPDKIOMGR_MAX_QD % sched_nr_m;
		for (u32 j = 0; j < sched_nr_m; j++)
			idxs[j] = j * ids_per_sched;
		for (u32 j = 0; j < ids_per_sched; ++j)
			for (u32 k = 0; k < sched_nr_m; ++k)
				id_gen_m.push_back(idxs[k]++);
		for (u32 j = 0; j < ids_per_sched_rem; ++j)
			id_gen_m.push_back(idxs[sched_nr_m - 1]++);
		assert(id_gen_m.size() == _SPDKIOMGR_MAX_QD);
		pthread_barrier_wait(&barrier_m);
		init_m = true;
		pthread_mutex_unlock(&mtx_m);
	}

	int register_user(u64 &id) {
		pthread_mutex_lock(&mtx_m);
		if (0 == id_gen_m.size()) {
			pthread_mutex_unlock(&mtx_m);
			return EINVAL;
		}
		id = id_gen_m.back();
		id_gen_m.pop_back();
		pthread_mutex_unlock(&mtx_m);
		assert(id < _SPDKIOMGR_MAX_QD);
		reg_nr_m.fetch_add(1);
		return 0;
	}

	void deregister_user(const u64 id) {
		pthread_mutex_lock(&mtx_m);
		id_gen_m.push_back(id);
		pthread_mutex_unlock(&mtx_m);
		assert(id < _SPDKIOMGR_MAX_QD);
		const u64 old_val __attribute__((unused)) = reg_nr_m.fetch_sub(1);
		assert(0 < old_val);
	}

	void submit_io(const SpdkIODirection dir, const u64 id, const iovec *const iov,
		const int iovcnt, const off_t off, SpdkIOctx *const ctx) {
		if (_SPDKIOMGR_MAX_QD <= id) {
			errno = EINVAL;
			ctx->err = -1;
			const u32 old_val __attribute__((unused)) = ctx->outstanding.fetch_sub(1);
			assert(1 == old_val);
			sys_futex(&ctx->outstanding, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
			// pthread_cond_signal(ctx->cond);
			return;
		}
		SpdkIOSlot &slot = io_queue_m[id];
		slot.iov = (iovec *) iov;
		slot.iovcnt = iovcnt;
		slot.off = off;
		slot.ctx = ctx;
		slot.dir = dir;
		mem_barrier();
		slot.id.fetch_add(1);
	}

	u64 get_size(void) const {
		return size_m;
	}
	struct SpdkIOSlot {
		struct iovec    *iov;
		int              iovcnt;
		off_t            off;
		SpdkIOctx       *ctx;
		SpdkIODirection  dir;
		std::atomic<u64> id;
		SpdkIOSlot() : iov(nullptr), iovcnt(-1), off(-1),
			       ctx(nullptr), dir(SPDK_LAST), id(0) {}
		SpdkIOSlot(const SpdkIODirection dir, const u64 id, const iovec *const iov,
			const int iovcnt, const off_t off, SpdkIOctx *const ctx)
			: iov((iovec *) iov), iovcnt(iovcnt), off(off),
			  ctx(ctx), dir(dir), id(id)
			{ }
		#if	0
		SpdkIOSlot(const SpdkIOSlot &slot) {
			iov = slot.iov;
			iovcnt = slot.iovcnt;
			off = slot.off;
			ctx = slot.ctx;
			dir = slot.dir;
			id.store(slot.id);
		}
		#endif
		void operator=(const SpdkIOSlot &slot) {
			iov = slot.iov;
			iovcnt = slot.iovcnt;
			off = slot.off;
			ctx = slot.ctx;
			dir = slot.dir;
			id.store(slot.id);
		}
		~SpdkIOSlot() {}
	} __attribute__((aligned(128)));
	std::array<SpdkIOSlot, _SPDKIOMGR_MAX_QD> io_queue_m;
	std::array<u64, _SPDKIOMGR_MAX_QD> ids_m;
	std::vector<u64> id_gen_m;
	std::atomic<u64> reg_nr_m;
	std::atomic<s8> exit_m;
	std::atomic<bool> init_m;
	u64 size_m;
	trt::RteController c_m;
	pthread_barrier_t barrier_m;
	pthread_mutex_t mtx_m;
	u32 sched_nr_m;

	ssize_t preadv(const struct iovec *iov, int iovcnt, off_t off);
	ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t off);
private:
	SpdkIOMgr(const SpdkIOMgr &)            = delete;
	SpdkIOMgr& operator=(const SpdkIOMgr &) = delete;
};

static inline size_t
iovec_len(const struct iovec *iov, unsigned iovcnt)
{
	size_t ret = 0;
	for (unsigned i=0; i < iovcnt; i++) {
		ret += iov[i].iov_len;
	}
	return ret;
}
static inline std::tuple<u64, u64>
get_lba_range(u64 offset, u64 length, size_t block_size)
{
	size_t lba_start = offset / block_size;
	size_t lba_end   = (offset + length + block_size - 1) / block_size;
	return std::make_tuple(lba_start, lba_end);
}

class SpdkIOThreadState {
	friend SpdkIO;
public:
	SpdkIOThreadState() : initialized_m(false) {
		pthread_cond_init(&cond_m, nullptr);
		pthread_mutex_init(&mtx_m, nullptr);
	};

	int init(SpdkIOMgr *mgr) {
		mgr_m = mgr;
		int rc = mgr->register_user(id_m);
		if (0 == rc) {
			initialized_m = true;
			UDEPOT_DBG("registerd IO thread to IO mgr id=%lu.\n", id_m);
		}
		buf_m = std::move(SpdkAlloc::alloc_iobuff(SpdkIOMgrThreadState::blk_m));
		return rc;
	}

	bool is_initialized() { return initialized_m; }
	~SpdkIOThreadState() {
		if (mgr_m)
			mgr_m->deregister_user(id_m);
		pthread_cond_destroy(&cond_m);
		pthread_mutex_destroy(&mtx_m);
		if (nullptr != buf_m.ptr_m)
			SpdkAlloc::free_iobuff(std::move(buf_m));
	}

	u64 get_size(void) {
		return mgr_m->get_size();
	}

	ssize_t sync_io(SpdkIODirection dir, const iovec *const iov, const int iovcnt, const off_t off) {
		const size_t iov_len = iovec_len(iov, iovcnt);
		SpdkIOctx io_ctx (&cond_m, &mtx_m);
		assert(1 == io_ctx.outstanding);
		mgr_m->submit_io(dir, id_m, iov, iovcnt, off, &io_ctx);
		// int64_t delay_ns = 40000;
		// if (SPDK_WRITE == dir)
		// 	delay_ns = 20000;
		// struct timespec ts = {0, delay_ns};
		// nanosleep(&ts, NULL);
		// ts = (timespec) {0, 10000};
		// while (0 != io_ctx.outstanding)
		// 	nanosleep(&ts, NULL);
		while (0 != io_ctx.outstanding) {
			int rc = sys_futex(&io_ctx.outstanding, FUTEX_WAIT, 1, NULL, NULL, 0);
			if (0 != rc) {
				switch (errno) {
				case EAGAIN:
				case EINTR:
					break;
				case EACCES:
				case EFAULT:
				case EINVAL:
				case ENFILE:
				case ENOSYS:
				case ETIMEDOUT:
				default:
					UDEPOT_ERR("sys_futex returned %d errno=%d", rc, errno);
					abort();
					break;
				}
			}
		}
		// pthread_mutex_lock(&mtx_m);
		// const int __attribute__((unused)) rc = pthread_cond_wait(&cond_m, &mtx_m);
		// pthread_mutex_unlock(&mtx_m);
		// assert(0 == rc);
		return 0 == io_ctx.err ? static_cast<ssize_t>(iov_len) : ({errno = io_ctx.err; -1;});
	}
protected:
	SpdkPtr   buf_m;
private:
	SpdkIOMgr *mgr_m;
	u64 id_m;
	bool initialized_m;
	pthread_cond_t cond_m;
	pthread_mutex_t mtx_m;
};

static SpdkGlobalState                   SpdkGlobalState__;
static SpdkIOMgr                         SpdkIOMgr__;
static thread_local SpdkIOMgrThreadState SpdkIOMgrThreadState__;
static thread_local bool                 SpdkThreadInitialized__ = false;
static thread_local SpdkIOThreadState    uDepotIOThreadState__;

struct t_io_arg {
	union {
		struct {
			u16 mgr_id;
			u32 slot_id;
		};
		uintptr_t raw;
	};
};
static_assert(sizeof(void *) == sizeof(t_io_arg), "t_io_arg has to be same size as a ptr");

static void *
spdk_io_task(void *const arg_)
{
	t_io_arg targ;
	targ.raw = reinterpret_cast<uintptr_t>(arg_);
	const u32 id = targ.slot_id;
	SpdkIOMgr *const mgr = &SpdkIOMgr__;
	SpdkIOMgr::SpdkIOSlot &slot = mgr->io_queue_m[id];
	ssize_t ret = 0;
	const ssize_t expected = iovec_len(slot.iov, slot.iovcnt);
	// UDEPOT_ERR("starting IO task dir=%d slot id=%u.", slot.dir, id);
	if (SPDK_READ == slot.dir)
		ret = mgr->preadv(slot.iov, slot.iovcnt, slot.off);
	else
		ret = mgr->pwritev(slot.iov, slot.iovcnt, slot.off);
	// UDEPOT_ERR("finishing IO task dir=%d slot id=%u.", slot.dir, id);
	if (ret < 0)
		slot.ctx->err = -1;
	if (expected != ret)
		UDEPOT_ERR("expected=%luB ret=%luB.", expected, ret);
	const u64 old_val __attribute__((unused)) = slot.ctx->outstanding.fetch_sub(1);
	assert(0 < old_val);
	int rc = sys_futex(&slot.ctx->outstanding, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
	if (unlikely(-1 == rc))
		UDEPOT_ERR("futex wake returned rc=%d errno=%d.", rc, errno);
	// pthread_cond_signal(slot.ctx->cond);
	return nullptr;
}

static void *
spdk_io_scheduler(void *arg)
{
	SpdkIOMgr *const mgr = &SpdkIOMgr__;
	const u64 sched_id = (uintptr_t) arg;
	assert(SpdkThreadInitialized__ == false);
	UDEPOT_DBG("Initializing SPDK scheduler id=%lu", sched_id);

	trt::SPDK::init(SpdkGlobalState__);
	trt_dmsg("init done\n");
	SpdkIOMgrThreadState__.init();
	trt_dmsg("Spawning SPDK poller\n");
	trt::T::spawn(trt::SPDK::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);
	SpdkThreadInitialized__ = true;
	if (0 == sched_id)
		mgr->size_m = SpdkIOMgrThreadState__.get_size();
	pthread_barrier_wait(&mgr->barrier_m);
	SpdkState *const spdkstate = trt::get_tls_SpdkState__();
	const u32 sched_nr = mgr->sched_nr_m;
	const u32 ids_per_sched = _SPDKIOMGR_MAX_QD / sched_nr;
	const u32 id_start = sched_id * ids_per_sched;
	u32 id_end = id_start + ids_per_sched;
	if (sched_nr - 1 == sched_id) {
		const u32 ids_per_sched_rem = _SPDKIOMGR_MAX_QD % sched_nr;
		id_end += ids_per_sched_rem;
	}
	assert(id_end <= mgr->io_queue_m.size());
	while (!spdkstate->is_done()) {
		if (mgr->exit_m)
			trt::SPDK::stop();
		// try to issue
		trt::Task::List tl;
		for (u32 i = id_start; i < id_end; i++) {
			SpdkIOMgr::SpdkIOSlot &slot = mgr->io_queue_m[i];
			const u64 id = slot.id.load(std::memory_order_relaxed);
			if (id == mgr->ids_m[i])
				continue;
			UDEPOT_DBG("adding task for sched=%lu slot=%u id=%lu old-id=%lu.", sched_id, i, id, mgr->ids_m[i]);
			assert(id == mgr->ids_m[i] + 1);
			t_io_arg arg;
			arg.slot_id = i;
			trt::Task *const t = trt::T::alloc_task(spdk_io_task, (void *) (arg.raw), nullptr, true);
			tl.push_back(*t);
			mgr->ids_m[i] += 1;
		}
		if (!tl.empty())
			trt::T::spawn_many(tl);
		trt::T::yield();
	}
	trt::T::set_exit_all(); // notify trt schedulers that we are done
	return nullptr;
}

void SpdkIO::global_init(void)
{
	SpdkGlobalState__.init();
	SpdkIOMgr__.init();
}

void SpdkIO::thread_init(void)
{
}

void SpdkIO::thread_exit(void)
{
}

static void initThreadState_if_needed(void)
{
	if (uDepotIOThreadState__.is_initialized())
		return;
	int err = uDepotIOThreadState__.init(&SpdkIOMgr__);
	if (err) {
		UDEPOT_ERR("setThreadQP failed.");
		abort();
	}
}

ssize_t SpdkIOMgr::preadv(const struct iovec *const iov, int iovcnt, off_t off)
{
	const u32 __attribute__((unused)) blk_len = SpdkIOMgrThreadState::blk_m;
	assert(1 == iovcnt && iov->iov_len <= blk_len);
	auto dev_qpr = SpdkIOMgrThreadState__.offset_to_qpair(off, true);
	auto dev_off = SpdkIOMgrThreadState__.dev_offset(off);
	SpdkPtr buff = SpdkPtr(iov->iov_base, iov->iov_len);
	ssize_t rc = trt::SPDK::read(dev_qpr, buff, dev_off / 512UL, (u32) (iov->iov_len / 512U));
	if (rc < 0)
		return rc;
	return rc * 512LL;
}

ssize_t SpdkIOMgr::pwritev(const struct iovec *iov, int iovcnt, off_t off)
{
	const u32 __attribute__((unused)) blk_len = SpdkIOMgrThreadState::blk_m;
	assert(1 == iovcnt && iov->iov_len <= blk_len);
	auto dev_qpr = SpdkIOMgrThreadState__.offset_to_qpair(off, false);
	auto dev_off = SpdkIOMgrThreadState__.dev_offset(off);
	SpdkPtr buff = SpdkPtr(iov->iov_base, iov->iov_len);
	ssize_t rc = trt::SPDK::write(dev_qpr, buff, dev_off / 512UL, (u32) (iov->iov_len / 512U));
	if (rc < 0)
		return rc;
	return rc * 512LL;
}


ssize_t SpdkIO::pwritev(const struct iovec *const iov, const int iovcnt, const off_t off, SpdkPtr &buf)
{
	u64 b                   = 512U;
	u64 iovlen              = iovec_len(iov, iovcnt);
	u64 lba_start, lba_end;
	std::tie(lba_start, lba_end) = get_lba_range(off, iovlen, b);
	u64 nlbas               = lba_end - lba_start;

	size_t copy_start = off - lba_start * b;
	if ((lba_start + nlbas)*b != off + iovlen) {
		UDEPOT_ERR("NYI: RMW off=%zd len=%lu block_size=%lu.", off, iovlen, b);
		errno = EINVAL;
		return -1;
	}

	// copy iovec to the buffer
	assert(copy_start + iovlen <= nlbas*b);
	assert(nlbas * b <= SpdkIOMgrThreadState::blk_m);
	buf.copy_from_iovec(copy_start, iov, iovcnt);

	iovec iov_io = (iovec) {.iov_base = buf.ptr_m, .iov_len = nlbas * b};
	ssize_t rc = uDepotIOThreadState__.sync_io(SPDK_WRITE, &iov_io, 1, off);
	if (rc != static_cast<ssize_t>(nlbas * b)) {
		UDEPOT_ERR("READ sync_io ret=%ld expected=%ld errno=%d.", rc, nlbas * b, errno);
	}

	return rc;
}

ssize_t SpdkIO::pwritev(const struct iovec *iov_in, int iovcnt, off_t off)
{
	initThreadState_if_needed();
	iovec *iov = (iovec *) iov_in;
	const u32 blk_m = SpdkIOMgrThreadState::blk_m;
	ssize_t rc = 0, tot = 0;
	for (; 0 < iovcnt; tot += rc) {
		const off_t offset = off % blk_m;
		size_t blk_len = blk_m - offset;
		size_t advance_len = 0;
		iovec *p;
		iovec bak;
		for (p = (iovec *) iov; 0 < blk_len && p < iov + iovcnt; ++p) {
			bak = *p;
			if (blk_len < p->iov_len)
				p->iov_len = blk_len;
			blk_len     -= p->iov_len;
			advance_len += p->iov_len;
		}
		assert(0 == blk_len || p == iov + iovcnt);
		assert(advance_len == iovec_len(iov, p - iov) && advance_len <= blk_m);
		rc = pwritev(iov, p - iov, off, uDepotIOThreadState__.buf_m);
		if ((ssize_t) advance_len != rc)
			return rc;
		*(--p) = bak; // restore last pointer
		off += advance_len;
		for (; 0 < advance_len;) {
			const size_t iovl = std::min(advance_len, iov->iov_len);
			iov->iov_len  -= iovl;
			iov->iov_base = (void *) ((char *) iov->iov_base + iovl);
			if (0 == iov->iov_len) {
				--iovcnt;
				++iov;
			}
			advance_len -= iovl;
		}
	}
	return tot;
}

ssize_t SpdkIO::preadv(const struct iovec *const iov, const int iovcnt, const off_t off, SpdkPtr &buf)
{
	u64 b                   = 512U;
	u64 iovlen              = iovec_len(iov, iovcnt);
	u64 lba_start, lba_end;
	std::tie(lba_start, lba_end) = get_lba_range(off, iovlen, b);
	u64 nlbas               = lba_end - lba_start;

	iovec iov_io = (iovec) {.iov_base = buf.ptr_m, .iov_len = nlbas * b};
	assert(nlbas * b <= SpdkIOMgrThreadState::blk_m);
	ssize_t rc = uDepotIOThreadState__.sync_io(SPDK_READ, &iov_io, 1, off);
	if (rc != static_cast<ssize_t>(nlbas * b)) {
		UDEPOT_ERR("READ sync_io ret=%ld expected=%ld errno=%d.", rc, nlbas * b, errno);
	}
	size_t copy_start = off - lba_start*b;
	// copy iovec to the buffer
	assert(copy_start + iovlen <= nlbas*b);
	buf.copy_to_iovec(copy_start, iov, iovcnt);

	return rc;
}

ssize_t SpdkIO::preadv(const struct iovec *const iov_in, int iovcnt, off_t off)
{
	initThreadState_if_needed();
	const u32 blk_m = SpdkIOMgrThreadState::blk_m;
	iovec *iov = (iovec *) iov_in;
	ssize_t rc = 0, tot = 0;
	for (; 0 < iovcnt; tot += rc) {
		const off_t offset = off % blk_m;
		size_t blk_len = blk_m - offset;
		size_t advance_len = 0;
		iovec *p;
		iovec bak;
		for (p = iov; 0 < blk_len && p < iov + iovcnt; ++p) {
			bak = *p;
			if (blk_len < p->iov_len)
				p->iov_len = blk_len;
			blk_len     -= p->iov_len;
			advance_len += p->iov_len;
		}
		assert(0 == blk_len || p == iov + iovcnt);
		assert(advance_len == iovec_len(iov, p - iov) && advance_len <= blk_m);
		rc = preadv(iov, p - iov, off, uDepotIOThreadState__.buf_m);
		if ((ssize_t) advance_len != rc)
			return rc;
		*(--p) = bak; // restore last pointer
		off += advance_len;
		for (; 0 < advance_len;) {
			const size_t iovl = std::min(advance_len, iov->iov_len);
			iov->iov_len  -= iovl;
			iov->iov_base = (void *) ((char *) iov->iov_base + iovl);
			if (0 == iov->iov_len) {
				--iovcnt;
				++iov;
			}
			advance_len -= iovl;
		}
	}
	return tot;
}

ssize_t SpdkIO::pwrite(const void *buff, size_t len, off_t off)
{
	initThreadState_if_needed();
	struct iovec iov = (struct iovec) {.iov_base = (void *)buff, .iov_len = len};
	return pwritev(&iov, 1, off);
}

ssize_t SpdkIO::pread(void *buff, size_t len, off_t off)
{
	initThreadState_if_needed();
	struct iovec iov = (struct iovec){.iov_base = buff, .iov_len = len};
	return preadv(&iov, 1, off);
}


static std::mutex mmap_mutex_g;
void *
SpdkIO::mmap(void *const addr, size_t len, const int prot, const int flags, const off_t off)
{
	std::cerr << "---------------------------------------------------------"
		  << std::endl
		  << "WARNING: SPDK Backend does not properly support mmap yet."
		  << std::endl
		  << "Doing anonymous mmap() with data read into memory"
		  << std::endl
		  << "---------------------------------------------------------"
		  << std::endl;
	void *const ptr = ::mmap(addr, len, prot, flags | MAP_ANONYMOUS, -1, off);
	if (MAP_FAILED == ptr)
		return ptr;
	const ssize_t rc = pread(ptr, len, off);
	if ((ssize_t) len != rc) {
		const int rc2 __attribute__((unused)) = ::munmap(ptr, len);
		assert(0 == rc2);
		errno = EIO;
		return MAP_FAILED;
	}
	UDEPOT_MSG("mmap pread addr=%p len=%lu rc=%ld off=%lu.", ptr, len, rc, off);

	mmap_mutex_g.lock();
	mmap_off_m[ptr] = off;
	mmap_mutex_g.unlock();
	return ptr;
}

int
SpdkIO::msync(void *addr, size_t len, int flags)
{
	mmap_mutex_g.lock();
	auto it = mmap_off_m.find(addr);
	mmap_mutex_g.unlock();
	if (it == mmap_off_m.end()) {
		errno = EINVAL;
		return -1;
	}

	const ssize_t rc = pwrite(addr, len, it->second);
	UDEPOT_MSG("msync pwrite addr=%p len=%lu rc=%ld off=%lu.",
		addr, len, rc, it->second);
	return (ssize_t) len == rc ? 0 : ({errno = EIO; -1;});
}

int
SpdkIO::munmap(void *addr, size_t len)
{
	mmap_mutex_g.lock();
	auto it = mmap_off_m.find(addr);
	if (it == mmap_off_m.end()) {
		mmap_mutex_g.unlock();
		errno = EINVAL;
		return -1;
	}
	mmap_off_m.erase(it);
	mmap_mutex_g.unlock();
	return ::munmap(addr, len);
}

u64 SpdkIO::get_size()
{
	initThreadState_if_needed();
	return uDepotIOThreadState__.get_size();
}

int SpdkIO::open(const char *pathname, int flags, mode_t mode)
{
	// initialize this threads state to get an early error if something goes
	// wrong
	initThreadState_if_needed();
	return uDepotIOThreadState__.init(&SpdkIOMgr__);
}

int SpdkIO::close()
{
	return 0; // return uDepotIOThreadState__.close();
}

} // end namespace udepot
