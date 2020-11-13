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

#ifndef	_UDEPOT_IO_H_
#define	_UDEPOT_IO_H_

#include <algorithm>
#include <cassert>
#include <errno.h>
#include <fcntl.h>
#include <memory>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "util/debug.h"
#include "util/types.h"

#include "trt_util/io_ptr.hh" // IoPtr, IoVec, etc.

/**
 * I/O backend for uDepot
 */

namespace udepot {


// Abstract base class:
//  Essentially fd operations, without the fd
//
// NB: This is not strictly needed since we use templates and not the base
// class, but I leave it as a interface description (at least until C++ concepts
// become available). --KOU
class uDepotIO_ {
public:
	virtual int open(const char *pathname, int flags, mode_t mode) = 0;
	virtual int close() = 0;

	virtual ssize_t pwrite(const void *buff, size_t count, off_t off) = 0;
	virtual ssize_t pread(void *buff, size_t count, off_t off) = 0;

	virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) = 0;
	virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) = 0;

	virtual void prefetch(size_t count, off_t off) = 0;

	// returns (u64)-1 on error or size
	virtual u64 get_size() = 0;
	virtual int ftruncate(off_t len) = 0;
	virtual int unlink(const char *pathname) = 0;

	virtual void *mmap(void *addr, size_t length, int prot, int flags, off_t offset) = 0;

	virtual int msync(void *addr, size_t len, int flags) = 0;
	virtual int munmap(void *addr, size_t len) = 0;

	virtual ~uDepotIO_() {}
protected:
	struct mregion {
		off_t off;
		size_t len;
	};
};



/**
 * Trivial implementation of FileIO for normal file descriptors
 */
class FileIO_ : public uDepotIO_ {
public:
	FileIO_() : fd_m(-1) {}
	FileIO_(int fd): fd_m(fd) {}
	FileIO_(FileIO_ const &)        = delete;
	void operator=(FileIO_ const &) = delete;

	int open(const char *pathname, int flags, mode_t mode) override {
		fd_m = ::open(pathname, flags, mode);
		return fd_m == -1 ? errno : 0;
	}

	int close() override {
		int ret = ::close(fd_m);
		fd_m = -1;
		return ret;
	}

	void prefetch(size_t len, off_t off) override {
		while (len) {
			const size_t l = std::min(len, (size_t) 1048576);
			const int err __attribute__((unused)) = ::readahead(fd_m, off, l);
			assert(0 == err);
			len -= l;
			off += l;
		}
		// ::posix_fadvise(fd_m, off, len, POSIX_FADV_WILLNEED | POSIX_FADV_SEQUENTIAL);
	}

	ssize_t pread(void *buff, size_t len, off_t off) override {
		return ::pread(fd_m, buff, len, off);
	}

	ssize_t preadv(const struct iovec *iov, int iovcnt, off_t off) override {
		return ::preadv(fd_m, iov, iovcnt, off);
	}

	ssize_t pwrite(const void *buff, size_t len, off_t off) override {
		return ::pwrite(fd_m, buff, len, off);
	}

	ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t off) override {
		return ::pwritev(fd_m, iov, iovcnt, off);
	}

	u64 get_size() override {
		return ::lseek64(fd_m, 0, SEEK_END);
	}

	int ftruncate(off_t len) override {
		return ::ftruncate(fd_m, len);
	}

	int unlink(const char *pathname) override {
		return ::unlink(pathname);
	}

	void *mmap(void *addr, size_t len, int prot, int flags, off_t off) override {
		return ::mmap(addr, len, prot, flags, fd_m, off);
	}

	int msync(void *addr, size_t len, int flags) override {
		return ::msync(addr, len, flags);
	}

	int munmap(void *addr, size_t len) override {
		return ::munmap(addr, len);
	}

	virtual ~FileIO_() {
		if (fd_m != -1)
			close();
	}
protected:
	int fd_m;
};


// An IO class must:
//  - implement the methods of uDepotIO_
//  - inherit from an allocator (e.g., IoBuffMalloc, IoBuffMalloc)
//  - implement pread_native(), pwrite_native() methods as below.
//  - implement preadv_native(), pwritev_native() methods as below.
//
// TODO: write C++ concepts for these, which can be checked by compilers
// supporting -fconcepts and act as documention even when the compiler does not
// support it.
class FileIO : public FileIO_, public IoBuffMalloc {
public:
	FileIO(int fd) : FileIO_(fd) {};
	FileIO()       : FileIO_()   {};

	static void global_init() { }
	static void thread_init() { }
	static void thread_exit() { }

	// Native versions are the same with the normal ones
	using Ptr = IoBuffMalloc::Ptr;
	ssize_t pread_native(Ptr buff, size_t len, off_t off) {
		return pread(buff.ptr_m, len, off);
	}

	ssize_t pwrite_native(Ptr buff, size_t len, off_t off) {
		return pwrite(buff.ptr_m, len, off);
	}

	// error is returned as negative number
	ssize_t preadv_native(IoVec<Ptr>  iov, off_t off) {
		return preadv(iov.iov_m, iov.iov_cnt_m, off);
	}
	ssize_t pwritev_native(IoVec<Ptr> iov, off_t off) {
		return pwritev(iov.iov_m, iov.iov_cnt_m, off);
	}
};


} // end udepot namespace

#endif /* _UDEPOT_IO_H_ */
