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

#ifndef	_UDEPOT_H_
#define	_UDEPOT_H_

#include <atomic>
#include <vector>
#include <string>
#include <cassert>

#include "kv.hh"
#include "util/types.h"
#include "uDepot/net.hh"
#include "uDepot/io.hh"

namespace udepot {

/**
 * uDepot base class, essentially a pure interface class with some
 * shared configuration and stat data members for distributed KV
 * implementations
 */
template<typename RT>
class uDepot: public KV {
public:
	uDepot(u32 thread_nr,
		const std::string &fname,
		const size_t size,
		const bool force_destroy = false);
	virtual ~uDepot();
	unsigned long get_size() const override {
		return used_bytes_m;
	}
	unsigned long get_total_size() const {
		return total_bytes_m;
	}
	unsigned long get_raw_capacity() const override {
		return size_m;
	}
	unsigned long get_kv_nr() const {
		return key_nr_m;
	}
	void print_stats();
	virtual void thread_local_entry();
	virtual void thread_local_exit();


protected:
	void kv_added(const u64 kv_bytes, const u64 tot_bytes) {
		used_bytes_m  += kv_bytes;
		total_bytes_m += tot_bytes;
		key_nr_m++;
	}
	void kv_bytes_restored(const u64 tot_bytes, const u64 kv_bytes) {
		used_bytes_m  = kv_bytes;
		total_bytes_m = tot_bytes;
	}
	void kv_removed(const u64 kv_bytes, const u64 tot_bytes) {
		u64 old_size __attribute__((unused)) = used_bytes_m.fetch_sub(kv_bytes);
		assert(kv_bytes <= old_size);
		old_size = total_bytes_m.fetch_sub(tot_bytes);
		assert(tot_bytes <= old_size);
		old_size = key_nr_m.fetch_sub(1);
		assert(0 < old_size);
	}

	/* TODO: make the following stats per thread */
	std::atomic<u64> key_nr_m;
	std::atomic<u64> used_bytes_m;
	std::atomic<u64> total_bytes_m;
	std::atomic<u64> local_put_nr_m;
	std::atomic<u64> local_del_nr_m;
	std::atomic<u64> local_get_nr_m;
	std::atomic<u64> remote_put_nr_m;
	std::atomic<u64> remote_del_nr_m;
	std::atomic<u64> remote_get_nr_m;
	u32 thread_nr_m;
	const bool force_destroy_m;
	const std::string fname_m;
	size_t size_m;
	void *mm_region_m;
	typename RT::IO udepot_io_m;

	virtual int register_local_region();
	virtual int unregister_local_region();

	uDepot(const uDepot &)            = delete;
	uDepot& operator=(const uDepot &) = delete;
};

} // end namespace udepot
#endif	/* _UDEPOT_H_ */
