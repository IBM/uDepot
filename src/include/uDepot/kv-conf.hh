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
#ifndef _UDEPOT_KV_CONF_H_
#define _UDEPOT_KV_CONF_H_

#include <string>
#include <vector>
#include <cstdint>
#include <iostream>

namespace udepot {
struct ParseArgs;
struct KV_conf {
	KV_conf(const std::string & fname);
	KV_conf():KV_conf(std::string(""))
		{}
	KV_conf(const std::string & fname,
		uint64_t size,
		bool force_destroy,
		uint64_t grain_size,
		uint64_t segment_size) : KV_conf(fname) {
		size_m = size;
		force_destroy_m = force_destroy;
		grain_size_m = grain_size;
		segment_size_m = segment_size;
	}
	virtual ~KV_conf() {};
	enum kv_type {
		KV_UDEPOT_SALSA                   = 2,
		KV_UDEPOT_SALSA_O_DIRECT          = 3,
		KV_UDEPOT_SALSA_SPDK              = 4,
		KV_UDEPOT_SALSA_TRT_AIO           = 5,
		KV_UDEPOT_SALSA_TRT_SPDK          = 6,
		KV_UDEPOT_SALSA_TRT_SPDK_ARRAY    = 7,
		// Memcache types
		KV_UDEPOT_SALSA_O_DIRECT_MC       = 8,
		KV_UDEPOT_SALSA_TRT_SPDK_ARRAY_MC = 9,
		KV_UDEPOT_SALSA_TRT_AIO_MC        = 10,
		KV_UDEPOT_LAST,
	};

	enum kv_type type_m;
	uint64_t size_m;
	uint64_t segment_size_m; // in # grains
	uint64_t grain_size_m;	 // in bytes
	uint32_t thread_nr_m;
	uint32_t overprovision_m; // overprovisioning in 1/1000 over
				  // entire capacity. e.g, 100 -> 10%
				  // of capacity set aside for GC
	bool force_destroy_m;
	bool help_m;

	std::string fname_m;

	std::string               self_server_m;
	std::vector<std::string>  remote_servers_m;

	static std::string type_to_string(kv_type);
	static uint64_t sanitize_grain_size(uint64_t grain_size);
	static uint64_t sanitize_segment_size(uint64_t grain_size, uint64_t segment_size);
	static bool valid_kv_type(kv_type);

	virtual int parse_args(ParseArgs &args);
	virtual void print_usage(const char []);
	virtual void validate_and_sanitize_parameters();
};

struct ParseArgs {
	const int argc_;
	const char *const*const argv_;
	std::vector<bool> used_; // bitset of used arguments, to track invalid arguments at the end

	ParseArgs(const int argc, const char *const *const argv)
		: argc_(argc)
		, argv_(argv)
		, used_(argc, false) {}

	void used_arg(int i)         { used_[i] = true; }
	void used_arg_and_val(int i) { used_arg(i); used_arg(i+1); }

	bool has_unused_args(void) {
		for (int i=1; i < argc_; i++) { // ignore argv[0]
			if (!used_[i])
				return true;
		}
		return false;
	}

	void print_unused_args(std::ostream &out) {
		for (int i=1; i < argc_; i++) { // ignore argv[0]
			if (!used_[i]) {
				if (argv_[i][0] == '-') {
					out << " Unused (invalid?) option:" << argv_[i]  << std::endl;
				} else {
					out << " Unused argument:" << argv_[i]  << std::endl;
				}
			}
		}
	}
};

}; // namespace udepot

#endif	// _UDEPOT_KV_CONF_H_
