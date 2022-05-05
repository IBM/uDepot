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

#ifndef	_UDEPOT_TRT_AIO_H_
#define	_UDEPOT_TRT_AIO_H_

/**
 * trt file IO backend for uDepot
 */

#include <map>

namespace udepot {

#include "uDepot/io.hh"

/**
 * Trt aio file backend
 */
class TrtFileIO : public udepot::FileIO_,
		  public IoBuffMemalign<4096> {
public:
	TrtFileIO();
	TrtFileIO(FileIO const &)         = delete;
	void operator=(TrtFileIO const &) = delete;

	static void global_init();
	static void thread_init();
	static void thread_exit();

	ssize_t pwrite(const void *buff, size_t len, off_t off) override;
	ssize_t pread(void *buff, size_t len, off_t off) override;

	ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override;
	ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override;

	int open(const char *pathname, int flags, mode_t mode) override;

	#if 0
	int close() override {
		// XXX: Don't close FD since we might have pending operations on it.
		// TODO: Register it for closing when we shut-down AIO
		fd_m = -1;
		return 0;
	}
	#endif

	// Native versions are the same with the normal ones
	// NB: It would actually made more sense if pread() and pwrite() were
	// implemented by calling pread_native() and pwritev_native().
	using Ptr = IoBuffMemalign<4096>::Ptr;
	virtual ssize_t pread_native(Ptr buff, size_t len, off_t off);

	virtual ssize_t pwrite_native(Ptr buff, size_t len, off_t off);

	// error is returned as negative number
	virtual ssize_t preadv_native(IoVec<Ptr>  iov, off_t off);

	virtual ssize_t pwritev_native(IoVec<Ptr> iov, off_t off);

	void *mmap(void *addr, size_t len, int prot, int flags, off_t off) override;
	int msync(void *addr, size_t len, int flags) override;
	int munmap(void *addr, size_t len) override;

	~TrtFileIO() {}

protected:
	std::map<void *, mregion> mmap_off_m;

	std::string pathname_m;
	int         flags_m;
	mode_t      mode_m;
	size_t      fd_tls_id_m;

	virtual void trt_init_if_needed();
	virtual int get_tls_fd(void);
};

} // end udepot namespace

#endif /* _UDEPOT_TRT_AIO_H_ */
