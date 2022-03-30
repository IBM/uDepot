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

#ifndef	_UDEPOT_TRT_URING_H_
#define	_UDEPOT_TRT_URING_H_

/**
 * trt file IO uring backend for uDepot
 */

#include "uDepot/io.hh"
#include "uDepot/io/trt-aio.hh"

#include <map>
namespace udepot {
class TrtFileIOUring : public udepot::TrtFileIO {
public:
	TrtFileIOUring();
	TrtFileIOUring(FileIO const &)         = delete;
	void operator=(TrtFileIOUring const &) = delete;

	static void global_init();
	static void thread_init();
	static void thread_exit();

	int open(const char *pathname, int flags, mode_t mode) override;

	ssize_t pwrite(const void *buff, size_t len, off_t off) override;
	ssize_t pread(void *buff, size_t len, off_t off) override;

	ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override;
	ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override;

	using Ptr = IoBuffMemalign<4096>::Ptr;
	ssize_t pread_native(Ptr buff, size_t len, off_t off) override;

	ssize_t pwrite_native(Ptr buff, size_t len, off_t off) override;

	// error is returned as negative number
	ssize_t preadv_native(IoVec<Ptr>  iov, off_t off) override;

	ssize_t pwritev_native(IoVec<Ptr> iov, off_t off) override;
private:
	void trt_init_if_needed() override;
	int get_tls_fd(void) override;
};

} // namespace udepot
#endif	// _UDEPOT_TRT_URING_H_
