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

#include <mutex>
#include <atomic>

#include "uDepot/io.hh"
#include "uDepot/io/trt-aio.hh"
#include "uDepot/io/file-direct.hh"

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_aio.hh"

namespace udepot {

const size_t MAX_TLS_FDS      = 128;
static thread_local std::array<int,MAX_TLS_FDS> fd_tls__{-1};

static size_t
get_fd_tls_id(void) {
	static std::atomic<size_t> fd_tls_id_next__(0);
	return fd_tls_id_next__.fetch_add(1);
}

int
TrtFileIO::get_tls_fd(void) {

	if (fd_tls_id_m >= MAX_TLS_FDS)
		return fd_m;

	int fd = fd_tls__[fd_tls_id_m];
	if (fd != -1)
		return fd;

	fd = ::open(pathname_m.c_str(), flags_m, mode_m);
	if (fd == -1) {
		UDEPOT_ERR("Error openning %s: %s\n", pathname_m.c_str(), strerror(errno));
		fd = fd_m;
	}

	fd_tls__[fd_tls_id_m] = fd;
	return fd;
}

void TrtFileIO::trt_init_if_needed(void) {
	if (!trt::AIO::is_initialized())
		trt::AIO::init();
}

TrtFileIO::TrtFileIO() : fd_tls_id_m(get_fd_tls_id()) {
	//trt_init_if_needed();
}

int
TrtFileIO::open(const char *pathname, int flags, mode_t mode) {

	if (pathname_m.size() != 0) {
		UDEPOT_ERR("FIXME: re-opening is not currently supported");
		return EINVAL;
	}

	fd_m = ::open(pathname, flags | O_DIRECT | O_NOATIME, mode);
	if (fd_m == -1)
		return errno;

	pathname_m = std::string(pathname);
	flags_m = flags | O_DIRECT | O_NOATIME;
	mode_m = mode;

	#if 0
	// This should disable readahead, but it does not seem to effect
	// performance, so we leave it disabled for now.
	int err = posix_fadvise64(fd_m, 0, 0, POSIX_FADV_RANDOM);
	if (err)
		return err;
	#endif
	return 0;
}

// If we are not inside TRT, just do the blocking operation
//
// NB: Because we use the aio interface, the fd does not need to be set as
// non-blocking, so this should work.


ssize_t TrtFileIO::pread(void *buff, size_t len, off_t off) {
	if (unlikely(off & 511)) {
		errno = EINVAL;
		return -1;
	}
	char *aligned_buf = nullptr;
	buf_on_stack_or_malloc(len, aligned_buf);
	if (nullptr == aligned_buf) {
		errno = ENOMEM;
		return -1;
	}
	const ssize_t aligned_len = align_up(len, 512);
	ssize_t rc;
	if (trt::T::in_trt()) {
		rc = trt::AIO::pread(get_tls_fd(), aligned_buf, aligned_len, off);
	} else {
		rc = ::pread(fd_m, aligned_buf, aligned_len, off);
	}
	assert(aligned_len == rc);
	memcpy(buff, aligned_buf, len);
	buf_on_stack_or_malloc_free(aligned_buf, len);
	return aligned_len == rc ? len : rc;
}

ssize_t TrtFileIO::pwrite(const void *buff, size_t len, off_t off) {
	if (unlikely(off & 4095)) {
		errno = EINVAL;
		return -1;
	}
	char *aligned_buf = nullptr;
	buf_on_stack_or_malloc(len, aligned_buf);
	if (nullptr == aligned_buf) {
		errno = ENOMEM;
		return -1;
	}
	memcpy(aligned_buf, buff, len);
	ssize_t rc;
	if (trt::T::in_trt()) {
		rc = trt::AIO::pwrite(get_tls_fd(), aligned_buf, len, off);
	} else {
		rc = ::pwrite(fd_m, aligned_buf, len, off);
	}
	buf_on_stack_or_malloc_free(aligned_buf, len);
	return rc;
}

ssize_t TrtFileIO::preadv(const struct iovec *iov, int iovcnt, off_t offset)
{
	const size_t len = iov_size(iov, iovcnt);
	if (unlikely(offset & 511)) {
		errno = EINVAL;
		return -1;
	}
	char *aligned_buf = nullptr;
	buf_on_stack_or_malloc(len, aligned_buf);
	if (nullptr == aligned_buf) {
		errno = ENOMEM;
		return -1;
	}
	const ssize_t aligned_len = align_up(len, 512);
	ssize_t rc;
	if (trt::T::in_trt()) {
		rc = trt::AIO::pread(get_tls_fd(), aligned_buf, len, offset);
	} else {
		rc = ::pread(fd_m, aligned_buf, len, offset);
	}
	memcpy_buf_to_iov(aligned_buf, iov, iovcnt, len);
	buf_on_stack_or_malloc_free(aligned_buf, len);
	return aligned_len == rc ? len : rc;
}

ssize_t TrtFileIO::pwritev(const struct iovec *iov, int iovcnt, off_t offset)
{
	if (unlikely(offset & 511)) {
		errno = EINVAL;
		return -1;
	}
	const size_t len = iov_size(iov, iovcnt);
	char *aligned_buf = nullptr;
	buf_on_stack_or_malloc(len, aligned_buf);
	if (nullptr == aligned_buf) {
		errno = ENOMEM;
		return -1;
	}
	memcpy_iov_to_buf(iov, iovcnt, len, aligned_buf);
	ssize_t rc;
	if (trt::T::in_trt()) {
		rc = trt::AIO::pwrite(get_tls_fd(), aligned_buf, len, offset);
	} else {
		rc = ::pwrite(fd_m, aligned_buf, len, offset);
	}
	buf_on_stack_or_malloc_free(aligned_buf, len);
	return rc;
}

ssize_t TrtFileIO::pread_native(Ptr buff, size_t len, off_t off)
{
	if (trt::T::in_trt()) {
		return trt::AIO::pread(get_tls_fd(), buff.ptr_m, len, off);
	} else {
		return ::pread(fd_m, buff.ptr_m, len, off);
	}
}

ssize_t TrtFileIO::pwrite_native(Ptr buff, size_t len, off_t off)
{
	if (trt::T::in_trt()) {
		return trt::AIO::pwrite(get_tls_fd(), buff.ptr_m, len, off);
	} else {
		return ::pwrite(fd_m, buff.ptr_m, len, off);
	}
}

ssize_t TrtFileIO::preadv_native(IoVec<Ptr>  iov, off_t off) {
	if (trt::T::in_trt()) {
		return trt::AIO::preadv(get_tls_fd(), iov.iov_m, iov.iov_cnt_m, off);
	} else {
		return ::preadv(fd_m, iov.iov_m, iov.iov_cnt_m, off);
	}
}

ssize_t TrtFileIO::pwritev_native(IoVec<Ptr> iov, off_t off) {
	if (trt::T::in_trt()) {
		return trt::AIO::pwritev(get_tls_fd(), iov.iov_m, iov.iov_cnt_m, off);
	} else {
		return ::pwritev(fd_m, iov.iov_m, iov.iov_cnt_m, off);
	}
}

void TrtFileIO::global_init() { }


// per trt scheduler initialization
void TrtFileIO::thread_init() {
	trt_dmsg("spawing aio_poller\n");
	trt::AIO::init();
	trt_dmsg("state initialized=%d\n", trt::AIO::is_initialized());
	trt::T::spawn(trt::AIO::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);

}

void TrtFileIO::thread_exit() {
	trt::AIO::stop();
}

static std::mutex mmap_mutex_g;
 void *
TrtFileIO::mmap(void *const addr, size_t len, const int prot, const int flags, const off_t off)
{
       UDEPOT_DBG("---------------------------------------------------------");
       UDEPOT_DBG("Chosen anonymous mmap, data will be flushed at the end.");
       UDEPOT_DBG("---------------------------------------------------------");
       void *const ptr = ::mmap(addr, len, prot|PROT_WRITE, flags | MAP_ANONYMOUS, -1, off);
       if (MAP_FAILED == ptr)
               return ptr;
       struct iovec iov[1] = {
	       {.iov_base = ptr, .iov_len = len},
       };
       IoVec<Ptr> iov_ptr(iov, 1);
       const ssize_t rc = preadv_native(iov_ptr, off);
       if ((ssize_t) len != rc) {
	       UDEPOT_MSG("preadv failed  addr=%p len=%lu rc=%ld off=%lu err=%s.", ptr, len, rc, off, strerror(errno));
               const int rc2 __attribute__((unused)) = ::munmap(ptr, len);
               assert(0 == rc2);
               errno = EIO;
               return MAP_FAILED;
       }
       UDEPOT_MSG("mmap pread addr=%p len=%lu rc=%ld off=%lu.", ptr, len, rc, off);

       mmap_mutex_g.lock();
       mmap_off_m.emplace(ptr, (mregion)  { off, len});
       mmap_mutex_g.unlock();
       return ptr;
}

int
TrtFileIO::msync(void *addr, size_t len, int flags)
{
	const uintptr_t loc = (uintptr_t)addr;
	std::map<void *, mregion>::iterator f = mmap_off_m.begin();
	mmap_mutex_g.lock();
	for (; f != mmap_off_m.end(); ++f) {
		const uintptr_t start = (uintptr_t) f->first;
		const uintptr_t end = start + f->second.len;
		if (start <= loc && loc < end) {
			break;
		}
	}
	mmap_mutex_g.unlock();
	if (f == mmap_off_m.end()) {
		UDEPOT_MSG("msync failed addr=%p len=%lu.", addr, len);
		errno = EINVAL;
		return -1;
	}

	const ssize_t rc = pwrite(addr, len, f->second.off);
	UDEPOT_MSG("msync pwrite addr=%p len=%lu rc=%ld off=%lu errno=%s.",
		   addr, len, rc, f->second.off, strerror(errno));
	return (ssize_t) len == rc ? 0 : ({errno = EIO; -1;});
}

int
TrtFileIO::munmap(void *addr, size_t len)
{
	const uintptr_t loc = (uintptr_t)addr;
	std::map<void *, mregion>::iterator f = mmap_off_m.begin();
	mmap_mutex_g.lock();
	for (; f != mmap_off_m.end(); ++f) {
		const uintptr_t start = (uintptr_t) f->first;
		const uintptr_t end = start + f->second.len;
		if (start <= loc && loc < end) {
			break;
		}
	}
	if (f == mmap_off_m.end()) {
		mmap_mutex_g.unlock();
		errno = EINVAL;
		return -1;
	}
	mmap_off_m.erase(f);
	mmap_mutex_g.unlock();
	return ::munmap(addr, len);
}

}; // end namespace udepot
