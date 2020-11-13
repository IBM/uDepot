/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop:4 shiftwidth:4:

#ifndef RTE_HH__
#define RTE_HH__

#include <rte_config.h>
#include <rte_malloc.h>

#include "trt_util/io_ptr.hh"

// code for the DPDK run-time system

// An allocator for constructing IO pointers for DPDK and SPDK.
//
// NB #1: the idea is to use one type for both DPDK and SPDK, so that we can
// build a zero-copy server. SPDK provides its own allocation function, which
// seems to be an attempt to decouple it from DPDK and its enviroment. The
// common implementation just ends up calling rte_malloc_socket() (see:
// spdk_dma_malloc() in lib/env_dpdk/env.c)
//
// NB #2: SOCKET_ID_ANY is a bit misleading. Looking at the code, it will
// allocate it locally (i.e., in the local NUMA node) by default. So we don't
// have to do anything smart here to get local allocation
template<size_t Align>
struct RteAlloc {
	static constexpr size_t ALIGNMENT = Align;

	using Ptr = IoPtr<RteAlloc<Align>>;
	static Ptr alloc_iobuff(size_t size) {
		void *ptr = rte_malloc_socket("", size, ALIGNMENT, SOCKET_ID_ANY);
		return Ptr(ptr, size);
	}

	static void free_iobuff(Ptr p) {
		rte_free(p.ptr_m);
		p.ptr_m = nullptr;
		p.size_m = 0;
	}
};

template<size_t A>
using RtePtr_ = typename RteAlloc<A>::Ptr;

#endif // RTE_HH__
