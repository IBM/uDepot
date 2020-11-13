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

#ifndef	_UDEPOT_SPDK_IO_H_
#define	_UDEPOT_SPDK_IO_H_

#include "uDepot/io.hh"
#include "uDepot/io/spdk-common.hh"

#include <map>

#include "trt_util/spdk.hh"

namespace udepot {

class SpdkIO final : public uDepotIO_, public SpdkNativeNYI {

public:
	static void global_init();
	static void thread_init();
	static void thread_exit();

	SpdkIO(SpdkIO const &) = delete;
	void operator=(SpdkIO const &) = delete;
	SpdkIO() {};

	// @pathname, @flags, and @mode arguments are currently ignored
	int open(const char *pathname, int flags, mode_t mode) override;
	int close() override;

	// no-ops
	void prefetch(size_t len, off_t off) override { }
	int unlink(const char *pathname) override { return 0; };

	// not supported, will return an error
	int ftruncate(off_t len) override {
		errno = ENOTSUP;
		return -1;
	}

	u64 get_size() override;
	ssize_t pread(void *buff, size_t len, off_t off) override;
	ssize_t preadv(const struct iovec *iov, int iovcnt, off_t off) override;
	ssize_t pwrite(const void *buff, size_t len, off_t off) override;
	ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t off) override;
	void *mmap(void *addr, size_t len, int prot, int flags, off_t off) override;
	int msync(void *addr, size_t len, int flags) override;
	int munmap(void *addr, size_t len) override;
private:
	std::map<void *, off_t> mmap_off_m;
	ssize_t pwritev(const struct iovec *const iov, const int iovcnt, const off_t off, SpdkPtr &buf);
	ssize_t preadv(const struct iovec *const iov, const int iovcnt, const off_t off, SpdkPtr &buf);
};
} // end udepot namespace
#endif /* _UDEPOT_SPDK_IO_H_ */
