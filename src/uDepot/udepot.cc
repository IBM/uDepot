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

#include "uDepot/uDepot.hh"

#include <cassert>
#include <atomic>
#include <cstring>
#include <set>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "util/types.h"
#include "util/debug.h"

#include "uDepot/net.hh"
#include "uDepot/backend.hh"
#include "uDepot/stats.hh"
#include "uDepot/thread-id.hh"

namespace udepot {

template<typename RT>
uDepot<RT>::uDepot(
	const u32 thread_nr,
	const std::string &fname,
	const size_t size,
	const bool force_destroy)
	:KV(),
	 key_nr_m(0),
	 // TODO: move the statistics below to uDepot/stats.hh
	 used_bytes_m(0ULL),
	 total_bytes_m(0ULL),
	 local_put_nr_m(0),
	 local_del_nr_m(0),
	 local_get_nr_m(0),
	 remote_put_nr_m(0),
	 remote_del_nr_m(0),
	 remote_get_nr_m(0),
	 thread_nr_m(thread_nr),
	 force_destroy_m(force_destroy),
	 fname_m(fname),
	 size_m(size),
	 mm_region_m(nullptr) { }

template<typename RT>
uDepot<RT>::~uDepot()
{
	print_stats();
}

template<typename RT>
void
uDepot<RT>::print_stats()
{
	UDEPOT_MSG(
		"KV pairs=%lu Used bytes=%luKiB Total Bytes=%luKiB.\n"
		"  Local Puts=%lu Remote Puts=%lu.\n"
		"  Local Dels=%lu Remote Dels=%lu.\n"
		"  Local Gets=%lu Remote Gets=%lu.\n",
		key_nr_m.load(), used_bytes_m.load() >> 10, total_bytes_m.load() >> 10,
		local_put_nr_m.load(), remote_put_nr_m.load(),
		local_del_nr_m.load(), remote_del_nr_m.load(),
		local_get_nr_m.load(), remote_get_nr_m.load());

	uDepotStats::print();
}

template<typename RT>
int uDepot<RT>::register_local_region()
{
	int rc = 0;
	const char *const fname = fname_m.c_str();
	u64 map_size;
	// fd_m = open(fname, O_RDWR | O_CREAT | O_DIRECT | O_SYNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	rc = udepot_io_m.open(fname, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	if (0 != rc) {
		UDEPOT_ERR("failed to open %s err=%d\n", fname, rc);
		goto out;
	}

	if (udepot_io_m.get_size() < size_m) {
		rc = udepot_io_m.ftruncate(size_m);
		if (0 != rc) {
			rc = errno;
			UDEPOT_ERR("ftruncate failed with %d.\n", errno);
			goto close;
		}
	}

	map_size = udepot_io_m.get_size();
	if (map_size == (u64)-1) {
		UDEPOT_ERR("= get_size() failed with %d\n", errno);
		goto unlink;
	}

	mm_region_m = udepot_io_m.mmap(nullptr, map_size, PROT_READ|PROT_WRITE, MAP_SHARED, 0);
	//mm_region_m = mmap64(nullptr, map_size, PROT_READ|PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	if (MAP_FAILED == mm_region_m) {
		UDEPOT_ERR("mmap failed with %d\n", errno);
		rc = ENOMEM;
		goto unlink;
	}

	//memset(mm_region_m, 0, map_size);

	assert(0 == rc);
	return rc;
unlink:
	rc = udepot_io_m.unlink(fname);
	assert(0 == rc);
close:
	rc = udepot_io_m.close();
	assert(0 == rc);
out:
	return rc;
}

template<typename RT>
int uDepot<RT>::unregister_local_region()
{
	int rc = udepot_io_m.msync(mm_region_m, size_m, MS_SYNC);
	assert(0 == rc);

	rc = udepot_io_m.munmap(mm_region_m, size_m);
	assert(0 == rc);

	// rc = unlink(fname_m.c_str());
	// assert(0 == rc);

	rc = udepot_io_m.close();
	assert(0 == rc);

	return rc;
}

// TODO: Make sure we call this only after uDepot has been initialized
// There is a thread_local_entry call in uDepot init, needed to do I/O
// at init time (metadata persistence, etc)
template<typename RT>
void uDepot<RT>::thread_local_entry()
{
	unsigned tid = -1;
	UDEPOT_DBG("tid=%u", ThreadId::get());
	const int rc = ThreadId::allocate(tid);
	if (0 != rc) {
		UDEPOT_ERR("threadid allocate failed with %d", rc);
		return;
	}
	UDEPOT_DBG("set tid to %u", ThreadId::get());
	RT::IO::thread_init();
	uDepotStats::tls_init();
}

template<typename RT>
void uDepot<RT>::thread_local_exit()
{
	const int rc = ThreadId::release();
	if (0 != rc) {
		UDEPOT_ERR("NOTE: threadid release failed with %d", rc);
		return;
	}
	RT::IO::thread_exit();
}

// instantiate the templates
template class uDepot<RuntimePosix>;
template class uDepot<RuntimePosixODirect>;
template class uDepot<RuntimePosixODirectMC>;
template class uDepot<RuntimeTrt>;
template class uDepot<RuntimeTrtMC>;
template class uDepot<RuntimeTrtUring>;
template class uDepot<RuntimeTrtUringMC>;
#if defined(UDEPOT_TRT_SPDK)
template class uDepot<RuntimeTrtSpdk>;
template class uDepot<RuntimeTrtSpdkArray>;
template class uDepot<RuntimePosixSpdk>;
template class uDepot<RuntimeTrtSpdkArrayMC>;
#endif

};
