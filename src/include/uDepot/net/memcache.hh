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

#ifndef _UDEPOT_NET_MEMCACHED_HH_
#define _UDEPOT_NET_MEMCACHED_HH_


#include <atomic>
#include <thread>
#include <vector>

#include "kv-mbuff.hh"
#include "uDepot/sync.hh"
#include "uDepot/mbuff-cache.hh"
#include "util/socket-peer-conf.hh"
#include "uDepot/net.hh"

namespace udepot {

class MemcacheNet;
class MemcacheServer;
class MemcacheClient;

class MemcacheNet {
public:
	typedef MemcacheServer Server;
	typedef MemcacheClient Client;
	typedef SocketPeerConf ServerID;

	class Conf {};

	MemcacheNet(Conf conf) {}

	int global_init();
	int thread_init();
	int thread_exit();
	int global_exit();

	~MemcacheNet() {}
};

class MemcacheServer {
public:
	class Conf;
	MemcacheServer(MemcacheNet &net, KV_MbuffInterface &kv_mbuff, Conf cnf);

	int start();
	int stop();

	SocketPeerConf get_server_id() const {
		return srv_conf_m.get_server_id();
	}

	~MemcacheServer();


	class Conf {
		friend MemcacheServer;
	public:
		static const std::string DEFAULT_PORT;
		static const std::string DEFAULT_PROTOCOL;

		//Conf(MemcacheServer &&);
		Conf(SocketPeerConf listen_conf);
		Conf();

		// build configuration from string
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
	KV_MbuffInterface  &srv_kv_mbuff_m;
	MbuffCache<PthreadLock>mb_cache_m;
	Mbuff              srv_mbuff1_m;
	Mbuff              srv_mbuff2_m;
	Conf               srv_conf_m;
	std::thread        srv_thread_m;
	std::atomic<bool>  srv_thread_ready_m;
	std::atomic<bool>  srv_thread_exit_m;
	// accessed by the server thread
	int                srv_listen_fd_m; // listen file descriptor
	int                srv_epoll_fd_m;  // epoll file descriptor
	std::vector<int>   srv_client_fds_m;

	// called in the server thread
	void socket_key_value_server(void);
	int bind_and_listen(void);
	void init_fds(void);
	int accept_client(void);
	void remove_client(int cli_fd);
	void close_all(void);

	int recv_from_client(int cli_fd);
	void handle_request_thread(s32, std::atomic<u32> *, u32);
};

// Memcache client not implemented for uDepot
class MemcacheClient {
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
		friend MemcacheClient;
	public:
		SocketPeerConf get_server_id() const { return connect_conf_m; };
		Conf();
		//Conf(MemcacheClient &&);
		Conf(SocketPeerConf server_conf);

		// build configuration from string
		int from_string(std::string s);
		static std::string parse_format_help();


	private:
		SocketPeerConf   connect_conf_m;

	};

	MemcacheClient(MemcacheNet &net, Conf cnf);
	MemcacheClient(MemcacheNet &net, MbuffAllocIface &mb_alloc, Conf cnf);

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

static_assert(NetCheck<MemcacheNet>(), "");

} // end namespace uDepot

#endif // _UDEPOT_NET_MEMCACHED_HH_
