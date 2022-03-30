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
#include "uDepot/io/trt-uring.hh"
#include "uDepot/io/file-direct.hh"

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_uring.hh"

namespace udepot {

const size_t MAX_TLS_FDS      = 128;
static thread_local std::array<int,MAX_TLS_FDS> fd_tls__{-1};

TrtFileIOUring::TrtFileIOUring() : TrtFileIO() {
	//trt_init_if_needed();
}

int
TrtFileIOUring::get_tls_fd(void) {

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

void TrtFileIOUring::trt_init_if_needed(void) {
	if (!trt::IOU::is_initialized())
		trt::IOU::init();
}

int
TrtFileIOUring::open(const char *pathname, int flags, mode_t mode) {

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


ssize_t TrtFileIOUring::pread(void *buff, size_t len, off_t off) {
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
		rc = trt::IOU::pread(get_tls_fd(), aligned_buf, aligned_len, off);
	} else {
		rc = ::pread(fd_m, aligned_buf, aligned_len, off);
	}
	assert(aligned_len == rc);
	memcpy(buff, aligned_buf, len);
	buf_on_stack_or_malloc_free(aligned_buf, len);
	return aligned_len == rc ? len : rc;
}

ssize_t TrtFileIOUring::pwrite(const void *buff, size_t len, off_t off) {
	ssize_t rc = 0, tot = 0;
	const size_t block_size = (1UL << 24); // 16 MB
	for (; 0 < len; tot += rc) {
		size_t l = std::min(len, block_size);
		if (unlikely(off & 4095)) {
			errno = EINVAL;
			return -1;
		}
		char *aligned_buf = nullptr;
		buf_on_stack_or_malloc(l, aligned_buf);
		if (nullptr == aligned_buf) {
			errno = ENOMEM;
			return -1;
		}
		memcpy(aligned_buf, buff, l);
		if (trt::T::in_trt()) {
			rc = trt::IOU::pwrite(get_tls_fd(), aligned_buf, l, off);
		} else {
			rc = ::pwrite(fd_m, aligned_buf, l, off);
		}
		buf_on_stack_or_malloc_free(aligned_buf, len);
		if (rc != (ssize_t) l) return rc < 0 ? rc : tot;
		len -= l;
		off += l;
		buff = (const void *) ((const char *) buff + l);
	}
	return tot;
}

ssize_t TrtFileIOUring::preadv(const struct iovec *iov, int iovcnt, off_t offset)
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
		rc = trt::IOU::pread(get_tls_fd(), aligned_buf, len, offset);
	} else {
		rc = ::pread(fd_m, aligned_buf, len, offset);
	}
	memcpy_buf_to_iov(aligned_buf, iov, iovcnt, len);
	buf_on_stack_or_malloc_free(aligned_buf, len);
	return aligned_len == rc ? len : rc;
}

ssize_t TrtFileIOUring::pwritev(const struct iovec *iov, int iovcnt, off_t offset)
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
		rc = trt::IOU::pwrite(get_tls_fd(), aligned_buf, len, offset);
	} else {
		rc = ::pwrite(fd_m, aligned_buf, len, offset);
	}
	buf_on_stack_or_malloc_free(aligned_buf, len);
	return rc;
}

ssize_t TrtFileIOUring::pread_native(Ptr buff, size_t len, off_t off)
{
	if (trt::T::in_trt()) {
		return trt::IOU::pread(get_tls_fd(), buff.ptr_m, len, off);
	} else {
		return ::pread(fd_m, buff.ptr_m, len, off);
	}
}

ssize_t TrtFileIOUring::pwrite_native(Ptr buff, size_t len, off_t off)
{
	if (trt::T::in_trt()) {
		return trt::IOU::pwrite(get_tls_fd(), buff.ptr_m, len, off);
	} else {
		return ::pwrite(fd_m, buff.ptr_m, len, off);
	}
}

ssize_t TrtFileIOUring::preadv_native(IoVec<Ptr>  iov, off_t off) {
	if (trt::T::in_trt()) {
		return trt::IOU::preadv(get_tls_fd(), iov.iov_m, iov.iov_cnt_m, off);
	} else {
		return ::preadv(fd_m, iov.iov_m, iov.iov_cnt_m, off);
	}
}

ssize_t TrtFileIOUring::pwritev_native(IoVec<Ptr> iov, off_t off) {
	if (trt::T::in_trt()) {
		return trt::IOU::pwritev(get_tls_fd(), iov.iov_m, iov.iov_cnt_m, off);
	} else {
		return ::pwritev(fd_m, iov.iov_m, iov.iov_cnt_m, off);
	}
}

void TrtFileIOUring::global_init() { }


// per trt scheduler initialization
void TrtFileIOUring::thread_init() {
	trt_dmsg("spawing iou_poller\n");
	trt::IOU::init();
	trt_dmsg("state initialized=%d\n", trt::IOU::is_initialized());
	trt::T::spawn(trt::IOU::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);

}

void TrtFileIOUring::thread_exit() {
	trt::IOU::stop();
}

}; // end namespace udepot
