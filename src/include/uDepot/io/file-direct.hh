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
#ifndef	_UDEPOT_FILE_IO_DIRECT_H_
#define	_UDEPOT_FILE_IO_DIRECT_H_

namespace udepot {

#include "uDepot/io.hh"

class FileIODirect final : public udepot::FileIO_,
                           public IoBuffMemalign<4096> {
public:
	FileIODirect() : FileIO_() {}
	~FileIODirect() {}
	int open(const char *pathname, int flags, mode_t mode) override {
		fd_m = ::open(pathname, flags | O_DIRECT, mode);
		return fd_m == -1 ? errno : 0;
	}
	ssize_t pread(void *buff, size_t len, off_t off) override;
	ssize_t preadv(const struct iovec *iov, int iovcnt, off_t off) override;
	ssize_t pwrite(const void *buff, size_t len, off_t off) override;
	ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t off) override;

	static void global_init() { }
	static void thread_init() { }
	static void thread_exit() { }

	using Ptr = IoBuffMemalign<4096>::Ptr;
	ssize_t pread_native(Ptr buff, size_t len, off_t off) {
		return ::pread(fd_m, buff.ptr_m, len, off);
	}

	void *mmap(void *addr, size_t len, int prot, int flags, off_t off) override {
		return ::mmap(addr, len, prot, flags, fd_m, off);
	}

	ssize_t pwrite_native(Ptr buff, size_t len, off_t off) {
		return ::pread(fd_m, buff.ptr_m, len, off);
	}

	ssize_t preadv_native(IoVec<Ptr> iov, off_t off) {
		return ::preadv(fd_m, iov.iov_m, iov.iov_cnt_m, off);
	}

	ssize_t pwritev_native(IoVec<Ptr> iov, off_t off) {
		return ::pwritev(fd_m, iov.iov_m, iov.iov_cnt_m, off);
	}

private:
	FileIODirect(FileIODirect const &)   = delete;
	void operator=(FileIODirect const &) = delete;
};

#define	_MAX_ON_STACK	8192
#define buf_on_stack_or_malloc(__len, __ptr_out)				\
	char __local_buf[_MAX_ON_STACK + 4096];				\
	do {								\
		const size_t __aligned_len = align_up(__len, 4096);	\
		void *__ptr = nullptr;					\
		if (_MAX_ON_STACK < __len) {				\
			int rc = posix_memalign(&__ptr, 4096, __aligned_len); \
			if (0 != rc)					\
				__ptr = nullptr;			\
		} else {						\
			__ptr = (void *) ((((uintptr_t) &__local_buf[0] ) + 4096) & (~4095UL)); \
		}							\
		__ptr_out = (char *) __ptr;				\
	} while (0);

#define buf_on_stack_or_malloc_free(__ptr, __len) do {	\
		if (_MAX_ON_STACK < __len)		\
			free(__ptr); \
		} while (0)
static inline size_t iov_size(const struct iovec *iov, int iovcnt)
{
	size_t len = 0;
	for (; 0 < iovcnt; --iovcnt, ++iov)
		len += iov->iov_len;
	return len;
}

static inline void memcpy_buf_to_iov(const char *buf, const struct iovec *iov, int iovcnt, size_t len)
{
	for (; 0 < len && 0 < iovcnt; --iovcnt, ++iov) {
		const size_t l = std::min(len, iov->iov_len);
		assert(l);
		memcpy(iov->iov_base, buf, l);
		buf += l;
		len -= l;
	}
	assert(0 == iovcnt && 0 == len);
}

static inline void memcpy_iov_to_buf(const struct iovec *iov, int iovcnt, size_t len, char *buf)
{
	for (; 0 < len && 0 < iovcnt; --iovcnt, ++iov) {
		const size_t l = std::min(len, iov->iov_len);
		assert(l);
		memcpy(buf, iov->iov_base, l);
		buf += l;
		len -= l;
	}
	assert(0 == iovcnt && 0 == len);
}

} // end udepot namespace

#endif	// _UDEPOT_FILE_IO_DIRECT_H_
