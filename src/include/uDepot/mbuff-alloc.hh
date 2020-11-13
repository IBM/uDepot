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

#ifndef _MBUFF_ALLOC_HH__
#define _MBUFF_ALLOC_HH__

#include <typeindex>

#include "uDepot/mbuff.hh"

namespace udepot {

// Methods for allocating appropriate (e.g., properly aligned) Mbuffs
class MbuffAllocIface {
public:
	// get type index for mbuffs
	virtual std::type_index mbuff_type_index() = 0;
	// Allocate a new Mbuff that can hold @s bytes.
	//   User needs to check Mbuff.get_free_size()
	virtual Mbuff mbuff_alloc(size_t s) = 0;
	// Free all heap-alocated memory of an mbuff
	virtual void mbuff_free_buffers(Mbuff &mb) = 0;

	// Add buffers to an Mbuff to have (at least) @s bytes of total free space
	//
	// Caller needs to check Mbuff.get_free_size() for the success of the
	// operation;
	//
	// If the mbuff already has enough space, nothing happens.
	virtual void mbuff_add_buffers(Mbuff &mb, size_t s) = 0;

	virtual ~MbuffAllocIface() {};
};

// Implement an MbuffAllocIface using IO
//
// IO should have:
//  IO::Ptr (for generating the Mbuffs type_index
//  IO::alloc_iobuff and IO::free_iobuff
template<typename IO>
class MbuffAllocIO : public virtual MbuffAllocIface {
public:
	virtual std::type_index mbuff_type_index(void) override final {
		return std::type_index(typeid(typename IO::Ptr));
	}

	virtual Mbuff mbuff_alloc(size_t size) override final {
		Mbuff mb(mbuff_type_index());
		mbuff_add_buffers(mb, size);
		return mb;
	}

	virtual void mbuff_add_buffers(Mbuff &mb, size_t new_free_size) override final {
		size_t free_size = mb.get_free_size();
		if (free_size >= new_free_size)
			return;

		UDEPOT_DBG("allocating mbuff iobuff.");
		// allign the allocation at 4K, so that we don't allocate in small chunks
		size_t b_len = align_up(new_free_size - free_size, 4096);
		auto ioptr = IO::alloc_iobuff(b_len);
		if (ioptr.ptr_m != nullptr)
			mb.add_iobuff(ioptr, b_len);
		return;
	}

	virtual void mbuff_free_buffers(Mbuff &mb) override final {
		mb.reset(
			[] (void *ptr) { IO::free_iobuff(typename IO::Ptr(ptr, 0)); }
		);
	}
};


} // end namespace udepot

#endif /* _MBUFF_ALLOC_HH__ */
