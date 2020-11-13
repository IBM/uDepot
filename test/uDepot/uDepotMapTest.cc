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

#include "uDepot/lsa/udepot-map.hh"
#include "uDepot/lsa/hash-entry.hh"
#include "uDepot/backend.hh"
#include "udepot-utests.hh"


namespace udepot {

uDepotMap<RuntimePosix> *map_g;
void *ht_g;

static int setup_test(const u64 bytes, const u32 thread_nr)
{
	int rc __attribute__((unused)) = 0;
	ht_g = malloc(bytes);
	assert(nullptr != ht_g);
	map_g = new uDepotMap<RuntimePosix>();
	assert(nullptr != map_g);
	rc = map_g->init(_UDEPOT_HOP_SCOTCH_LOCK_NR, bytes, ht_g);
	assert(0 == rc);
	return 0;
}

static int end_test()
{
	delete map_g;
	map_g = nullptr;
	free(ht_g);
	ht_g = nullptr;
	return 0;
}

constexpr u64 prime = 2654435761UL;

 __attribute__((unused))
 static int uDepotMapPutTest(const u64 start, const u64 item_nr)
{
	u64 nshifts = 0, ncollisions = 0;
	struct timespec before;
	clock_gettime(CLOCK_MONOTONIC, &before);
	for (u64 i = start; i < start + item_nr; ++i) {
		const u64 key = (i * prime);
		HashEntry *trgt = nullptr;
		int rc = map_g->lookup(key, &trgt);
                switch (rc) {
                case  ENOSPC:
			rc = map_g->try_shift(key , &trgt);
			assert(0 == rc && nullptr != trgt);
			assert(trgt->deleted() ^ !trgt->used());
			nshifts++;
                case ENODATA:
			map_g->insert(trgt, key, 1024, i);
                        assert(trgt->used());
                        break;
                case 0:
                        do {
                                ncollisions++;
                                trgt = map_g->next(trgt, key);
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
 static int uDepotMapGetTest(const u64 start, const u64 item_nr)
{
	struct timespec before;
	clock_gettime(CLOCK_MONOTONIC, &before);
	for (u64 i = start; i < start + item_nr; ++i) {
		const u64 key = (i * prime);
		HashEntry *trgt = map_g->lookup(key, i);
		// assert(nullptr != trgt);
		if (nullptr == trgt) {
			UDEPOT_ERR("get returned trgt=%p.", trgt);
			assert(0); // error
			return ENODATA;
		}
		// int rc __attribute__((unused)) = map_g->lookup(key, &trgt);
		// assert(0 == rc && nullptr != trgt);
	}
	struct timespec after;
	clock_gettime(CLOCK_MONOTONIC, &after);
	u64 ns = (1000000000UL) * (after.tv_sec - before.tv_sec) + after.tv_nsec - before.tv_nsec;
	UDEPOT_MSG("lookups/sec=%lu", item_nr * (1000000000UL) / ns );
	return 0;
}

 __attribute__((unused))
 static int uDepotMapDelTest(const u64 start, const u64 item_nr)
{
	for (u64 i = start; i < start + item_nr; ++i) {
		const u64 key = (i * prime);
		HashEntry *trgt = nullptr;
		int rc __attribute__((unused)) = map_g->lookup(key, &trgt);
		assert(0 == rc && nullptr != trgt);
		map_g->remove(trgt, 0);
		rc = map_g->lookup(key, &trgt);
		assert(ENODATA == rc && nullptr != trgt);
	}
	return 0;
}

 __attribute__((unused))
static int uDepotMapPutTestMT(const u64 start, const u64 item_nr)
{
	u64 nshifts = 0, ncollisions = 0;
	for (u64 i = start; i < start + item_nr; ++i) {
		const u64 key = (i * prime);
		HashEntry *trgt = nullptr;
		map_g->lock(key);
		int rc = map_g->lookup(key, &trgt);
                switch (rc) {
                case  ENOSPC:
			rc = map_g->try_shift(key , &trgt);
			assert(0 == rc && nullptr != trgt);
			assert(trgt->deleted() ^ !trgt->used());
			nshifts++;
                case ENODATA:
			map_g->insert(trgt, key, 1024, i);
                        break;
                case 0:
                        do {
                                ncollisions++;
                                trgt = map_g->next(trgt, key);
                        } while (nullptr != trgt);
                        break;
                default:
                        assert(0);
                        break;
                }
		map_g->unlock(key);
		assert(trgt->used());
	}
	UDEPOT_DBG("shift_nr=%lu collision_nr=%lu", nshifts, ncollisions);
	return 0;
}

 __attribute__((unused))
 static int uDepotMapGetTestMT(const u64 start, const u64 item_nr)
{
	for (u64 i = start; i < start + item_nr; ++i) {
		const u64 key = (i * prime);
		map_g->lock(key);
		HashEntry *trgt = map_g->lookup(key, i);
		// assert(nullptr != trgt);
		if (nullptr == trgt) {
			UDEPOT_ERR("get returned trgt=%p.", trgt);
			assert(0); // error
			map_g->unlock(key);
			return ENODATA;
		}
		map_g->unlock(key);
		// int rc __attribute__((unused)) = map_g->lookup(key, &trgt);
		// assert(0 == rc && nullptr != trgt);
	}
	return 0;
}

extern int uDepotMapTest(const int argc, char *argv[]);
int uDepotMapTest(const int argc, char *argv[])
{
	int rc __attribute__((unused)) = 0;
	const u32 bytes[] = {2048, 1<<20, 1<<29};
	const u32 added = _UDEPOT_HOP_SCOTCH_BUCKET_SIZE * 2 * sizeof(HashEntry);
	for (const u32 *p = bytes; p < &bytes[sizeof(bytes)/sizeof(bytes[0])]; ++p) {
		rc = setup_test(*p + added, 1);
		const u64 items[] = {0, 1, *p / (u64) sizeof(HashEntry) / 2UL, *p * 85UL / 100UL / (u64) sizeof(HashEntry) };
		for (const u64 *item = &items[0]; item < &items[sizeof(items)/sizeof(items[0])]; ++item) {
			rc = uDepotMapPutTest(0, *item);
			assert(0 == rc);
			map_g->printStats();
			rc = uDepotMapGetTest(0, *item);
			assert(0 == rc);
			rc = uDepotMapDelTest(0, *item);
			assert(0 == rc);
		}
		assert(0 == rc);
		end_test();
	}
        bool found = false;
        for (int i = 0; i < argc; ++i) {
		std::string arg = std::string(argv[i]);
                if (arg == "--threads") {
                        found = true;
                        break;
                }
        }
        if (!found)
                return 0;
        // TODO: read params from args
	const u64 items_total = bytes[sizeof(bytes)/sizeof(bytes[0]) - 1] * 90UL / 100UL/ (u64) sizeof(HashEntry);
	for (u32 thread_nr = 1; thread_nr <= 20; thread_nr = 16 != thread_nr ? thread_nr * 2 : 20) {
		pthread_barrier_t barrier;
		pthread_barrier_init(&barrier, NULL, thread_nr + 1);
		std::thread threads[thread_nr];
		rc = setup_test(bytes[sizeof(bytes)/sizeof(bytes[0]) - 1] + added, 1);
		assert(0 == rc);
		for (u32 tid = 0; tid < thread_nr; ++tid) {
			threads[tid] = std::thread(
				[&barrier, &bytes, tid, &thread_nr, &items_total]() {
					pthread_barrier_wait(&barrier);
					u64 items = items_total / thread_nr;
					UDEPOT_DBG("tid=%u start = %lu", tid, tid*items);
					int rc __attribute__((unused)) = uDepotMapPutTestMT(tid * items, items);
					assert(0 == rc);
					pthread_barrier_wait(&barrier);
					pthread_barrier_wait(&barrier);
					rc = uDepotMapGetTestMT(tid * items, items);
					assert(0 == rc);
					pthread_barrier_wait(&barrier);
				});
		}
		struct timespec before;
		struct timespec after;
		clock_gettime(CLOCK_MONOTONIC, &before);
		pthread_barrier_wait(&barrier);
		pthread_barrier_wait(&barrier);
		clock_gettime(CLOCK_MONOTONIC, &after);
		u64 put_ns = (1000000000UL) * (after.tv_sec - before.tv_sec) + after.tv_nsec - before.tv_nsec;
		clock_gettime(CLOCK_MONOTONIC, &before);
		pthread_barrier_wait(&barrier);
		pthread_barrier_wait(&barrier);
		clock_gettime(CLOCK_MONOTONIC, &after);
		u64 get_ns = (1000000000UL) * (after.tv_sec - before.tv_sec) + after.tv_nsec - before.tv_nsec;
		UDEPOT_MSG("inserts/sec=%lu item_nr=%lu threads=%u",items_total * (1000000000UL) / put_ns ,items_total, thread_nr);
		UDEPOT_MSG("lookups/sec=%lu item_nr=%lu",items_total * (1000000000UL) / get_ns ,items_total);
		for (auto &th : threads)
			if (th.joinable())
				th.join();
		end_test();
	}
	return 0;
}
};
