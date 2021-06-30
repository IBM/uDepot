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

#ifndef	_UDEPOT_SALSA_DIRECTORY_MAP_H_
#define	_UDEPOT_SALSA_DIRECTORY_MAP_H_

#include "frontends/usalsa++/SalsaCtlr.hh"

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
class uDepotDirectoryMap: private salsa::SalsaCtlr {
public:
	uDepotDirectoryMap(uDepotSalsaMD<RT> &md, typename RT::IO &udepot_io);
	~uDepotDirectoryMap();

	int gc_callback(u64 grain_start, u64 grain_nr) final;
	void seg_md_callback(u64 grain_start, u64 grain_nr) final;
	int init(salsa::Scm *scm, uDepotSalsa<RT> *udpt);
	int shutdown(uDepotSalsa<RT> *udpt);
	/*
	 * 0: Hash Table grown successfully
	 * ENOSPC: Hash Table grow failed
	 */
	int grow();
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

	struct DirMapEntry {
		DirMapEntry() : mm_region(MAP_FAILED), grain_offset(0), size_b(0), huge(false) {}
		uDepotMap<RT> map;
		void *mm_region;
		u64   grain_offset;
		u64   size_b;
		bool  huge;
	};

	struct DirMapRef {
		DirMapRef() : directory(nullptr) {};
		typename RT::RwpfTy                     rwpflock;
		std::atomic<std::vector<DirMapEntry> *> directory;
	};
	DirMapRef dir_ref_m;

	u64 hash_to_dir_idx_(const u64 h, const u32 dir_idx_bits) const {
		assert(dir_idx_bits < UDEPOT_SALSA_KEYTAG_BITS + map_entry_nr_bits_m);
		const u64 hh = (h & ((1UL << (UDEPOT_SALSA_KEYTAG_BITS + map_entry_nr_bits_m)) - 1UL));
		return (hh) >> (UDEPOT_SALSA_KEYTAG_BITS + map_entry_nr_bits_m - dir_idx_bits);
	}

	uDepotMap<RT> *hash_to_map(const u64 h) {
		std::vector<DirMapEntry> *const directory = dir_ref_m.directory.load(std::memory_order_relaxed);
		assert(nullptr != directory);
		const u64 idx = hash_to_dir_idx_(h, grow_nr_m - 1);
		return &((*directory)[idx]).map;
	}
private:
	const uDepotSalsaType type_m;
	typename RT::IO       &udepot_io_m;
	uDepotSalsaMD<RT>     &md_m;
	u32                   grow_nr_m;
	u32                   map_entry_nr_bits_m;
	typename RT::LockTy   grow_lock_m;
	int restore(salsa::Scm *scm, uDepotSalsa<RT> *udpt);
	int prep_crash_recovery(salsa::Scm *scm, uDepotSalsa<RT> *udpt);
	int crash_recovery(salsa::Scm *scm, uDepotSalsa<RT> *udpt);
	int invalidate_ftr();
	uDepotDirectoryMap(const uDepotDirectoryMap &)            = delete;
	uDepotDirectoryMap(const uDepotDirectoryMap &&)           = delete;
	uDepotDirectoryMap& operator=(const uDepotDirectoryMap &) = delete;

};

}; // namespace udepot
#endif	// _UDEPOT_HOP_SCOTCH_LOCK_NR
