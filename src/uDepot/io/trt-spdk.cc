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

#include "uDepot/io/trt-spdk.hh"
#include "trt_backends/trt_spdk.hh"
#include "util/debug.h"

namespace udepot {

static SpdkGlobalState                         SpdkGlobalState__;
static std::string                             SpdkNamespaceStr__;
static thread_local bool                       SpdkThreadInitialized__ = false;
static thread_local std::shared_ptr<SpdkQpair> SpdkThreadQP__;


void TrtSpdkIO::global_init(void) {
	SpdkGlobalState__.init();
}

void TrtSpdkIO::thread_init(void) {
	assert(SpdkThreadInitialized__ == false);
	trt_dmsg("Initializing SPDK\n");
	trt::SPDK::init(SpdkGlobalState__);
	trt_dmsg("Spawining SPDK poller\n");
	trt::T::spawn(trt::SPDK::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);
	trt_dmsg("init done\n");
	SpdkThreadInitialized__ = true;
}

void TrtSpdkIO::thread_exit(void) {
	trt::SPDK::stop();
}

// set the global string of which namespace to use.
static int
set_SpdkNamespaceStr(std::string ns_name) {
	assert(SpdkNamespaceStr__.empty());
	std::vector<std::string> namespaces = trt::SPDK::getNamespaceNames();
	if (namespaces.begin() == namespaces.end()) {
		trt_err("No SPDK namespaces found\n");
		errno = EINVAL;
		return -1;
	}

	auto match_ns = [&ns_name] (std::string const&ns) -> bool {
		return ns.find(ns_name) != std::string::npos;
	};

	auto ns_found = std::find_if(namespaces.begin(), namespaces.end(), match_ns);
	if (ns_found != namespaces.end()) {
		SpdkNamespaceStr__ = *ns_found;
		trt_msg("Found a match for user-provided string namespace: %s. Using: %s\n", ns_name.c_str(), SpdkNamespaceStr__.c_str());
	} else {
		// sort namespaces so we get some consistency across executions
		std::sort(namespaces.begin(), namespaces.end());
		SpdkNamespaceStr__ = *namespaces.begin();
		trt_msg("User-provided string namespace %s not found. Using: %s\n", ns_name.c_str(), SpdkNamespaceStr__.c_str());
	}

	return 0;
}

static int
setThreadQP(void) {
	assert(SpdkThreadInitialized__);
	assert(SpdkThreadQP__ == nullptr);
	SpdkThreadQP__ = trt::SPDK::getQpair(SpdkNamespaceStr__);
	if (SpdkThreadQP__ != nullptr)
		return 0;
	trt_err("Unable to get an SPDK queue pair for namespace \"%s\"\n", SpdkNamespaceStr__.c_str());
	errno = EIO;
	return -1;
}

static void setThreadQP_if_needed(void) {
	if (SpdkThreadQP__ != nullptr)
		return;
	int err = setThreadQP();
	if (err) {
		UDEPOT_ERR("setThreadQP failed");
		abort();
	}
}

int TrtSpdkIO::open(const char *pathname, int flags, mode_t mode) {
	int ret;

	assert(SpdkThreadInitialized__);
	if (SpdkNamespaceStr__.size() > 0) {
		UDEPOT_ERR("%s: already opened: %s\n", __PRETTY_FUNCTION__, SpdkNamespaceStr__.c_str());
		abort();
	}

	ret = set_SpdkNamespaceStr(std::string(pathname));
	if (ret < 0)
		return ret;
	// initialize this threads' queue pair, just to get an early error if
	// something goes wrong.
	return setThreadQP();
}

int TrtSpdkIO::close() {
	if (SpdkThreadQP__ == nullptr)
		return EIO;
	SpdkThreadQP__ = nullptr;
	return 0;
}

SpdkQpair *TrtSpdkIO::getThreadQP(void) {
	setThreadQP_if_needed();
	return SpdkThreadQP__.get();
}

ssize_t
TrtSpdkIO::pread_native(Ptr buff, size_t len, off_t off) {
	SpdkQpair *qp = getThreadQP();
	size_t bsize = qp->get_sector_size();

	// Maybe we can do better and somehow return a partial result when we can?
	// Or copy?
	if (len % bsize != 0) {
		UDEPOT_ERR("len (%zd) not aligned to block size (%zd)", len, bsize);
		errno = -EINVAL;
		return -1;
	}
	if (off % bsize != 0) {
		UDEPOT_ERR("offset (%zd) not aligned to block size (%zd)", off, bsize);
		errno = -EINVAL;
		return -1;
	}

	uint64_t lba_start = off / bsize;
	uint64_t nlbas = len / bsize;

	ssize_t ret = trt::SPDK::read(qp, buff, lba_start, nlbas);
	if (ret == -1)
		errno = EIO;

	return (ret*bsize);
}

ssize_t
TrtSpdkIO::pwrite_native(Ptr buff, size_t len, off_t off) {
	SpdkQpair *qp = getThreadQP();
	size_t bsize = qp->get_sector_size();

	// Maybe we can do better and somehow return a partial result when we can?
	// Or copy?
	if (len % bsize != 0) {
		UDEPOT_ERR("len (%zd) not aligned to block size (%zd)", len, bsize);
		errno = -EINVAL;
		return -1;
	}
	if (off % bsize != 0) {
		UDEPOT_ERR("offset (%zd) not aligned to block size (%zd)", off, bsize);
		errno = -EINVAL;
		return -1;
	}

	uint64_t lba_start = off / bsize;
	uint64_t nlbas = len / bsize;

	ssize_t ret = trt::SPDK::write(qp, buff, lba_start, nlbas);
	if (ret == -1)
		errno = EIO;

	return (ret*bsize);
}

ssize_t
TrtSpdkIO::preadv_native(IoVec<Ptr> iov_, off_t off) {
	ssize_t tot = 0;
	int iovcnt = iov_.iov_cnt_m;
	struct iovec *iov = iov_.iov_m;

	for (int i=0; i < iovcnt; i++) {
		void *iov_base = iov[i].iov_base;
		size_t iov_len = iov[i].iov_len;
		auto ptr = Ptr(iov_base, iov_len);
		ssize_t ret = pread_native(std::move(ptr), iov_len, off + tot);
		if (ret  == -1)
			return -1;
		tot += ret;
		// partial read, just return what we 've read so far
		if (static_cast<size_t>(ret) != iov_len)
			break;
	}

	return tot;
}

ssize_t
TrtSpdkIO::pwritev_native(IoVec<Ptr> iov_, off_t off) {
	ssize_t tot = 0;
	int iovcnt = iov_.iov_cnt_m;
	struct iovec *iov = iov_.iov_m;

	for (int i=0; i < iovcnt; i++) {
		void *iov_base = iov[i].iov_base;
		size_t iov_len = iov[i].iov_len;
		auto ptr = Ptr(iov_base, iov_len);
		ssize_t ret = pwrite_native(std::move(ptr), iov_len, off + tot);
		if (ret  == -1)
			return -1;
		tot += ret;
		// partial read, just return what we 've read so far
		if (static_cast<size_t>(ret) != iov_len)
			break;
	}

	return tot;
}

void *
TrtSpdkIO::mmap(void *addr, size_t len, int prot, int flags, off_t off) {
	std::cerr << "---------------------------------------------------------"
	          << std::endl
	          << "Doing anonymous mmap(). Data will be flushed to the drive at shutdown."
	          << std::endl
	          << "---------------------------------------------------------"
	          << std::endl;
	void *const ptr = ::mmap(addr, len, prot|PROT_WRITE, flags | MAP_ANONYMOUS, -1, off);
	if (MAP_FAILED == ptr)
		return ptr;
	ssize_t rc = 0, tot = 0;
	for (size_t i = 0; i < len; tot += rc) {
		const size_t l = std::min((len - i), (size_t) 8192U);
		rc = pread((void *) ((char *) ptr + i), l, off + i);
		if ((ssize_t) l != rc)
			break;
		i += l;
	}
	if ((ssize_t) len != tot) {
		const int rc2 __attribute__((unused)) = ::munmap(ptr, len);
		assert(0 == rc2);
		errno = EIO;
		return MAP_FAILED;
	}

       mtx_m.lock();
       mmap_off_m.emplace(ptr, (mregion)  { off, len});
       mtx_m.unlock();
	return ptr;
}

int
TrtSpdkIO::msync(void *addr, size_t len, int flags) {
	const uintptr_t loc = (uintptr_t)addr;
	std::map<void *, mregion>::iterator it = mmap_off_m.begin();
	mtx_m.lock();
	for (; it != mmap_off_m.end(); ++it) {
		const uintptr_t start = (uintptr_t) it->first;
		const uintptr_t end = start + it->second.len;
		if (start <= loc && loc < end) {
			break;
		}
	}
	mtx_m.unlock();
	if (it == mmap_off_m.end()) {
		errno = EINVAL;
		return -1;
	}

	ssize_t rc = 0, tot = 0;
	const off_t off = it->second.off;
	for (size_t i = 0; i < len; tot += rc) {
		const size_t l = std::min((len - i), (size_t) 8192U);
		rc = pwrite((void *) ((char *) addr + i), l, off + i);
		if ((ssize_t) l != rc)
			return rc;
		i += l;
	}
	return tot == (ssize_t) len ? 0 : ({errno = EIO; -1;});
}

int
TrtSpdkIO::munmap(void *addr, size_t len) {
	const uintptr_t loc = (uintptr_t)addr;
	std::map<void *, mregion>::iterator it = mmap_off_m.begin();
	mtx_m.lock();
	for (; it != mmap_off_m.end(); ++it) {
		const uintptr_t start = (uintptr_t) it->first;
		const uintptr_t end = start + it->second.len;
		if (start <= loc && loc < end) {
			break;
		}
	}
	if (it == mmap_off_m.end()) {
		mtx_m.unlock();
		errno = EINVAL;
		return -1;
	}
	mmap_off_m.erase(it);
	mtx_m.unlock();
	return ::munmap(addr, len);
}

} // end namespace udepot
