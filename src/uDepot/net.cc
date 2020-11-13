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

#include "uDepot/net.hh"
#include "uDepot/net/socket.hh"
#include "uDepot/net/memcache.hh"
#include "uDepot/net/trt-epoll.hh"
#include "uDepot/net/trt-memcache.hh"

namespace udepot {

template<typename Net>
uDepotNet<Net>::uDepotNet(
	uint32_t                        thread_nr,
	std::vector<typename Cli::Conf> client_confs,
	typename Srv::Conf              server_conf,
	KV_MbuffInterface               &kv_mbuff)
 : client_confs_m(client_confs)
 , server_conf_m(server_conf)
 , base_m(typename Net::Conf()) // hard-coded for now
 , server_m(base_m, kv_mbuff, typename Srv::Conf(server_conf_m))
 , conn_m(thread_nr)
 , consistent_hash_m(thread_nr,
					 std::vector<typename Net::Client *>(
							 client_confs_m.size() +
							 (server_conf.get_server_id().is_invalid() ? 0 : 1) ))
 , kv_mbuff_m(kv_mbuff)
 {
	for (u32 tid = 0; tid < consistent_hash_m.size(); ++tid)
		for (u32 i = 0; i < consistent_hash_m[tid].size(); ++i)
			consistent_hash_m[tid][i] = nullptr;

 }

template<typename Net>
void uDepotNet<Net>::setup_consistent_hash_map(void)
{
	// Each server has an id. (Typically this is its address.)
	// We make a sorted list of these ids self+[other_peers] so that each peer
	// has the same mapping.
	// (NB: std::set is sorted)
	std::set<typename Net::ServerID> ordered_ids;

	// add our id if its valid (i.e., if we are not just a client)
	auto self_server_id = server_conf_m.get_server_id();
	if (self_server_id.is_valid())
		ordered_ids.insert(self_server_id);

	// add the ids from the other peers
	for (auto &c: client_confs_m)
		ordered_ids.insert(c.get_server_id());

	for (u32 cid = 0; cid < conn_m.size(); ++cid) {
		int idx = 0;
		for (auto &id : ordered_ids) {
			if (id == self_server_id)
				continue;
			for (auto vit = conn_m[cid].begin(); vit != conn_m[cid].end(); ++vit) {
				if (id == (*vit)->get_server_id())
					consistent_hash_m[cid][idx] = *vit;
			}
			++idx;
		}
	}
}

template<typename Net>
__attribute__((warn_unused_result))
int uDepotNet<Net>::init_socket_key_value_client(void) {
	for (u32 tid = 0; tid < consistent_hash_m.size(); ++tid) {
		for (auto &conf : client_confs_m) {
			Cli *cli = new Cli(base_m, kv_mbuff_m, conf);
			if (cli == nullptr) {
				UDEPOT_ERR("%s: new failed", __PRETTY_FUNCTION__);
				return ENOMEM;
			}
			int err = cli->start();
			if (err) {
				UDEPOT_ERR("Client initialization failed\n");
				return err;
			}
			conn_m[tid].push_back(cli);
		}
	}
	return 0;
}

template<typename Net>
__attribute__((warn_unused_result))
int uDepotNet<Net>::start_net_all(void) {
	int err;
	err = server_m.start();
	if (err) {
		UDEPOT_ERR("Initiliazing net server failed with: %d.\n", err);
		goto fail;
	}

	// 4. register remote Socket regions
	err = init_socket_key_value_client();
	if (err) {
		UDEPOT_ERR("init_socket_key_value_client failed with %d.\n", err);
		goto fail_stop_server;
	}

	setup_consistent_hash_map();
	return 0;

fail_stop_server:
	server_m.stop();
fail:
	return -1;
}

template<typename Net>
int uDepotNet<Net>::stop_clients(void) {
	int ret = 0;
	for (u32 tid = 0; tid < consistent_hash_m.size(); ++tid) {
		for (auto vit = conn_m[tid].begin(); vit != conn_m[tid].end(); ++vit) {
			Cli *client = *vit;
			int err = client->stop();
			if (err) {
				ret = err;
				UDEPOT_ERR("client->stop() returned: %d\n", err);
			}
		}
	}
	return ret;
}

template<typename Net>
int uDepotNet<Net>::stop_server(void) {
	return server_m.stop();
}

template<typename Net>
int uDepotNet<Net>::clear() {
	for (u32 tid = 0; tid < consistent_hash_m.size(); ++tid) {
		for (auto vit = conn_m[tid].begin(); vit != conn_m[tid].end(); ++vit)
			delete *vit;
		for (u32 i = 0; i < consistent_hash_m[tid].size(); ++i)
			consistent_hash_m[tid][i] = NULL;
		conn_m[tid].clear();
	}
	return 0; // TODO: proper error code
}

template<typename Net>
int uDepotNet<Net>::stop_net_all(void) {
	stop_clients();
	stop_server();
	clear();
	return 0; // TODO: proper error code
}

template class uDepotNet<SocketNet>;
template class uDepotNet<MemcacheNet>;
template class uDepotNet<MemcacheTrtNet>;
template class uDepotNet<TrtEpollNet>;

} // end namespace udepot
