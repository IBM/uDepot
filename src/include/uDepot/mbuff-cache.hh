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

#ifndef MBUFF_CACHE_HH_
#define MBUFF_CACHE_HH_

#include <deque>
#include <array>
#include <typeindex>

#include "uDepot/mbuff.hh"
#include "uDepot/mbuff-alloc.hh"
#include "uDepot/thread-id.hh"

namespace udepot {

class MbuffCacheBase {
public:
	virtual Mbuff *mb_get(void) = 0;
	virtual void mb_put(Mbuff *mb) = 0;
};

template<typename LockTy, size_t N = 128>
class MbuffCache : public MbuffCacheBase {
public:
	MbuffCache()                       = delete;
	MbuffCache(MbuffCache const&)      = delete;
	void operator=(MbuffCache const &) = delete;

	// NB: mb_alloc is strictly not needed because we can get everything from
	// RT::IO, since this is how we implement the MbuffAlloc interface. We do it
	// this way because in theory, we could have different Mbuffs for NET
	// buffers. (FWIW, I think the best approach is to have a common buffer that
	// always works for both IO and NET so that we don't complicate things too
	// much. This could work by having RT::{alloc,free}_iobuff() that provide
	// proper buffers for both IO and NET.) Once we figure out how to combine
	// NET and IO buffers, we can revisit this.
	MbuffCache(MbuffAllocIface &mb_alloc) : mb_alloc_m(mb_alloc) {}

	// Get an mbuff from the cache
	// If cache is empty, an Mbuff will be allocated
	// If allocation fails, nullptr is returned
	Mbuff *mb_get(void) {
		unsigned tid = ThreadId::get();
		Mbuff *ret;
		LockTy *lock;
		std::deque<Mbuff *> *cache;

		if (tid < mbuff_thread_caches_m.size()) {
			// private cache, no need to lock
			cache = &mbuff_thread_caches_m[tid];
			lock = nullptr;
		} else {
			// shared cache
			cache = &mbuff_shared_cache_m;
			lock = &mbuff_shared_cache_lock_m;
		}

		if (lock)
			lock->lock();

		// if nothing in cache try to allocate
		if (cache->size() == 0) {
			ret = mb_alloc();
		} else {
			ret = cache->front();
			assert(ret);
			cache->pop_front();
		}

		if (lock)
			lock->unlock();

		return ret;
	}

	// put an mbuff back to the cache
	//  This will call ->reslice(0) on the Mbuff.
	void mb_put(Mbuff *mb) {
		unsigned tid = ThreadId::get();
		LockTy *lock;
		std::deque<Mbuff *> *cache;

		mb->reslice(0);

		if (tid < mbuff_thread_caches_m.size()) {
			// private cache, no need to lock
			cache = &mbuff_thread_caches_m[tid];
			lock = nullptr;
		} else {
			// shared cache
			cache = &mbuff_shared_cache_m;
			lock = &mbuff_shared_cache_lock_m;
		}

		if (lock)
			lock->lock();

		try {
			cache->push_front(mb);
		} catch (...) {
			// not much to do here... the buffer will not be usable, but it will be
			// deallocated when the uDepotSalsa instance is destroyed
		}

		if (lock)
			lock->unlock();
	}

	virtual ~MbuffCache() {
		for (size_t i=0; i < mbuff_cache_m.size(); i++) {
			Mbuff *mb = &mbuff_cache_m[i];
			mb_alloc_m.mbuff_free_buffers(*mb);
		}
	}

private:
	// Allocate an mbuff that can be used for caching.
	// The buffers in the mbuff will are freed in the destructor.
	Mbuff *mb_alloc(void) {
		Mbuff *ret;
		mbuff_cache_lock_m.lock();
		try {
			std::type_index tyidx = mb_alloc_m.mbuff_type_index();
			mbuff_cache_m.emplace_back(tyidx);
			ret = &mbuff_cache_m.back();
		} catch (...) {
			ret = nullptr;
		}
		mbuff_cache_lock_m.unlock();
		return ret;
	}

private:
	// keep the mbuffs we cache elsewhere (typically in TLS pointers) here so
	// that we can free their buffers when we are done.
	std::deque<Mbuff>                  mbuff_cache_m;
	LockTy                mbuff_cache_lock_m;

	std::array<std::deque<Mbuff *>, N> mbuff_thread_caches_m;
	std::deque<Mbuff *>                mbuff_shared_cache_m;
	LockTy                             mbuff_shared_cache_lock_m;

	MbuffAllocIface                   &mb_alloc_m;
};

} // end namespace udepot

#endif /* ifndef MBUFF_CACHE_HH_ */
