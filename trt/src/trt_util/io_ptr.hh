/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4:

#ifndef _IO_PTR_H__
#define _IO_PTR_H__

#include <stdlib.h>   // posix_memalign
#include <string.h>   // memcpy
#include <inttypes.h> // uint32_t
#include <tuple>

// IoPtr is a wrapper for pointers to buffers for doing IO.
//
// Different IO backends have different requirments for the IO buffers (e.g.,
// direct IO needs aligned buffers and SPDK needs buffers allocated with
// rte_malloc()). To distinguish between these different cases, we add a
// template parameter to IoPtr that acts as a tag (sometimes this is called a
// phantom) type. The tag corresponds to the allocator that was used for this
// buffer. We can use the tag to check that we get buffers properly allocated.
//
// An example of such a use can be found in MBuff::add_iobuff()
//
// Yes, this could be a smart pointer.
// Yes, this is not perfect but I think it's better than nothing.
template<typename AllocatorTag>
struct IoPtr {
	void *ptr_m;
	size_t size_m;

	// default constructor creates an invalid pointer
	IoPtr() : ptr_m(nullptr), size_m(0) {}

	explicit IoPtr(void *p, uint32_t size) : ptr_m(p), size_m(size) {}

	IoPtr(IoPtr &&p) {
		std::swap(ptr_m, p.ptr_m);
		std::swap(size_m, p.size_m);
	}

	void operator=(IoPtr &&p) {
		std::swap(ptr_m, p.ptr_m);
		std::swap(size_m, p.size_m);
	}

	IoPtr(IoPtr const&) = delete;
	void operator=(IoPtr const &) = delete;

    // helper functions
    void copy_from_iovec(off_t off, const struct iovec *iov, size_t iov_cnt);
    void copy_to_iovec(off_t off, const struct iovec *iov, size_t iov_cnt);

    __attribute__((warn_unused_result))
    std::tuple<void *, size_t>
    reset(void) {
        void *p = nullptr;
        size_t s;
        std::swap(ptr_m, p);
        std::swap(size_m, s);
        return std::make_tuple(p, s);
    }
};

// IoVec wrapper tagged with Ptr
//  used to get some type safety for p{read,write}v_native() functions
template<typename Ptr>
struct IoVec {
	struct iovec   *iov_m;
	int             iov_cnt_m;

	IoVec(struct iovec *iov, size_t iov_cnt) : iov_m(iov), iov_cnt_m(iov_cnt)
	{};
};

// The idea is to define allocators for different IO subsystems.
// Below are two commonly used allocators that also illustrate the expected
// interface

// Allocate IO buffers using malloc
struct IoBuffMalloc {
	// Each allocator defines its own IoPtr type.
	// For an allocator A, this should normally be IoPtr<A>
	using Ptr = IoPtr<IoBuffMalloc>;

	// allocate / free an IO buffer.
	static Ptr alloc_iobuff(size_t size) { return Ptr(std::malloc(size), size); }
	static void free_iobuff(Ptr ptr)     { free(ptr.ptr_m); }
};

// Allocate aligned buffers (e.g., for direct IO), based in alignment parameter
template<size_t Align>
struct IoBuffMemalign {
	static constexpr size_t ALIGNMENT = Align;
	using Ptr = IoPtr<IoBuffMemalign<Align>>;

	static Ptr alloc_iobuff(size_t size) {
		void *ptr;
		int err;
		err = posix_memalign(&ptr, ALIGNMENT, size);
		return err ? Ptr(nullptr, 0) : Ptr(ptr, size);
	}

	static void free_iobuff(Ptr ptr) {
		free(ptr.ptr_m);
		ptr.ptr_m = nullptr;
		ptr.size_m = 0;
	}
};

// Helper functions

template<typename T>
void IoPtr<T>::copy_from_iovec(off_t off, const struct iovec *iov, size_t iov_cnt) {
    // TODO: bound checks
    char *p = off + (char *)ptr_m;
    for (unsigned i=0; i<iov_cnt; i++) {
        memcpy(p, iov[i].iov_base, iov[i].iov_len);
        p += iov[i].iov_len;
    }
}

template<typename T>
void IoPtr<T>::copy_to_iovec(off_t off, const struct iovec *iov, size_t iov_cnt) {
    // TODO: bound checks
    char *p = off + (char *)ptr_m;
    for (unsigned i=0; i<iov_cnt; i++) {
        memcpy(iov[i].iov_base, p, iov[i].iov_len);
        p += iov[i].iov_len;
    }
}

#endif // _IO_PTR_H__
