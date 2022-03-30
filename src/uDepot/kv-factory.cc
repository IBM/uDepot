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


#include "uDepot/kv-factory.hh"

#include "kv.hh"
#include "uDepot/backend.hh"
#include "uDepot/udepot-lsa.hh"
#include "uDepot/net/memcache.hh"
#include "uDepot/net/socket.hh"
#include "uDepot/kv-conf-helpers.hh"

namespace udepot {

template<typename RT>
static
::KV *uDepotSalsa_new(const KV_conf &conf)
{
	return new uDepotSalsa<RT>(conf.thread_nr_m,
				get_udepot_clients_conf<typename RT::Net>(conf.remote_servers_m),
				conf.fname_m,
				conf.size_m,
				get_udepot_server_conf<typename RT::Net>(conf.self_server_m),
				conf.force_destroy_m,
				conf.grain_size_m,
				conf.segment_size_m,
				conf.overprovision_m);
}

::KV *
KV_factory::KV_new(const KV_conf &conf)
{
	KV *ret = nullptr;
	switch(conf.type_m) {
		case KV_conf::KV_UDEPOT_SALSA:
			ret = uDepotSalsa_new<RuntimePosix>(conf);
			break;
		case KV_conf::KV_UDEPOT_SALSA_O_DIRECT:
			ret = uDepotSalsa_new<RuntimePosixODirect>(conf);
			break;
		case KV_conf::KV_UDEPOT_SALSA_TRT_AIO:
			ret = uDepotSalsa_new<RuntimeTrt>(conf);
			break;
		case KV_conf::KV_UDEPOT_SALSA_TRT_URING:
			ret = uDepotSalsa_new<RuntimeTrtUring>(conf);
			break;
		#if defined(UDEPOT_TRT_SPDK)
		case KV_conf::KV_UDEPOT_SALSA_SPDK:
			ret = uDepotSalsa_new<RuntimePosixSpdk>(conf);
			break;
		case KV_conf::KV_UDEPOT_SALSA_TRT_SPDK:
			ret = uDepotSalsa_new<RuntimeTrtSpdk>(conf);
			break;
		case KV_conf::KV_UDEPOT_SALSA_TRT_SPDK_ARRAY:
			ret = uDepotSalsa_new<RuntimeTrtSpdkArray>(conf);
			break;
		case KV_conf::KV_UDEPOT_SALSA_TRT_SPDK_ARRAY_MC:
			ret = uDepotSalsa_new<RuntimeTrtSpdkArrayMC>(conf);
			break;
		#endif
		case KV_conf::KV_UDEPOT_SALSA_O_DIRECT_MC:
			ret = uDepotSalsa_new<RuntimePosixODirectMC>(conf);
			break;
		case KV_conf::KV_UDEPOT_SALSA_TRT_AIO_MC:
			ret = uDepotSalsa_new<RuntimeTrtMC>(conf);
			break;
		case KV_conf::KV_UDEPOT_SALSA_TRT_URING_MC:
			ret = uDepotSalsa_new<RuntimeTrtUringMC>(conf);
			break;
		default:
			UDEPOT_ERR("Unsupported backend: %u\n", static_cast<unsigned>(conf.type_m));
			ret = nullptr;
			break;
	}
	return ret;
}

};
