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


#ifndef _UDEPOT_NET_HH_
#define _UDEPOT_NET_HH_

#include <vector>
#include <set>

#include "uDepot/net/concepts.hh"
#include "kv-mbuff.hh"
#include "util/socket-peer-conf.hh"

namespace udepot {

// Common part for uDepot networking.

template<typename Net>
class uDepotNet {
	using Srv     = typename Net::Server;
	using Cli     = typename Net::Client;

	const std::vector<typename Cli::Conf> client_confs_m; // client confs
	typename Srv::Conf                    server_conf_m;  // server configuration
	Net                                   base_m;         // net instance
	Srv                                   server_m;       // server instance
	std::vector<std::vector<Cli *>>       conn_m;
	std::vector<std::vector<Cli *>>       consistent_hash_m;
	KV_MbuffInterface                    &kv_mbuff_m;

	void setup_consistent_hash_map(void);
	__attribute__((warn_unused_result)) int init_socket_key_value_client(void);
	int stop_clients(void);
	int stop_server(void);
	int clear();

	bool is_needed() {
		return (server_conf_m.is_valid() || client_confs_m.size() > 0);
	}


public:
	// NB: the kv_mbuff is passed to the server and clients:
	// server uses it to serve requests
	// clients may use it for buffer allocation on mbuffs
	uDepotNet(uint32_t                        thread_nr,
	          std::vector<typename Cli::Conf> client_confs,
	          typename Srv::Conf              server_conf,
	          KV_MbuffInterface               &kv_mbuff);

	int global_init(void) { return base_m.global_init(); }
	int thread_init(void) {
		return is_needed() ? base_m.thread_init() : 0;
	}
	int thread_exit(void) {
		return is_needed() ? base_m.thread_exit() : 0;
	}
	int global_exit(void) { return base_m.global_exit(); }

	__attribute__((warn_unused_result)) int start_net_all(void);
	int stop_net_all(void);

public:
	// Retrieve a client based on the key hash and the tid
	//
	// Returns a client that can be used for operations, or NULL if the
	// operation needs to be performed locally.
	Cli *get_client(uint64_t h, uint32_t tid) {
		if (consistent_hash_m[tid].size() == 0)
			return nullptr;
		const u32 idx = h % (consistent_hash_m[tid].size());
		return get_scd(tid, idx);
	}

private:
	Cli *get_scd(const u32 tid, const u32 idx) const {
		return tid < consistent_hash_m.size()
			?  (idx < consistent_hash_m[tid].size() ? consistent_hash_m[tid][idx] : nullptr)
			: nullptr;
	}
};

} // end namespace udepot

#endif /* ifndef _UDEPOT_NET_HH_ */
