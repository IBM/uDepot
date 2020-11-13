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

#ifndef _UDEPOT_IO_HELPERS_
#define _UDEPOT_IO_HELPERS_

namespace udepot {

// IO helper functions
//
// Full (_full()) versions of the operations are expected to deal with partially
// successful read/write calls, so that the caller does not have to deal with
// it. Unfortunately, they are not very well tested. One way to do that might be 
// to have an IO backend that performs partial read/writes

#include "uDepot/mbuff.hh"
#include "uDepot/io.hh"
#include "util/debug.h"

static inline size_t
iovec_len(const struct iovec *iov, unsigned iovcnt)
{
	size_t ret = 0;
	for (unsigned i=0; i < iovcnt; i++) {
		ret += iov[i].iov_len;
	}
	return ret;
}

// Returns <error, bytes_read>.
//
//  Uses pread_native() which behaves similarly to pread()
//  It is considered an error for pread_native() to return zero before we
//  read all the requested bytes. In this case, pread_native_full()
//  returns <ENODATA, bytes_read>;
//
template<typename IO>
static inline
std::tuple<int, size_t>
io_pread_native_full(IO &io, typename IO::Ptr &&buff, size_t io_len, off_t io_off) {
	size_t bytes_read = 0;
	int err;
	for(;;)  {
		assert(io_len > bytes_read);
		ssize_t ret;
		size_t op_len = io_len - bytes_read;
		size_t op_off = io_off + bytes_read;
		ret = io.pread_native(std::forward(buff), op_len, op_off);
		if (ret == -1) {
			err = errno;
			break;
		} else if (ret == 0) {
			err = ENODATA;
			break;
		}

		bytes_read += ret;
		if (bytes_read == io_len) {
			err = 0;
			break;
		}
		buff.ptr_m += ret;
	}

	return std::make_tuple(err, bytes_read);
}

// Append data to mbuff by calling pread() on io.
//
//  Performs a single call of io.pread_native()
//  Returns:
//   > 0 -> bytes read
//   < 0 -> -error
template<typename IO>
static inline
ssize_t
io_pread_mbuff_append(IO &io, Mbuff &mb, size_t io_len, off_t io_off) {
	// eagerly check if there is enough size for the operation
	if (mb.append_avail_size() < io_len) {
		UDEPOT_ERR("mbuff: not enough free space for operation (get_free_size(): %zd, io_len: %zd)", mb.get_free_size(), io_len);
		return -ENOSPC;
	}

	// check that mbuff has (proper) native IO buffers
	assert(mb.ioptr_compatible<typename IO::Ptr>());

	// build an appropriate iovec for the operation
	const size_t iov_size = 16;
	struct iovec iov[iov_size];
	size_t remaining = io_len;
	size_t iov_cnt = 0;
	auto append_fn =
		[&remaining, &iov, &iov_size, &iov_cnt]
		(unsigned char *b, size_t len) -> size_t {
			if (iov_cnt == iov_size || remaining == 0)
				return 0;
			size_t min_len = std::min(remaining, len);
			iov[iov_cnt].iov_base = b;
			iov[iov_cnt].iov_len = min_len;
			iov_cnt++;
			remaining -= min_len;
			return min_len;
		};
	mb.append(std::ref(append_fn));
	size_t op_bytes = io_len - remaining;
	assert(iovec_len(iov, iov_cnt) == op_bytes);

	size_t io_ret;
	if (iov_cnt == 1) { // This is silly, but it seems to improve ubench perf a bit
		typename IO::Ptr io_ptr(iov[0].iov_base, iov[0].iov_len);
		io_ret = io.pread_native(std::move(io_ptr), iov[0].iov_len, io_off);
	} else {
		IoVec<typename IO::Ptr> iov_ptr(iov, iov_cnt);
		io_ret = io.preadv_native(iov_ptr, io_off);
	}

	// in case of an error or a partial read, invalidate the mbuff area that was
	// not written
	if (io_ret < 0) {
		mb.reslice(mb.get_valid_size() - op_bytes);
		io_ret = -errno;
	} else if (static_cast<size_t>(io_ret) < op_bytes) {
		mb.reslice(mb.get_valid_size() - op_bytes + io_ret);
	}

	return io_ret;
}

// Returns <error, bytes_read>.
//
// Calls io_pread_mbuff_append(). If it returns 0 (EOF), then this function
// returs ENODATA;
template<typename IO>
static inline
std::tuple<int, size_t>
io_pread_mbuff_append_full(IO &io, Mbuff &mb, size_t io_len, off_t io_off) {

	size_t bytes_read = 0;
	int err = 0;

	if (io_len == 0) {
		UDEPOT_DBG("Request to read %zd bytes\n", io_len);
		return std::make_tuple(0, 0);
	}

	while (bytes_read < io_len) {
		size_t op_len = io_len - bytes_read;
		size_t op_off = io_off + bytes_read;
		ssize_t ret = io_pread_mbuff_append(io, mb, op_len, op_off);
		UDEPOT_DBG("ret=%zd len:%zd mb valid size:%zd\n", ret, op_len, mb.get_valid_size());
		if (ret < 0) {
			err = -(static_cast<int>(ret));
			break;
		} else if (ret == 0) {
			err = ENODATA;
			break;
		}

		bytes_read += ret;
	}

	return std::make_tuple(err, bytes_read);
}

// Write from mbuff:
//  io_len: amount of data to write
//  io_off: where to write in the IO device
//  mb: mbuff to get data from
//  mbuff_off: offset in mbuff to get data from
template<typename IO>
static inline
ssize_t
io_pwrite_mbuff(IO &io, size_t io_len, off_t io_off, Mbuff &mb, size_t mbuff_off)
{
	size_t op_bytes;
	int iov_cnt;

	// build an appropriate iovec for the operation
	const size_t iov_size = 16;
	struct iovec iov[iov_size];
	std::tie(iov_cnt, op_bytes) = mb.fill_iovec_valid(mbuff_off, io_len, iov, iov_size);
	if (iov_cnt < 0) {
		UDEPOT_ERR("fill_iovec_valid failed with%s", strerror(-iov_cnt));
		return iov_cnt;
	}
	assert(iovec_len(iov, iov_cnt) == op_bytes);
	IoVec<typename IO::Ptr> iov_ptr(iov, iov_cnt);
	ssize_t io_ret = io.pwritev_native(iov_ptr, io_off);
	return io_ret;
}

template<typename IO>
static inline
std::tuple<int, ssize_t>
io_pwrite_mbuff_full(IO &io, size_t io_len, off_t io_off, Mbuff &mb, size_t mbuff_off = 0)
{
	size_t bytes_written = 0;
	int err = 0;
	while (bytes_written < io_len) {
		size_t op_len = io_len - bytes_written;
		size_t op_off = io_off + bytes_written;
		ssize_t ret = io_pwrite_mbuff(io, op_len, op_off, mb, mbuff_off + bytes_written);
		if (ret < 0) {
			err = -(static_cast<int>(ret));
			break;
		}
		assert(ret != 0);
		bytes_written += ret;
	}

	return std::make_tuple(err, bytes_written);
}

}

#endif /* ifndef _UDEPOT_IO_HELPERS_ */
