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

#include "uDepot/kv-conf.hh"

#include <string>
#include <cstdio>
#include <unistd.h>

#include "util/debug.h"
#include "uDepot/udepot-lsa.hh"

namespace udepot {

KV_conf::KV_conf(const std::string & fname):
	type_m(KV_UDEPOT_SALSA), size_m(0),
	segment_size_m(UDEPOT_DEFAULT_SEGMENT_SIZE),
	grain_size_m(UDEPOT_DEFAULT_GRAIN_SIZE),
	thread_nr_m(1),
	overprovision_m(UDEPOT_DEFAULT_OVERPROVISION),
	force_destroy_m(false),
	help_m(false),
	fname_m(fname)
{}

bool
KV_conf::valid_kv_type(const kv_type type)
{
	switch (type) {
		case KV_UDEPOT_SALSA:
		case KV_UDEPOT_SALSA_O_DIRECT:
#if defined(UDEPOT_TRT_SPDK)
		case KV_UDEPOT_SALSA_TRT_SPDK:
		case KV_UDEPOT_SALSA_TRT_SPDK_ARRAY:
		case KV_UDEPOT_SALSA_SPDK:
		case KV_UDEPOT_SALSA_TRT_SPDK_ARRAY_MC:
#endif
		case KV_UDEPOT_SALSA_TRT_AIO:
		case KV_UDEPOT_SALSA_TRT_AIO_MC:
		case KV_UDEPOT_SALSA_O_DIRECT_MC:
		return true;

		case KV_UDEPOT_LAST:
		default:
			return false;
	}
}

int
KV_conf::parse_args(ParseArgs &args)
{

	const int argc    = args.argc_;
	const char *const *const argv = args.argv_;

	for (int i = 1; i < argc; ++i) {
		std::string arg = std::string(argv[i]);
		try {
			// all arguments take at least one value, except help
			std::string val = i < argc - 1 ? std::string(argv[i + 1]) : "";
			UDEPOT_DBG("arg given %s (val=%s).\n", arg.c_str(), val.c_str());
			if ("-t" == arg || "--threads" == arg) {
				thread_nr_m = std::stoi(val);
				args.used_arg_and_val(i);
				++i;
			} else if ("-u" == arg || "--udepot" == arg) {
				int type = std::stoi(val);
				args.used_arg_and_val(i);
				++i;
				if (type < KV_UDEPOT_LAST)
					type_m = static_cast<enum kv_type>(type);
			} else if ("--segment-size" == arg) {
				segment_size_m = std::stoul(val);
				args.used_arg_and_val(i);
				++i;
			} else if ("--grain-size" == arg) {
				grain_size_m = std::stoul(val);
				args.used_arg_and_val(i);
				++i;
			} else if ("-h" == arg || "--help" == arg) {
				args.used_arg(i);
				help_m = true;
				return 0;
			} else if ("-f" == arg || "--file" == arg) {
				fname_m = val;
				args.used_arg_and_val(i);
				++i;
			} else if ("-s" == arg || "--size" == arg) {
				size_m = std::stoul(val);
				args.used_arg_and_val(i);
				++i;
			} else if ("--server-conf" == arg) {
				self_server_m = val;
				args.used_arg_and_val(i);
				++i;
			} else if ("-o" == arg || "--overprovision" == arg) {
				overprovision_m = std::stoi(val);
				args.used_arg_and_val(i);
				++i;
			} else if ("-m" == arg || "--machines" == arg) {
				UDEPOT_DBG("machines arg given.\n");
				args.used_arg(i);
				for (int j = i + 1; j < argc; ++j) {
					std::string val = std::string(argv[j]);
					if ('-' == val[0])
						break;
					remote_servers_m.push_back(val);
					args.used_arg(j);
					++i;
				}
			} else if ("--force-destroy" == arg) {
				args.used_arg(i);
				force_destroy_m = true;
			}
		} catch (std::invalid_argument &ia) {
			UDEPOT_ERR("invalid argument for %s\n", arg.c_str());
			return EINVAL;
		}
	}

	if ("" == fname_m && type_m != KV_UDEPOT_SALSA_TRT_SPDK &&
		type_m != KV_UDEPOT_SALSA_TRT_SPDK_ARRAY &&
		type_m != KV_UDEPOT_SALSA_SPDK) {
		UDEPOT_ERR("%s", "" == fname_m ? "-no file path given.\n" : "");
		return EINVAL;
	}
	if (!valid_kv_type(type_m)) {
		UDEPOT_ERR("Invalid kv type given.\n");
		return EINVAL;
	}
	return 0;
}

std::string
KV_conf::type_to_string(const kv_type type)
{
	switch (type) {
		case KV_UDEPOT_SALSA:          return std::string("SALSA");
		case KV_UDEPOT_SALSA_O_DIRECT: return std::string("SALSA_O_DIRECT");
		case KV_UDEPOT_SALSA_SPDK:     return std::string("SALSA_SPDK");
		case KV_UDEPOT_SALSA_TRT_AIO:  return std::string("SALSA_TRT_AIO");
		case KV_UDEPOT_SALSA_TRT_SPDK: return std::string("SALSA_TRT_SPDK");
		case KV_UDEPOT_SALSA_TRT_SPDK_ARRAY: return std::string("SALSA_TRT_SPDK_ARRAY");
		case KV_UDEPOT_SALSA_O_DIRECT_MC: return std::string("SALSA_O_DIRECT_MC");
		case KV_UDEPOT_SALSA_TRT_SPDK_ARRAY_MC: return std::string("SALSA_TRT_SPDK_ARRAY_MC");
		case KV_UDEPOT_SALSA_TRT_AIO_MC: return std::string("SALSA_TRT_AIO_MC");
		case KV_UDEPOT_LAST:           return std::string("__LAST__");
	}
	return std::string("Invalid type");
}

void
KV_conf::print_usage(const char name[])
{
	printf("Usage: %s\n", name);
	printf("Compulsory arguments:\n");
	printf("-f, --file path: path to file that contains the KV data for this machine.\n\n");
	printf("Optional arguments:\n");
	printf("-s --size size: size of the file in bytes (default=0 will use the existing file size).\n");
	printf("-t, --threads nr: Number of KV client threads to be spawned on this machine\n");
	printf("--segment-size: Change the default segment size in grains (default = %lu grains).\n",
		UDEPOT_DEFAULT_SEGMENT_SIZE);
	printf("--grain-size: Change the default grain size in grains %luB(default = ).\n",
		UDEPOT_DEFAULT_GRAIN_SIZE);
	printf("--force-destroy: If set the file will be overwritten even it contains data (default=false).\n");
	printf("--server-conf : URL of the local server that will accept remote connections.\n");
	printf("-m, --machines ip0 ip1 .. ipN: List of space separated IPs of the remote KV server machines.\n");
	printf("-h, --help Print usage and exit\n");
	printf("-u, --udepot TYPE: type of KV implementation to be used:\n");
	for (u32 i = 0; i < KV_UDEPOT_LAST; i++) {
		auto ty = static_cast<kv_type>(i);
		auto disabled = valid_kv_type(ty) ? "" : " (disabled at compile-time)";
		printf(" %2u: %s%s\n", i, type_to_string(ty).c_str(), disabled);
	}
}

u64 KV_conf::sanitize_grain_size(u64 grain_size)
{
	// grain_size: >=1 and power of 2
	grain_size = std::max(1UL, grain_size);
	grain_size = round_down_pow2(grain_size);
	return grain_size;
}

u64 KV_conf::sanitize_segment_size(u64 grain_size, u64 segment_size)
{
	size_t old = segment_size;
	// segment: >= 1 and OS PAGE SIZE aligned
	// PAGE SIZE segment alignment needed for mmap
	const u64 page_size = sysconf(_SC_PAGESIZE);
	const u64 step_size = grain_size < page_size ? page_size / grain_size : 1;
	segment_size = align_down(segment_size, step_size);
	segment_size = std::max(1UL, segment_size);
	if (old != segment_size)
		UDEPOT_MSG("segment size sanitized from %lu to %lu\n", old, segment_size);
	return segment_size;
}

void KV_conf::validate_and_sanitize_parameters()
{
	switch (type_m) {
		case KV_UDEPOT_SALSA_O_DIRECT:
		case KV_UDEPOT_SALSA_SPDK:
		case KV_UDEPOT_SALSA_TRT_SPDK:
		case KV_UDEPOT_SALSA_TRT_AIO:
		case KV_UDEPOT_SALSA_TRT_SPDK_ARRAY:
		case KV_UDEPOT_SALSA_O_DIRECT_MC:
		case KV_UDEPOT_SALSA_TRT_SPDK_ARRAY_MC:
			// make sure the grain-size is at least 512-byte
			// aligned, otherwise dma accesses will fail
			grain_size_m = align_up(grain_size_m, 512UL);
		default:
			break;
	}
	// order is important, grain size has to be sanitized first
	grain_size_m = sanitize_grain_size(grain_size_m);
	segment_size_m = sanitize_segment_size(grain_size_m, segment_size_m);

	overprovision_m = std::min(600U, overprovision_m); // max 60%
	overprovision_m = std::max(5U, overprovision_m); // min 0.5%

        size_m = std::min(grain_size_m * (UDEPOT_SALSA_UNUSED_PBA_ENTRY - 1UL), size_m);
}

};
