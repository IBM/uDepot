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
#include "uDepot/io/file-direct.hh"

#include <cstring>

#include "util/debug.h"

namespace udepot {
ssize_t FileIODirect::pread(void *const buf, const size_t len, const off_t off) {
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
	const ssize_t rc = ::pread(fd_m, aligned_buf, aligned_len, off);
	assert(aligned_len == rc);
	memcpy(buf, aligned_buf, len);
	buf_on_stack_or_malloc_free(aligned_buf, len);
	return aligned_len == rc ? len : rc;
}

ssize_t FileIODirect::pwrite(const void *const buf, const size_t len, const off_t off) {
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
	memcpy(aligned_buf, buf, len);
	const ssize_t rc = ::pwrite(fd_m, aligned_buf, len, off);
	buf_on_stack_or_malloc_free(aligned_buf, len);
	return rc;
}

ssize_t FileIODirect::preadv(const struct iovec *const iov, const int iovcnt, const off_t off) {
	const size_t len = iov_size(iov, iovcnt);
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
	const ssize_t rc = ::pread(fd_m, aligned_buf, aligned_len, off);
	memcpy_buf_to_iov(aligned_buf, iov, iovcnt, len);
	buf_on_stack_or_malloc_free(aligned_buf, len);
	return aligned_len == rc ? len : rc;
	// return ::preadv(fd_m, iov, iovcnt, off);
}

ssize_t FileIODirect::pwritev(const struct iovec *const iov, const int iovcnt, const off_t off) {
	if (unlikely(off & 511)) {
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
	const ssize_t rc = ::pwrite(fd_m, aligned_buf, len, off);
	buf_on_stack_or_malloc_free(aligned_buf, len);
	return rc;
	// return ::pwritev(fd_m, iov, iovcnt, off);
}
}; // end namespace udepot
