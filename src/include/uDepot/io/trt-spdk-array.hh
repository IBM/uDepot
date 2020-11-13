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

#ifndef	_UDEPOT_TRT_SPDK_ARRAY_IO_H_
#define	_UDEPOT_TRT_SPDK_ARRAY_IO_H_

#include "uDepot/io.hh"
#include "uDepot/io/spdk-common.hh"

#include "trt_util/rte.hh"

#include <map>
#include <functional>

struct SpdkQpair;

namespace udepot {

// NB: RteAlloc is a slow-path allocator. For the cases we care about we use
// mbuffs, and we cache buffers anyway so nothing smarter seems to be required.
// If this is not true, we can always implement a caching allocator for RTE
// buffers.
class TrtSpdkArrayIO final : public uDepotIO_, public RteAlloc<512> {

public:
	static void global_init();
	static void thread_init();
	static void thread_exit();

	TrtSpdkArrayIO(TrtSpdkArrayIO const &) = delete;
	void operator=(TrtSpdkArrayIO const &) = delete;
	TrtSpdkArrayIO();

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
	int msync(void *addr, size_t len, int flags);
	int munmap(void *addr, size_t len);

	using Ptr = RteAlloc<512>::Ptr;
	ssize_t pread_native(Ptr buff, size_t len, off_t off);
	ssize_t pwrite_native(Ptr buff, size_t len, off_t off);
	ssize_t preadv_native(IoVec<Ptr>  iov, off_t off);
	ssize_t pwritev_native(IoVec<Ptr> iov, off_t off);

private:
	std::map<void *, mregion> mmap_off_m;
	using PioFn = std::function<ssize_t (SpdkQpair *, void *, size_t, off_t)>;
	using PiovFn = std::function<ssize_t (SpdkQpair *, const struct iovec *, int, off_t)>;
	PioFn pread_iofn_m;
	PioFn pwrite_iofn_m;
	PiovFn preadv_iofn_m;
	PiovFn pwritev_iofn_m;

	PioFn pread_native_iofn_m;
	PioFn pwrite_native_iofn_m;
	PiovFn preadv_native_iofn_m;
	PiovFn pwritev_native_iofn_m;
	ssize_t pio(void *, size_t, off_t, const PioFn &);
	ssize_t piov(const struct iovec *, int, off_t, const PiovFn &);
	static ssize_t pread_native_fn(SpdkQpair *, void *, size_t , off_t );
	static ssize_t pwrite_native_fn(SpdkQpair *, void *, size_t , off_t );
	static ssize_t preadv_native_fn(SpdkQpair *, const struct iovec *, int, off_t);
	static ssize_t pwritev_native_fn(SpdkQpair *, const struct iovec *, int, off_t);
};

} // end udepot namespace
#endif /* _UDEPOT_TRT_SPDK_ARRAY_IO_H_ */
