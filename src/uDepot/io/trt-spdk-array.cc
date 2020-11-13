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
#include "uDepot/io/trt-spdk-array.hh"

#include <atomic>
#include <array>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <mutex>

#include "util/types.h"
#include "trt_backends/trt_spdk.hh"

namespace udepot {

static struct GlobalState {
	SpdkGlobalState        spdk_state_;
	// namespaces_set_ is used to distinguish between empty @namespaces_ because
	// no user string was provided, which means that we need to use all devices
	// and an empty @namespaces, because open() has not been yet called.
	bool                     namespaces_set_;
	std::vector<std::string> namespaces_;

	GlobalState() : spdk_state_(), namespaces_set_(false), namespaces_() {}

	void init() {
		spdk_state_.init();
	}

	int set_namespaces(std::string str) {
		assert(!namespaces_set_);
		namespaces_set_ = true;
		if (str.empty())
			return 0;

		std::istringstream ss(str);
		std::string ns;
		while (std::getline(ss, ns, ',')) {
			namespaces_.push_back(ns);
		}

		std::sort(namespaces_.begin(), namespaces_.end());
		auto it = std::unique(namespaces_.begin(), namespaces_.end());
		if (it != namespaces_.end()) {
			UDEPOT_ERR("duplicate namespace provided, aborting.");
			namespaces_.erase(namespaces_.begin(), namespaces_.end());
			namespaces_set_ = false;
			return EINVAL;
		}
		return 0;
	}

	std::vector<std::string> get_namespaces(void) {
		assert(namespaces_set_);
		std::vector<std::string> spdk_namespaces = trt::SPDK::getNamespaceNames();
		// return all spdk namespaces if user did not provide any
		if (namespaces_.empty()) {
			std::sort(spdk_namespaces.begin(), spdk_namespaces.end());
			return spdk_namespaces;
		} else {
			return namespaces_;
		}
	}
} GlobalState__;

static thread_local class ThreadState {
public:
	ThreadState() : spdk_qps_nr_m (0) {
		std::fill(spdk_qps_m.begin(), spdk_qps_m.end(), nullptr);
	};

	bool is_initialized() { return spdk_qps_nr_m  > 0; }

	SpdkQpair *offset_to_qpair(off_t start, bool read) {
		const off_t qpair_idx = (start / blk_m) % spdk_qps_nr_m;
		if (read)
			read_io_m[qpair_idx]++;
		else
			write_io_m[qpair_idx]++;
		UDEPOT_DBG("qps=%u qp_idx=%ld", spdk_qps_nr_m.load(), qpair_idx);
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

	int ts_init(void) {
		assert(!is_initialized());
		std::vector<std::string> namespaces = GlobalState__.get_namespaces();
		for (auto &ns: namespaces) {
			assert(nullptr == spdk_qps_m[spdk_qps_nr_m]);
			spdk_qps_m[spdk_qps_nr_m] = trt::SPDK::getQpair_by_prefix(ns);
			if (nullptr == spdk_qps_m[spdk_qps_nr_m]) {
				trt_err("Unable to get an SPDK queue pair for namespace \"%s\"\n", ns.c_str());
				for (int i = spdk_qps_nr_m - 1; 0 <= i; --i)
					spdk_qps_m[spdk_qps_nr_m] = nullptr;
				spdk_qps_nr_m = 0;
				errno = EIO;
				return -1;
			}

			trt_msg("Got spdk QPair %p for namespace \"%s\"\n", spdk_qps_m[spdk_qps_nr_m].get(), ns.c_str());
			spdk_qps_nr_m++;
			if (spdk_qps_m.size() <= spdk_qps_nr_m)
				break;
		}
		trt_msg("Initialized %u nvme namespaces %luGiB\n", spdk_qps_nr_m.load(), get_size() >> 30);
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

	~ThreadState() { close(); }
private:
	friend TrtSpdkArrayIO;

	#define _UDEPOT_TRT_SPDK_ARRAY_IO_MAX	256
	std::array<std::shared_ptr<SpdkQpair>, _UDEPOT_TRT_SPDK_ARRAY_IO_MAX> spdk_qps_m;
	std::atomic<u32> spdk_qps_nr_m;
	ThreadState(ThreadState const &) = delete;
	void operator=(ThreadState const &) = delete;
protected:
	static constexpr u32 blk_m = 8192U;

	std::array<std::atomic<u32>, _UDEPOT_TRT_SPDK_ARRAY_IO_MAX> read_io_m;
	std::array<std::atomic<u32>, _UDEPOT_TRT_SPDK_ARRAY_IO_MAX> write_io_m;

} ThreadState__;

static thread_local bool        SpdkThreadInitialized__ = false;

TrtSpdkArrayIO::TrtSpdkArrayIO():
	pread_iofn_m(&trt::SPDK::pread),
	pwrite_iofn_m(&trt::SPDK::pwrite),
	preadv_iofn_m(&trt::SPDK::preadv),
	pwritev_iofn_m(&trt::SPDK::pwritev),
	pread_native_iofn_m(&TrtSpdkArrayIO::pread_native_fn),
	pwrite_native_iofn_m(&TrtSpdkArrayIO::pwrite_native_fn),
	preadv_native_iofn_m(&TrtSpdkArrayIO::preadv_native_fn),
	pwritev_native_iofn_m(&TrtSpdkArrayIO::pwritev_native_fn)
{
}
void TrtSpdkArrayIO::global_init(void) {
	GlobalState__.init();
}

void TrtSpdkArrayIO::thread_init(void) {
	assert(SpdkThreadInitialized__ == false);
	trt_dmsg("Initializing SPDK\n");
	trt::SPDK::init(GlobalState__.spdk_state_);
	trt_dmsg("Spawning SPDK poller\n");
	trt::T::spawn(trt::SPDK::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);
	trt_dmsg("init done\n");
	SpdkThreadInitialized__ = true;
}

void TrtSpdkArrayIO::thread_exit(void)
{
	trt::SPDK::stop();
}

static void initThreadState_if_needed(void)
{
	assert(SpdkThreadInitialized__);
	if (ThreadState__.is_initialized())
		return;
	int err = ThreadState__.ts_init();
	if (err) {
		UDEPOT_ERR("setThreadQP failed\n");
		abort();
	}
}

static inline size_t
iovec_len(const struct iovec *iov, unsigned iovcnt)
{
    size_t ret = 0;
    for (unsigned i=0; i < iovcnt; i++) {
        ret += iov[i].iov_len;
    }
    return ret;
}

ssize_t TrtSpdkArrayIO::pio(void *buff, size_t len, off_t off, const PioFn &f)
{
	initThreadState_if_needed();
	const u32 blk_m = ThreadState::blk_m;
	ssize_t rc = 0, tot = 0;
	UDEPOT_DBG("pio addr=%p len=%lu off=%lu.", buff, len, off);
	for (; 0 < len; tot += rc) {
		const off_t offset = off % blk_m;
		const size_t l = std::min(len, (size_t) blk_m - offset);
		auto dev_qpr = ThreadState__.offset_to_qpair(off, true);
		auto dev_off = ThreadState__.dev_offset(off);
		UDEPOT_DBG("dev_qpr=%p buf=%p, len=%ld off=%lu.", dev_qpr, buff, l, dev_off);
		rc = f(dev_qpr, buff, l, dev_off);
		if ((ssize_t) l != rc)
			return rc < 0 ? rc : tot;
		buff = (void *) ((char *) buff + l);
		len -= l;
		off += l;
	}
	return tot;
}

ssize_t TrtSpdkArrayIO::piov(const struct iovec *const iov_in, int iovcnt, off_t off, const PiovFn &f)
{
	initThreadState_if_needed();
	const u32 blk_m = ThreadState::blk_m;
	iovec *iov = (iovec *) iov_in;
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
		auto dev_qpr = ThreadState__.offset_to_qpair(off, true);
		auto dev_off = ThreadState__.dev_offset(off);
		assert(advance_len == iovec_len(iov, p - iov) && advance_len <= blk_m);
		UDEPOT_DBG("dev_qpr=%p iov=%p, iovlen=%ld off=%lu.", dev_qpr, iov, p - iov, dev_off);
		rc = f(dev_qpr, iov, p - iov, dev_off);
		if ((ssize_t) advance_len != rc)
			return rc < 0 ? rc : tot;
		*(--p) = bak; // restore last pointer
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
		off += advance_len;
	}
	return tot;
}

ssize_t TrtSpdkArrayIO::pread(void *buff, size_t len, off_t off)
{
	return pio(buff, len, off, pread_iofn_m);
}

ssize_t TrtSpdkArrayIO::preadv(const struct iovec *const iov, int iovcnt, off_t off)
{
	return piov(iov, iovcnt, off, preadv_iofn_m);
}

ssize_t TrtSpdkArrayIO::pwrite(const void *buff, size_t len, off_t off)
{
	return pio((void *) buff, len, off, pwrite_iofn_m);
}

ssize_t TrtSpdkArrayIO::pwritev(const struct iovec *iov, int iovcnt, off_t off)
{
	return piov(iov, iovcnt, off, pwritev_iofn_m);
}

ssize_t
TrtSpdkArrayIO::pread_native_fn(SpdkQpair *qp, void *buff_, size_t len, off_t off)
{
    const u64 b = qp->get_sector_size();
    const u64 lba_start = off / b;
    const u64 lba_end = udiv_round_up(off + len, b);
    const u64 nlbas = lba_end - lba_start;
    SpdkPtr buff = SpdkPtr(buff_, len);
    const int rc = trt::SPDK::read(qp, buff, lba_start, nlbas);
    if (static_cast<u64>(rc) != nlbas) {
	    UDEPOT_ERR("spdk read ret=%d.", rc);
	    errno = EIO;
	    return -1;
    }
    return len;
}

ssize_t
TrtSpdkArrayIO::pwrite_native_fn(SpdkQpair *qp, void *buff_, size_t len, off_t off)
{
    const u64 b = qp->get_sector_size();
    const u64 lba_start = off / b;
    const u64 lba_end = udiv_round_up(off + len, b);
    const u64 nlbas = lba_end - lba_start;
    SpdkPtr buff = SpdkPtr(buff_, len);
    const int rc = trt::SPDK::write(qp, buff, lba_start, nlbas);
    if (static_cast<u64>(rc) != nlbas) {
	    UDEPOT_ERR("spdk read ret=%d.", rc);
	    errno = EIO;
	    return -1;
    }
    return len;
}

ssize_t
TrtSpdkArrayIO::preadv_native_fn(SpdkQpair *qp, const struct iovec *iov, int iovcnt, off_t off)
{
	ssize_t tot = 0;
	for (; iovcnt; ++iov, --iovcnt) {
		const ssize_t rc = pread_native_fn(qp, iov->iov_base, iov->iov_len, off + tot);
		if (static_cast<s64>(iov->iov_len) != rc)
			return rc < 0 ? rc : (tot + rc);
		tot += rc;
	}
	return tot;
}

ssize_t
TrtSpdkArrayIO::pwritev_native_fn(SpdkQpair *qp, const struct iovec *iov, int iovcnt, off_t off)
{
	ssize_t tot = 0;
	for (; iovcnt; ++iov, --iovcnt) {
		const ssize_t rc = pwrite_native_fn(qp, iov->iov_base, iov->iov_len, off + tot);
		if (static_cast<s64>(iov->iov_len) != rc)
			return rc < 0 ? rc : (tot + rc);
		tot += rc;
	}
	return tot;
}

ssize_t TrtSpdkArrayIO::pread_native(Ptr buff, size_t len, off_t off)
{
	return pio(buff.ptr_m, len, off, pread_native_iofn_m);
}

ssize_t TrtSpdkArrayIO::pwrite_native(Ptr buff, size_t len, off_t off)
{
	return pio(buff.ptr_m, len, off, pwrite_native_iofn_m);
}

ssize_t TrtSpdkArrayIO::preadv_native(IoVec<Ptr>  iov, off_t off)
{
	if (likely(1 == iov.iov_cnt_m))
		return pio(iov.iov_m->iov_base, iov.iov_m->iov_len, off, pread_native_iofn_m);
	else
		return piov(iov.iov_m, iov.iov_cnt_m, off, preadv_native_iofn_m);
}

ssize_t TrtSpdkArrayIO::pwritev_native(IoVec<Ptr> iov, off_t off)
{
	if (likely(1 == iov.iov_cnt_m))
		return pio(iov.iov_m->iov_base, iov.iov_m->iov_len, off, pwrite_native_iofn_m);
	else
		return piov(iov.iov_m, iov.iov_cnt_m, off, pwritev_native_iofn_m);
}


std::mutex mmap_mutex_g;
void *
TrtSpdkArrayIO::mmap(void *const addr, size_t len, const int prot, const int flags, const off_t off)
{
	std::cerr << "---------------------------------------------------------"
		  << std::endl
		  << "WARNING: SPDK Backend does not properly support mmap yet."
		  << std::endl
		  << "Doing anonymous mmap() with data read into memory"
		  << std::endl
		  << "---------------------------------------------------------"
		  << std::endl;
	void *const ptr = ::mmap(addr, len, prot | PROT_WRITE, flags | MAP_ANONYMOUS | MAP_POPULATE | MAP_LOCKED, -1, off);
	if (MAP_FAILED == ptr)
		return ptr;

	const ssize_t rc = pread(ptr, len, off);
	if ((ssize_t) len != rc) {
		const int rc2 __attribute__((unused)) = ::munmap(ptr, len);
		assert(0 == rc2);
		errno = EIO;
		return MAP_FAILED;
	}
	UDEPOT_MSG("mmap pread addr=%p len=%lu rc=%ld off=%lu.",
	 	ptr, len, rc, off);

	mmap_mutex_g.lock();
	mmap_off_m.emplace(ptr, (mregion)  { off, len});
	mmap_mutex_g.unlock();
	return ptr;
}

int
TrtSpdkArrayIO::msync(void *addr, size_t len, int flags)
{
	const uintptr_t loc = (uintptr_t)addr;
	std::map<void *, mregion>::iterator it = mmap_off_m.begin();
	mmap_mutex_g.lock();
	for (; it != mmap_off_m.end(); ++it) {
		const uintptr_t start = (uintptr_t) it->first;
		const uintptr_t end = start + it->second.len;
		if (start <= loc && loc < end) {
			break;
		}
	}
	mmap_mutex_g.unlock();
	if (it == mmap_off_m.end()) {
		errno = EINVAL;
		return -1;
	}

	const ssize_t rc = pwrite(addr, len, it->second.off);
	UDEPOT_DBG("msync pwrite addr=%p len=%lu rc=%ld off=%lu.",
	 	addr, len, rc, it->second.off);
	return (ssize_t) len == rc ? 0 : ({errno = EIO; -1;});
}

int
TrtSpdkArrayIO::munmap(void *addr, size_t len)
{
	const uintptr_t loc = (uintptr_t)addr;
	std::map<void *, mregion>::iterator it = mmap_off_m.begin();
	mmap_mutex_g.lock();
	for (; it != mmap_off_m.end(); ++it) {
		const uintptr_t start = (uintptr_t) it->first;
		const uintptr_t end = start + it->second.len;
		if (start <= loc && loc < end) {
			break;
		}
	}
	if (it == mmap_off_m.end()) {
		mmap_mutex_g.unlock();
		errno = EINVAL;
		return -1;
	}
	mmap_off_m.erase(it);
	mmap_mutex_g.unlock();
	return ::munmap(addr, len);
}

u64 TrtSpdkArrayIO::get_size()
{
	initThreadState_if_needed();
	return ThreadState__.get_size();
}

int TrtSpdkArrayIO::open(const char *pathname, int flags, mode_t mode)
{
	int err = 0;
	// initialize this threads state to get an early error if something goes
	// wrong
	assert(SpdkThreadInitialized__);
	assert(!ThreadState__.is_initialized());
	err = GlobalState__.set_namespaces(std::string(pathname));
	if (0 != err) {
		UDEPOT_ERR("Failed to set_namespaces err=%s.", strerror(err));
		return err;
	}
	return ThreadState__.ts_init();
}

int TrtSpdkArrayIO::close()
{
	return ThreadState__.close();
}
} // end namespace udepot

// instantiate template
template class RteAlloc<512>;

