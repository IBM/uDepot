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

#ifndef _UDEPOT_NET_MEMCACHED_TRT_HH_
#define _UDEPOT_NET_MEMCACHED_TRT_HH_

#include <atomic>
#include <thread>

#include "util/socket-peer-conf.hh"
#include "uDepot/net.hh"

namespace udepot {

class MemcacheTrtNet;
class MemcacheTrtClient;
class MemcacheTrtServer;

class MemcacheTrtNet {
public:
	typedef MemcacheTrtServer Server;
	typedef MemcacheTrtClient Client;
	typedef SocketPeerConf    ServerID;

	class Conf {};

	MemcacheTrtNet(Conf conf);
	~MemcacheTrtNet() {}

	int global_init();
	int thread_init();
	int thread_exit();
	int global_exit();
};

class MemcacheTrtServer {
public:
	class Conf;
	MemcacheTrtServer(MemcacheTrtNet &net, KV_MbuffInterface &kv_mbuff, Conf cnf);
        ~MemcacheTrtServer();

	int start();
	int stop();

	SocketPeerConf get_server_id() const {
		return srv_conf_m.get_server_id();
	}

	size_t get_net_cap() const {
		return srv_conf_m.listen_conf_m.net_cap_bytes_m;
	}

	class Conf {
		friend MemcacheTrtServer;
	public:
		static const std::string DEFAULT_PORT;
		static const std::string DEFAULT_PROTOCOL;

		Conf(SocketPeerConf listen_conf);
		Conf();

		int from_string(std::string s);
		static std::string parse_format_help();
		SocketPeerConf get_server_id() const {
			return listen_conf_m;
		}

		bool is_valid(void) {
			return listen_conf_m.is_valid();
		}

	protected:
		SocketPeerConf  listen_conf_m;
	};
private:
	Conf                srv_conf_m;
	KV_MbuffInterface  &srv_kv_m;
	std::atomic<bool>   srv_thread_exit_m;
	std::thread         srv_stats_thr_m;
	int                 srv_listen_fd_m; // listen file descriptor
	void mc_stats(void);
};


class MemcacheTrtClient {
public:
	class Conf;
	int start();
	int stop();

	// needed by consistent mapping
	Conf get_conf() {
		return cli_conf_m;
	}

	int get_sfd() const { return cli_fd_m; }
	class Conf {
		friend MemcacheTrtClient;
	public:
		SocketPeerConf get_server_id() const { return connect_conf_m; };
		Conf();
		//Conf(MemcacheTrtClient &&);
		Conf(SocketPeerConf server_conf);

		// build configuration from string
		int from_string(std::string s);
		static std::string parse_format_help();


	private:
		SocketPeerConf   connect_conf_m;

	};

	MemcacheTrtClient(MemcacheTrtNet &net, MbuffAllocIface &mb_alloc, Conf cnf);

	SocketPeerConf get_server_id() const {
		return cli_conf_m.get_server_id();
	}

	int remote_get(Mbuff const& key, Mbuff &val_out) { return ENOTSUP; }
	int remote_put(Mbuff &keyval, size_t key_len)    { return ENOTSUP; }
	int remote_del(Mbuff const& key)                 { return ENOTSUP; }

private:
	// The current policy is that whenever an error is occured (e.g,. socket
	// error, or received unexpected packet) we close the socket and move to
	// "disconnected" mode.
	bool is_disconnected(void) const {
		return cli_fd_m == -1;
	}
	bool is_connected(void) { return !is_disconnected(); }
	// connect() or reconect()
	int connect();
	int disconnect();
	Conf              cli_conf_m;
	int               cli_fd_m;
};

static_assert(NetCheck<MemcacheTrtNet>(), "");

} // end namespace udepot

#endif /* ifndef _UDEPOT_NET_MEMCACHED_TRT_HH_ */
