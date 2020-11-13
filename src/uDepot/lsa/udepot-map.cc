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

#include "frontends/usalsa++/SalsaCtlr.hh"

#include <cerrno>
#include <cstring>

#include "util/types.h"
#include "util/debug.h"

#include "uDepot/lsa/udepot-map.hh"
#include "uDepot/lsa/hash-entry.hh"
#include "uDepot/backend.hh"

namespace udepot {

template<typename RT>
uDepotMap<RT>::uDepotMap()
    : entry_nr_m(0),
      ht_m(nullptr),
      size_m(0),
      entry_per_lock_nr_m(0),
      lock_nr_m(_UDEPOT_HOP_SCOTCH_LOCK_NR),
      entry_nr_bits_m(0)
{ for (auto &i : used_entry_nr_m) i = 0; }

template<typename RT>
uDepotMap<RT>::~uDepotMap()
{
	size_m = 0;
	ht_m = nullptr;
	printStats();
}

template<typename RT>
void
uDepotMap<RT>::printStats()
{
        u64 used_entry_nr = 0;
        for (auto &i: used_entry_nr_m)
                used_entry_nr += i;
	double fill_factor = 0 == entry_nr_m ? 0.0 : (double) used_entry_nr / entry_nr_m;
	UDEPOT_MSG("tot-entries=%lu used-entries=%lu fill-factor=%lf",
		entry_nr_m, used_entry_nr, fill_factor);
}

template<typename RT>
int
uDepotMap<RT>::restore(u32 lock_nr, u64 size, void *const ht)
{
	u64 size_aligned = align_down(size, sizeof(HashEntry));
	u64 entry_nr = size_aligned / sizeof(HashEntry);
	if (0 == size_aligned || nullptr == ht || entry_nr < _UDEPOT_HOP_SCOTCH_BUCKET_SIZE)
		return EINVAL;

	entry_nr = round_down_pow2(entry_nr - _UDEPOT_HOP_SCOTCH_BUCKET_SIZE) + _UDEPOT_HOP_SCOTCH_BUCKET_SIZE;
	assert(is_power_of_two(entry_nr - _UDEPOT_HOP_SCOTCH_BUCKET_SIZE));
	size_m = entry_nr * sizeof(HashEntry);
	entry_nr_bits_m = NUM_BITS(entry_nr - _UDEPOT_HOP_SCOTCH_BUCKET_SIZE - 1);
	assert((1UL << entry_nr_bits_m) == (entry_nr - _UDEPOT_HOP_SCOTCH_BUCKET_SIZE));

	entry_nr_m = entry_nr;
	ht_m = reinterpret_cast<HashEntry *>(ht);
	do {
		lock_nr_m = lock_nr;
		if (1 == lock_nr_m) {
			entry_per_lock_nr_m = entry_nr_m;
			break;
		}
		entry_per_lock_nr_m = udiv_round_up(entry_nr_m, lock_nr_m);
		if ((_UDEPOT_HOP_SCOTCH_BUCKET_SIZE * (1 + _UDEPOT_HOP_MAX_BUCKET_DISPLACE)) <= entry_per_lock_nr_m)
			break;
		lock_nr /= 2;
	} while (1);
	UDEPOT_DBG("ht entry-nr-pow2=%lu out of entry-nr-max=%lu round_down_pow2=%lu lock_nr=%u",
		1UL << entry_nr_bits_m, size_aligned / sizeof(HashEntry),
                round_down_pow2(size_aligned/sizeof(HashEntry)), lock_nr_m);
	return 0;
}

template<typename RT>
int
uDepotMap<RT>::init(const u32 lock_nr, u64 size, void *const ht)
{
	const int rc = restore(lock_nr, size, ht);
	if (0 != rc)
		return rc;
	UDEPOT_DBG("initializing hash table size=%luKiB", size_m >> 10);
	memset(ht, (-1) , size);
	#if	0
	HashEntry *const first = &ht_m[0];
	HashEntry *const last  = &ht_m[entry_nr_m];
	for (HashEntry *p = first; p < last; ++p) {
                assert(!p->used() && !p->deleted());
	}
	#endif
#ifdef DEBUG
        u64 sum = 0;
        for (u32 i = 0; i < lock_nr_m; ++i) {
                u64 start, end;
                // std::tie(start, end) = udm->hash_to_lock_range(h);
                std::tie(start, end) = locks_to_grow_range(&locks_m[i], nullptr);
                sum += end - start;
        }
        assert(sum == entry_nr_m);
#endif
	return 0;
}

// instantiate the templates
template class uDepotMap<RuntimePosix>;
template class uDepotMap<RuntimePosixODirect>;
template class uDepotMap<RuntimePosixODirectMC>;
template class uDepotMap<RuntimeTrt>;
template class uDepotMap<RuntimeTrtMC>;
#if defined(UDEPOT_TRT_SPDK)
template class uDepotMap<RuntimeTrtSpdk>;
template class uDepotMap<RuntimeTrtSpdkArray>;
template class uDepotMap<RuntimePosixSpdk>;
template class uDepotMap<RuntimeTrtSpdkArrayMC>;
#endif

} // namespace udepot
