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

#ifndef	_UDEPOT_SALSA_DIR_MAP_OR_H_
#define _UDEPOT_SALSA_DIR_MAP_OR_H_

#include <atomic>
#include <cassert>
#include <cerrno>
#include <pthread.h>
#include <sys/mman.h>
#include <vector>

#include "util/types.h"
#include "util/debug.h"
#include "uDepot/rwlock-pagefault.hh"
#include "uDepot/sync.hh"
#include "uDepot/lsa/udepot-map.hh"
#include "uDepot/lsa/hash-entry.hh"
#include "uDepot/lsa/metadata.hh"
#include "uDepot/lsa/type.hh"

namespace udepot {

class uDepotIO;

template<typename RT>
class uDepotSalsa;


template<typename RT>
// with Fast Online Resizing
class uDepotDirMapOR {
public:
	uDepotDirMapOR();
	~uDepotDirMapOR();

	int init(u64, u64);
	int shutdown();
	/*
	 * 0: Hash Table grown successfully
	 * ENOSPC: Hash Table grow failed
	 */
	int grow(u64, u64);
        int alloc_shadow(u64, u64);
        int grow_finished(u64, u64);
	/*
	 * 0: HashEntry matching key found, reference returned in @he_out
	 * ENODATA: no matching HashEntry found, reference for the HashEntry of the data to be added in @he_out
	 * ENOSPC: no matching HashEntry found, and ht does not have a slot to put key
	 */
	int lookup(const u64 h, HashEntry **const he_out) {
		return hash_to_map(h)->lookup(h, he_out);
	}
	// 0: free HashEntry found, reference returned in @he_out
	// ENOSPC: no matching HashEntry found, and ht does not have a slot to put key
	int next_free(const u64 h, HashEntry **const he_out) {
		return hash_to_map(h)->next_free(h, he_out);
	}
	// next HashEntry with the same data, if any
	HashEntry *next(HashEntry *const he, const u64 h) {
		return hash_to_map(h)->next(he, h);
	}
	HashEntry *lookup(const u64 h, const u64 pba) {
		return hash_to_map(h)->lookup(h, pba);
	}
	// @he a HashEntry returned through a lookup
	void insert(const u64 h, HashEntry *const he, const size_t kv_size, const u64 pba) {
		hash_to_map(h)->insert(he, h, kv_size, pba);
	}
	void update(const u64 h, HashEntry *const he, const size_t kv_size, const u64 pba) {
		hash_to_map(h)->update(he, h, kv_size, pba);
	}
	void update_inplace(const u64 h, HashEntry *const he, const size_t kv_size, const u64 pba) {
		hash_to_map(h)->update_inplace(he, h, kv_size, pba);
	}
	// 0 if space was made
	int try_shift(u64 h, HashEntry **const he_out) {
		return hash_to_map(h)->try_shift(h, he_out);
	}

	// @he a HashEntry returned through a lookup
	void remove(const u64 h, HashEntry *const he, const u64 pba) {
		hash_to_map(h)->remove(he, pba);
	}

	void purge(const u64 h, HashEntry *const he) {
		hash_to_map(h)->purge(he);
	}

	class DirMapEntry {
	public:
		DirMapEntry() : mm_region(MAP_FAILED), grain_offset(0), size_b(0) {}
		uDepotMap<RT> map;
		void     *mm_region;
		u64       grain_offset;
		u64       size_b;
	};

	class DirMapRef {
	public:
		DirMapRef() : directory(nullptr) {};
		typename RT::RwpfTy                     rwpflock;
                std::atomic<u64>                        state;
                u8 padding[64];
		std::atomic<std::vector<DirMapEntry> *> directory;
		std::atomic<std::vector<DirMapEntry> *> shadow_directory;
	};
	DirMapRef dir_ref_m;

	uDepotMap<RT> *hash_to_map(const u64 h) {
		std::vector<DirMapEntry> *const directory = dir_ref_m.directory.load(std::memory_order_relaxed);
		assert(nullptr != directory);
		return &((*directory)[h % directory->size()]).map;
	}
	uDepotMap<RT> *hash_to_shadow_map(const u64 h) {
		std::vector<DirMapEntry> *const directory = dir_ref_m.shadow_directory.load(std::memory_order_relaxed);
		assert(nullptr != directory);
		return &((*directory)[h % directory->size()]).map;
	}

        int cpy_lock_region(uDepotMap<RT> *udm, uDepotLock *l1, uDepotLock *l2, u64 *rem_out);
private:
	u32                   grow_nr_m;
	typename RT::LockTy   grow_lock_m;
        u64 lock_range_cpy_finished() { const u64 old_val = dir_ref_m.state.fetch_sub(1); assert(0 < old_val); return old_val - 1U; }
	uDepotDirMapOR(const uDepotDirMapOR &)            = delete;
	uDepotDirMapOR(const uDepotDirMapOR &&)           = delete;
	uDepotDirMapOR& operator=(const uDepotDirMapOR &) = delete;
};

}; // namespace udepot
#endif	// _UDEPOT_SALSA_DIR_MAP_OR_H_
