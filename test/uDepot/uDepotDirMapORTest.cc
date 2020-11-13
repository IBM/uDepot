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

#include <cassert>
#include <pthread.h> // pthread_barrier_t
#include <cstdlib>

#include "util/debug.h"
#include "util/types.h"

#include "uDepot/lsa/udepot-dir-map-or.hh"
#include "uDepot/lsa/udepot-map.hh"
#include "uDepot/lsa/hash-entry.hh"
#include "uDepot/backend.hh"
#include "util/profiling.hh"
#include "udepot-utests.hh"


#define UDEPOT_OR_PROFILING

namespace udepot {

uDepotDirMapOR<RuntimePosix> map_g;
u64 seg_size = 8193;
u64 grain_size = 4096;
pthread_barrier_t barrier_g, grow_barrier_g;

constexpr u64 prime = 2654435761UL;

__attribute__((unused))
static int uDepotDirMapORPutTest(const u64 start, const u64 item_nr)
{
	u64 nshifts = 0, ncollisions = 0;
	struct timespec before;
	clock_gettime(CLOCK_MONOTONIC, &before);
	for (u64 i = start; i < start + item_nr; ++i) {
		const u64 key = (i * prime);
		HashEntry *trgt = nullptr;
		int rc = map_g.lookup(key, &trgt);
		switch (rc) {
		case  ENOSPC:
			rc = map_g.try_shift(key, &trgt);
			assert(0 == rc);
			assert(nullptr != trgt && (trgt->deleted() ^ !trgt->used()));
			nshifts++;
		case ENODATA:
			map_g.insert(key, trgt, 1024, i);
			assert(trgt->used());
			break;
		case 0:
			do {
				ncollisions++;
				trgt = map_g.next(trgt, key);
			} while (nullptr != trgt);
			break;
		default:
			assert(0);
			break;
		}
	}
	struct timespec after;
	clock_gettime(CLOCK_MONOTONIC, &after);
	u64 ns = (1000000000UL) * (after.tv_sec - before.tv_sec) + after.tv_nsec - before.tv_nsec;
	UDEPOT_MSG("inserts/sec=%lu item_nr=%lu shift_nr=%lu collision_nr=%lu",
		item_nr * (1000000000UL) / ns ,item_nr, nshifts, ncollisions);
	return 0;
}

__attribute__((unused))
static int uDepotDirMapORGetTest(const u64 start, const u64 item_nr)
{
	struct timespec before;
	clock_gettime(CLOCK_MONOTONIC, &before);
	for (u64 i = start; i < start + item_nr; ++i) {
		const u64 key = (i * prime);
		HashEntry *trgt = map_g.lookup(key, i);
		// assert(nullptr != trgt);
		if (nullptr == trgt) {
			UDEPOT_ERR("get returned trgt=%p.", trgt);
			assert(0); // error
			return ENODATA;
		}
		// int rc __attribute__((unused)) = map_g.lookup(key, &trgt);
		// assert(0 == rc && nullptr != trgt);
	}
	struct timespec after;
	clock_gettime(CLOCK_MONOTONIC, &after);
	u64 ns = (1000000000UL) * (after.tv_sec - before.tv_sec) + after.tv_nsec - before.tv_nsec;
	UDEPOT_MSG("lookups/sec=%lu", item_nr * (1000000000UL) / ns );
	return 0;
}

__attribute__((unused))
static int uDepotDirMapORDelTest(const u64 start, const u64 item_nr)
{
	for (u64 i = start; i < start + item_nr; ++i) {
		const u64 key = (i * prime);
		HashEntry *trgt = nullptr;
		int rc __attribute__((unused)) = map_g.lookup(key, &trgt);
		assert(0 == rc && nullptr != trgt);
		map_g.remove(key, trgt, i);
		rc = map_g.lookup(key, &trgt);
		assert(ENODATA == rc && nullptr != trgt);
	}
	return 0;
}

static inline
void lock_ref_cnt(uDepotLock *const l1, uDepotLock *const l2, u32 &l1s, u32 &l2s)
{
	l1s = l1->ref_cnt();
	if (nullptr == l2)
		l2s = l1s;
	else
		l2s = l2->ref_cnt();

}

__attribute__((unused))
static int uDepotDirMapORPutTestMT(const u64 start, const u64 item_nr)
{
	u64 nshifts = 0, ncollisions = 0, grow_nr __attribute__((unused)) = 0;
	uDepotMap<RuntimePosix> *udm;
	uDepotLock *l1, *l2;
	u32 l1s = 0, l2s = 0;
	for (u64 i = start; i < start + item_nr; ++i) {

		#if defined(UDEPOT_OR_PROFILING)
		bool resize = false;
		static thread_local uint32_t probe_count = 0;
		uint64_t probe_ticks0 = ticks::get_ticks();
		#endif

		const u64 key = (i * prime);
		HashEntry *trgt = nullptr;
	again:
		udm = map_g.hash_to_map(key);
		udm->lock_common(key, &l1, &l2);
		udm->lock(l1,l2);
		lock_ref_cnt(l1, l2, l1s, l2s);
		// if one is 0 and one is 1 then one of the two has been copied and the other not
		// copy the other one as well, to simplify things
		if (l1s ^ l2s) {
			u64 rem = -1;
			int rc __attribute__((unused)) = map_g.cpy_lock_region(udm, l1, l2, &rem);
			#if defined(UDEPOT_OR_PROFILING)
			resize = true;
			#endif
			assert(0 == rc);
			lock_ref_cnt(l1, l2, l1s, l2s);
			assert(0 == l1s && 0 == l2s);
			grow_nr++;
			if (0 == rem)
				pthread_barrier_wait(&grow_barrier_g); // let grow thread know
		}
		// if growing then switch to new table
		if (0 == l1s && 0 == l1s) {
			// switch to shadow
			uDepotMap<RuntimePosix> *new_udm = map_g.hash_to_shadow_map(key);
			uDepotLock *l3, *l4;
			new_udm->lock_common(key, &l3, &l4);
			new_udm->lock(l3, l4);
			udm->unlock(l1, l2);
			udm = new_udm;
			l1 = l3; l2 = l4;
		}
		assert(0 == (l1s ^ l2s));
		int rc = udm->lookup(key, &trgt);
		switch (rc) {
		case  ENOSPC:
			rc = udm->try_shift(key, &trgt);
			// grow, special path
			if (0 != rc) {
				#if defined(UDEPOT_OR_PROFILING)
				resize = true;
				#endif
				u64 rem = -1;
				assert(ENOSPC == rc);
				assert(1 == l1s && 1 == l2s);
				rc = map_g.cpy_lock_region(udm, l1, l2, &rem);
				assert(0 == rc);
				lock_ref_cnt(l1, l2, l1s, l2s);
				assert(0 == l1s && 0 == l2s);
				udm->unlock(l1, l2);
				grow_nr++;
				if (0 == rem)
					pthread_barrier_wait(&grow_barrier_g); // let grow thread know
				goto again; // go ahead and do the insert, on the new table
			}
			assert(nullptr != trgt && (trgt->deleted() ^ !trgt->used()));
			nshifts++;
		case ENODATA:
			udm->insert(trgt, key, 1024, i);
			break;
		case 0:
			do {
				ncollisions++;
				trgt = udm->next(trgt, key);
			} while (nullptr != trgt);
			break;
		default:
			assert(0);
			break;
		}
		assert(trgt->used());
		udm->unlock(l1, l2);

		#if defined(UDEPOT_OR_PROFILING)
		if (probe_count++ % 64 == 0) {
			uint64_t t = ticks::get_ticks() - probe_ticks0;
			DTRACE_PROBE2(udepot, online_resize_put_ticks, t, resize);
		}
		#endif
	}
	UDEPOT_DBG("shift_nr=%lu collision_nr=%lu grow_nr=%lu", nshifts, ncollisions, grow_nr);
	return 0;
}

__attribute__((unused))
static int uDepotDirMapORGetTestMT(const u64 start, const u64 item_nr)
{
	uDepotMap<RuntimePosix> *udm;
	uDepotLock *l1, *l2;
	u32 l1s = 0, l2s = 0;
	for (u64 i = start; i < start + item_nr; ++i) {
		#if defined(UDEPOT_OR_PROFILING)
		static thread_local uint32_t probe_count = 0;
		uint64_t probe_ticks0 = ticks::get_ticks();
		#endif
		const u64 key = (i * prime);
		udm = map_g.hash_to_map(key);
		udm->lock_common(key, &l1, &l2);
		lock_ref_cnt(l1, l2, l1s, l2s);
		assert(0 == (l1s ^ l2s));
		// if growing then switch to new table
		if (0 == l1s && 0 == l1s) {
			// switch to shadow
			udm = map_g.hash_to_shadow_map(key);
			udm->lock_common(key, &l1, &l2);
		}
		udm->lock(l1,l2);
		HashEntry *trgt = udm->lookup(key, i);
		// assert(nullptr != trgt);
		if (nullptr == trgt) {
			UDEPOT_ERR("get returned trgt=%p.", trgt);
			assert(0); // error
			// udm->unlock(l1, l2);
			// return ENODATA;
		}
		udm->unlock(l1, l2);
		#if defined(UDEPOT_OR_PROFILING)
		if (probe_count++ % 128 == 0) {
			uint64_t t = ticks::get_ticks() - probe_ticks0;
			DTRACE_PROBE1(udepot, online_resize_get_ticks, t);
		}
		#endif
		// int rc __attribute__((unused)) = map_g.lookup(key, &trgt);
		// assert(0 == rc && nullptr != trgt);
	}
	return 0;
}

int uDepotDirMapORTest(const int argc, const char *const argv[])
{
	u32 thread_nr = 1;
	u64 kv_item_nr = 1000;
	std::atomic<bool> done (false);
	for (int i = 0; i < argc; ++i) {
		std::string arg = std::string(argv[i]);
		std::string val = i < argc - 1 ? std::string(argv[i + 1]) : "";
		if (arg == "--threads" || arg == "-t") {
			thread_nr = std::stoul(val);
			++i;
		} else if (arg == "--seg_size" || arg == "-s") {
			seg_size = std::stoul(val);
			++i;
		} else if (arg == "--grain_size" || arg == "-g") {
			grain_size = std::stoul(val);
			++i;
		} else if (arg == "--kv") {
			kv_item_nr = std::stoul(val);
			++i;
		}
	}
	UDEPOT_MSG("Starting Dir Map test with %u threads for %lu KV pairs (nlocks: %lu).",
		thread_nr, kv_item_nr, _UDEPOT_HOP_SCOTCH_LOCK_NR);
	pthread_barrier_init(&barrier_g, NULL, thread_nr + 1);
	pthread_barrier_init(&grow_barrier_g, NULL, 2);
	std::thread threads[thread_nr + 1];
	int rc = map_g.init(seg_size, grain_size);
	if (0 != rc) {
		UDEPOT_ERR("dir map init returned %s.", strerror(rc));
		return rc;
	}

	for (u32 tid = 0; tid < thread_nr; ++tid) {
		threads[tid] = std::thread(
			[tid, &thread_nr, &kv_item_nr]() {
				pthread_barrier_wait(&barrier_g);
				u64 items = kv_item_nr / thread_nr;
				UDEPOT_DBG("tid=%u start = %lu", tid, tid*items);
				int rc __attribute__((unused)) = uDepotDirMapORPutTestMT(tid * items, items);
				assert(0 == rc);
				pthread_barrier_wait(&barrier_g);
				pthread_barrier_wait(&barrier_g);
				rc = uDepotDirMapORGetTestMT(tid * items, items);
				assert(0 == rc);
				pthread_barrier_wait(&barrier_g);
			});
	}
	threads[thread_nr] = std::thread(
		[&done]() {
			while (!done) {
				// alloc shadow directory
				int rc __attribute__((unused)) = map_g.alloc_shadow(seg_size, grain_size);
				assert(0 == rc);
				// wait until all the ht entries have been copied
				pthread_barrier_wait(&grow_barrier_g);
				if (done)
					break;
				rc = map_g.grow_finished(seg_size, grain_size);
				assert(0 == rc);
			}
		});
	struct timespec before;
	struct timespec after;
	clock_gettime(CLOCK_MONOTONIC, &before);
	pthread_barrier_wait(&barrier_g);
	pthread_barrier_wait(&barrier_g);
	clock_gettime(CLOCK_MONOTONIC, &after);
	u64 put_ns = (1000000000UL) * (after.tv_sec - before.tv_sec) + after.tv_nsec - before.tv_nsec;
	clock_gettime(CLOCK_MONOTONIC, &before);
	pthread_barrier_wait(&barrier_g);
	pthread_barrier_wait(&barrier_g);
	clock_gettime(CLOCK_MONOTONIC, &after);
	u64 get_ns = (1000000000UL) * (after.tv_sec - before.tv_sec) + after.tv_nsec - before.tv_nsec;
	UDEPOT_MSG("inserts/sec=%lu item_nr=%lu threads=%u",kv_item_nr * (1000000000UL) / put_ns ,kv_item_nr, thread_nr);
	UDEPOT_MSG("lookups/sec=%lu item_nr=%lu",kv_item_nr * (1000000000UL) / get_ns ,kv_item_nr);
	done = true;
	pthread_barrier_wait(&grow_barrier_g);
	for (auto &th : threads)
		if (th.joinable())
			th.join();
	map_g.shutdown();
	return 0;
}

} // end namespace udepot

int main (const int argc, const char *const argv[])
{
	int rc = udepot::uDepotDirMapORTest(argc, argv);
	return rc;
}
