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

#ifndef	_UDEPOT_SALSA_MAP_H_
#define	_UDEPOT_SALSA_MAP_H_

#include <cerrno>
#include <cassert>
#include <atomic>
#include <memory> //shared_ptr
#include <pthread.h>

#include "util/types.h"
#include "util/debug.h"
#include "uDepot/lsa/hash-entry.hh"
#include "uDepot/sync.hh"

namespace udepot {

template<typename RT>
class uDepotDirectoryMap;

#define	_UDEPOT_HOP_SCOTCH_LOCK_NR	(8192UL)
#define	_UDEPOT_HOP_MAX_BUCKET_DISPLACE	(64UL)


template<typename RT>
class uDepotMap {

	template<typename> friend class uDepotDirectoryMap;
	template<typename> friend class uDepotDirMapOR;

public:
	uDepotMap();
	~uDepotMap();

 	int init(u32 lock_nr, u64 size, void *ht);
 	int restore(u32 lock_nr, u64 size, void *ht);
	/*
	 * 0: HashEntry matching key found, reference returned in @he_out
	 * ENODATA: no matching HashEntry found, reference for the HashEntry of the data to be added in @he_out
	 * ENOSPC: no matching HashEntry found, and ht does not have a slot to put key
	 */
	__attribute__((optimize("unroll-loops")))
	int lookup(const u64 h, HashEntry **const he_out) const {
		const u64 hb = hash_to_hb_(h);
                // not enough just to check the tag, the offset has to
                // match also, the bucket has to be the same for a match
		const u64 to_match = hash_to_keyfp_(h);
		HashEntry *const first = &ht_m[hb];
		HashEntry *const last  = first + _UDEPOT_HOP_SCOTCH_BUCKET_SIZE;
		HashEntry *candidate = nullptr;
		for (HashEntry *p = first; p < last; ++p) {
			// if (!p->used() || p->deleted())
			// 	continue;
			if (!p->used())
				break;
			if (p->used() && !p->deleted() && to_match == entry_keyfp(p)) {
				*he_out = p;
				return 0; // matching entry found
			}
		}

		for (HashEntry *p = first; p < last; ++p) {
			if (!p->used()) {
				candidate = p;
				break;
			}
			else if (p->deleted())
				candidate = p; // candidate is the last deleted
		}
		return nullptr == candidate ?
			ENOSPC:
			(*he_out = candidate, ENODATA);
	}

	// 0: free HashEntry found, reference returned in @he_out
	// ENOSPC: no matching HashEntry found, and ht does not have a slot to put key
	int next_free(const u64 h, HashEntry **const he_out) const {
		const u32 hb = hash_to_hb_(h);
		HashEntry *const first = &ht_m[hb];
		HashEntry *const last  = first + _UDEPOT_HOP_SCOTCH_BUCKET_SIZE;
		for (HashEntry *p = first; p < last; ++p)
			if (!p->used() || p->deleted()) {
				*he_out = p;
				return 0;
			}
		return ENOSPC;
	}

	// next HashEntry with the same data, if any
	HashEntry *next(HashEntry *const he, u64 h) const {
		if (nullptr == he)
			return nullptr;
		const u64 hb = hash_to_hb_(h);
		const u64 to_match = hash_to_keyfp_(h);
		HashEntry *const first = &ht_m[hb];
		HashEntry *const last  = first + _UDEPOT_HOP_SCOTCH_BUCKET_SIZE;
		for (HashEntry *p = he + 1; p < last; ++p) {
			if (!p->used() || p->deleted())
				continue;
			else if (!p->deleted() && to_match == entry_keyfp(p))
				return p; // matching entry found
		}
		return nullptr;
	}

	// lookup for GC
	__attribute__((optimize("unroll-loops")))
	HashEntry *lookup(const u64 h, const u64 pba) const {
		const u64 hb = hash_to_hb_(h);
		const u64 to_match __attribute__((unused)) = hash_to_keyfp_(h);
		HashEntry *const first = &ht_m[hb];
		HashEntry *const last  = first + _UDEPOT_HOP_SCOTCH_BUCKET_SIZE;
		for (HashEntry *p = first; p < last; ++p) {
			if (!p->used())
				continue;
			else if (pba == p->pba) {
				assert(to_match == entry_keyfp(p));
				return p; // matching entry found
			}
		}
		return nullptr;
	}

	int try_shift(u64 h, HashEntry **const he_out) {
		const u32 hb = hash_to_hb_(h);
		HashEntry *const first = &ht_m[hb];
                #if	1
		HashEntry *const last  = std::min(first + _UDEPOT_HOP_SCOTCH_BUCKET_SIZE * _UDEPOT_HOP_MAX_BUCKET_DISPLACE,
                                                &ht_m[entry_nr_m - _UDEPOT_HOP_SCOTCH_BUCKET_SIZE - 1UL]);
		HashEntry *p_free = nullptr;
		for (HashEntry *p = first + _UDEPOT_HOP_SCOTCH_BUCKET_SIZE; p < last; ++p) {
			// find first empty
			if (!!(!p->used()) ^ !!(p->deleted())) {
				p_free = p;
				break;
			}
		}
		if (nullptr == p_free)
			return ENOSPC;
		// start displacing backwards until we
		// are within bucket size of original
		// entry
		HashEntry *p = p_free - _UDEPOT_HOP_SCOTCH_BUCKET_SIZE + 1;
		while (first <= p && p < p_free) {
			if (0 == try_shift_one(p)) {
				if ((uintptr_t)(p - first) < _UDEPOT_HOP_SCOTCH_BUCKET_SIZE) {
					assert(!!(!p->used()) ^ !!(p->deleted()));
					*he_out = p;
					return 0;
				} else {
					// displaced entry still far away, keep going backwards
					// p is new p_free
					p_free = p;
					p = std::max(first, p_free - _UDEPOT_HOP_SCOTCH_BUCKET_SIZE + 1);
					continue;
				}
			}
			p++;
		}
                #else
                HashEntry *const last  = first + _UDEPOT_HOP_SCOTCH_BUCKET_SIZE;
                for (HashEntry *p = first; p < last; ++p) {
                        assert(p->used()); // otherwise why try to shift values
                        if (0 == try_shift_one(p)) {
                                // moved to another location and space was made at its place
                                assert(!!(!p->used()) ^ !!(p->deleted()));
                                *he_out = p;
                                return 0;
                        }
                }
                #endif
		return ENOSPC;
	}

	// @he a HashEntry returned through a lookup
	void insert(HashEntry *const he, const u64 h, const size_t kv_size, const u64 pba) {
		update(he, h, kv_size, pba);
		used_entry_nr_m[hash_to_hb_(h) % used_entry_nr_m.size()]++;
	}

	void update(HashEntry *const he, const u64 h, const size_t kv_size, const u64 pba) {
		assert(he && (he->deleted() ^ !he->used()));
		const u64 keytag = hash_to_keytag_(h);
		const u64 bucket_offset = he - &ht_m[hash_to_hb_(h)];
		assert(&ht_m[hash_to_hb_(h) ]<= he);
		he->bucket_offset = bucket_offset;
		he->key_tag = keytag;
		he->kv_size = std::min(kv_size, UDEPOT_SALSA_MAX_KV_SIZE);
		he->pba = pba;
	}

	void update_inplace(HashEntry *const he, const u64 h, const size_t kv_size, const u64 pba) {
		assert(he && (!he->deleted() && he->used()));
		const u64 keytag = hash_to_keytag_(h);
		he->key_tag = keytag;
		he->kv_size = std::min(kv_size, UDEPOT_SALSA_MAX_KV_SIZE);
		he->pba = pba;
		assert(he->bucket_offset == he - &ht_m[hash_to_hb_(h)]);
	}

	// @he a HashEntry returned through a lookup
	void remove(HashEntry *const he, const u64 pba) {
		assert(he && !he->deleted() && he->used());
		// keep the key so that it can be copied over when we grow
		he->pba = pba;
		he->set_deleted();
		const u64 old_val __attribute__((unused)) = used_entry_nr_m[hash_to_hb_(entry_keyfp(he)) % used_entry_nr_m.size()]--;
		assert(0 <= old_val);
	}

	void purge(HashEntry *const he) {
		assert(he && he->deleted());
		// keep the key so that it can be copied over when we grow
		he->purge();
	}

	void lock(const u64 h) {
		uDepotLock *sl1, *sl2;
		lock_common(h, &sl1, &sl2);
		sl1->lock();
		if (nullptr != sl2)
			sl2->lock();
	}
	void unlock(const u64 h) {
		uDepotLock *sl1, *sl2;
		lock_common(h, &sl1, &sl2);
		if (nullptr != sl2)
			sl2->unlock();
		sl1->unlock();
	}
	void lock(uDepotLock *const sl1, uDepotLock *const sl2) {
		sl1->lock();
		if (nullptr != sl2)
			sl2->lock();
	}
	void unlock(uDepotLock *const sl1, uDepotLock *const sl2) {
		if (nullptr != sl2)
			sl2->unlock();
		sl1->unlock();
	}
	void printStats();
	void lock_common(const u64 h, uDepotLock **const sl1_out, uDepotLock **const sl2_out) {
		const u64 first = hash_to_hb_(h);
		const u64 last  = std::min(first + _UDEPOT_HOP_SCOTCH_BUCKET_SIZE * (_UDEPOT_HOP_MAX_BUCKET_DISPLACE + 1),
                                                entry_nr_m - _UDEPOT_HOP_SCOTCH_BUCKET_SIZE  - 1UL);
		const u64 slb = _UDEPOT_HOP_SCOTCH_BUCKET_SIZE <= first ?
			(first - _UDEPOT_HOP_SCOTCH_BUCKET_SIZE) / entry_per_lock_nr_m:
			first / entry_per_lock_nr_m;
		const u64 first_slb __attribute__((unused)) = slb * entry_per_lock_nr_m;
		const u64 last_slb  = (slb + 1) * entry_per_lock_nr_m;
		assert(first_slb <= first);
		typename RT::LockTy *const sl1  = &locks_m[slb], *sl2 = nullptr;
		// if hopscotch bucket spans two lock buckets then we'll have to take two locks, in order
		if (last_slb <= last) {
			assert(slb < lock_nr_m);
			sl2 =  &locks_m[slb + 1];
			assert(last <= (slb + 2) * entry_per_lock_nr_m);
		}
		*sl1_out = sl1;
		*sl2_out = sl2;
	}

// protected:
	u64 entry_nr_m;
	HashEntry *ht_m;
        void entry_restored(const u64 h) { used_entry_nr_m[hash_to_hb_(h) % used_entry_nr_m.size()]++; }
	void copy_for_grow(const HashEntry *const src, HashEntry *trgt) {
		assert(!trgt->used());
		*trgt = *src;
                used_entry_nr_m[hash_to_hb_(entry_keyfp(src)) % used_entry_nr_m.size()]++;
	}

	__attribute__((always_inline))
	u64 entry_keyfp(const HashEntry *const p) const {
		const u64 offset = (p - p->bucket_offset) - &ht_m[0]; // entry_nr_bits_m
		const u64 key_tag = p->key_tag;
		assert((u64)NUM_BITS(offset) <= entry_nr_bits_m);
		return ((key_tag << entry_nr_bits_m) | offset);
	}
protected:
        std::tuple<u64, u64> hash_to_lock_range(const u64 h) const {
		const u64 first = hash_to_hb_(h);
		const u64 last  = std::min(first + _UDEPOT_HOP_SCOTCH_BUCKET_SIZE * (_UDEPOT_HOP_MAX_BUCKET_DISPLACE + 1),
                                        entry_nr_m);
		const u64 slb = _UDEPOT_HOP_SCOTCH_BUCKET_SIZE <= first ?
			(first - _UDEPOT_HOP_SCOTCH_BUCKET_SIZE) / entry_per_lock_nr_m:
			first / entry_per_lock_nr_m;
		const u64 first_slb __attribute__((unused)) = slb * entry_per_lock_nr_m;
		u64 last_slb  = (slb + 1) * entry_per_lock_nr_m;
		if (last_slb <= last) {
                        last_slb = (slb + 2) * entry_per_lock_nr_m;
			assert(last <= last_slb);
		}
                last_slb = std::min(last_slb, entry_nr_m);
                return std::make_tuple(first_slb, last_slb);
        }

        std::tuple<u64, u64> locks_to_grow_range(const uDepotLock *sl1, const uDepotLock *sl2) const {
                const u64 slb = (typename RT::LockTy *) sl1 - &locks_m[0];
                u64 first2, first1 = first2 = slb * entry_per_lock_nr_m;
                u64 last2, last1 = last2 = std::min((slb + 1) * entry_per_lock_nr_m, entry_nr_m);
                if (0 == sl1->ref_cnt()) // no grow on 1st lock
                        first1 = last1;
                if (nullptr != sl2) {
                        first2 = (slb + 1) * entry_per_lock_nr_m;
                        last2 = std::min((slb + 2) * entry_per_lock_nr_m, entry_nr_m);
                        if (0 == sl2->ref_cnt()) // no grow on 2nd lock
                                last2 = first2;
                }
                return std::make_tuple(first1, last2);
        }

private:
	u64 size_m;
	u64 entry_per_lock_nr_m;
	u32 lock_nr_m;
	u32 entry_nr_bits_m;
	// u8 pad[64];
	std::array<u64, _UDEPOT_HOP_SCOTCH_LOCK_NR> used_entry_nr_m;
	typename RT::LockTy locks_m[_UDEPOT_HOP_SCOTCH_LOCK_NR + 1];
	uDepotMap(const uDepotMap &)            = delete;
	uDepotMap(const uDepotMap &&)           = delete;
	uDepotMap& operator=(const uDepotMap &) = delete;

        u64 hash_to_keytag_(const u64 h) const {
		return (h >> (entry_nr_bits_m)) & (UDEPOT_SALSA_KEYTAG_BITMASK);
        }

	u64 hash_to_keyfp_(const u64 h) const {
                return h & ((1UL << (UDEPOT_SALSA_KEYTAG_BITS + entry_nr_bits_m)) - 1UL);
        }

	u64 hash_to_hb_(const u64 h) const {
                // entry_nr_m - _UDEPOT_HOP_SCOTCH_LOCK_NR is a power of two, look at init()
		return (h) & (entry_nr_m - _UDEPOT_HOP_SCOTCH_BUCKET_SIZE - 1);
	}

	int try_shift_one(HashEntry *const candidate) {
		assert(candidate->used());
		HashEntry *victim = nullptr;
		int rc = next_free(entry_keyfp(candidate), &victim);
		if (0 != rc)
			return rc;
		// swap them
                const u64 hb = hash_to_hb_(entry_keyfp(candidate));
		HashEntry tmp = *candidate;
		assert(candidate->used() && !victim->deleted());
		assert(!!(!victim->used()) ^ !!(victim->deleted()));
		*candidate = *victim;
		*victim = tmp;
                assert(&ht_m[hb] <= victim);
		const u64 bucket_offset = victim - &ht_m[hb];
                victim->bucket_offset = bucket_offset;
                assert(victim->bucket_offset < _UDEPOT_HOP_SCOTCH_BUCKET_SIZE);
                if (candidate->used()) {
                        const u64 hb = hash_to_hb_(entry_keyfp(candidate));
                        assert(candidate->deleted());
                        assert(&ht_m[hb] <= candidate);
                        const u64 bucket_offset = candidate - &ht_m[hb];
                        candidate->bucket_offset = bucket_offset;
                }
		assert(victim == lookup(entry_keyfp(victim), victim->pba));
		return 0;
	}

	void prefetch_bucket(const u8 *b) const {
		b = (const u8*) ((uintptr_t) b & 63);
		constexpr int prf_nr = _UDEPOT_HOP_SCOTCH_BUCKET_SIZE * sizeof(HashEntry) / 64;
		for (int i = 0; i < prf_nr / 4; ++i) {
			__builtin_prefetch(b);
			__builtin_prefetch(b + 64);
			__builtin_prefetch(b + 128);
			__builtin_prefetch(b + 196);
			b += 256;
		}
	}
};


}; // namespace udepot
#endif	// _UDEPOT_SALSA_MAP_H_
