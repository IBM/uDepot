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

#ifndef _UDEPOT_NET_TRT_EPOLL_HH_
#define _UDEPOT_NET_TRT_EPOLL_HH_

#include "util/socket-peer-conf.hh"
#include "uDepot/net.hh"

namespace udepot {

class TrtEpollNet;
class TrtEpollClient;
class TrtEpollServer;

class TrtEpollNet {
public:
	typedef TrtEpollServer Server;
	typedef TrtEpollClient Client;
	typedef SocketPeerConf ServerID;

	class Conf {};

	TrtEpollNet(Conf conf);

	int global_init();
	int thread_init();
	int thread_exit();
	int global_exit();

	~TrtEpollNet() {}
};

class TrtEpollServer {
public:
	class Conf {
		friend TrtEpollServer;
	public:
		static const std::string DEFAULT_PORT;
		static const std::string DEFAULT_PROTOCOL;

		//Conf(SocketPeerConf listen_conf);
		Conf();

		int from_string(std::string s);
		static std::string parse_format_help();

	protected:
		SocketPeerConf  listen_conf_m;

	public:
		SocketPeerConf get_server_id() const {
			return listen_conf_m;
		}

		bool is_valid(void) const {
			return listen_conf_m.is_valid();
		}
	};
public:
	TrtEpollServer(TrtEpollNet &net, KV_MbuffInterface &kv_mbuff, Conf cnf);

	SocketPeerConf get_server_id() const {
		return srv_conf_m.get_server_id();
	}

	int start();
	int stop();

private:
	Conf                srv_conf_m;
	KV_MbuffInterface  &srv_kv_m;
	int                 srv_listen_fd_m; // listen file descriptor
};



class TrtEpollClient {
public:
	class Conf {
		friend TrtEpollClient;
	public:
		Conf();
	private:
		Conf(SocketPeerConf server_conf);
	public:

		int from_string(std::string s);
		static std::string parse_format_help();
	private:
		SocketPeerConf   connect_conf_m;

	public:
		SocketPeerConf get_server_id() const { return connect_conf_m; };
	};

	TrtEpollClient(TrtEpollNet &net, MbuffAllocIface &mb_alloc, Conf cnf);

private:
	Conf              cli_conf_m;

public:
	int start();
	int stop();

	int remote_get(Mbuff const& key, Mbuff &val_out) { return ENOTSUP; }
	int remote_put(Mbuff &keyval, size_t key_len)    { return ENOTSUP; }
	int remote_del(Mbuff const& key)                 { return ENOTSUP; }

	SocketPeerConf get_server_id() const {
		return cli_conf_m.get_server_id();
	}

protected:
};

static_assert(NetCheck<TrtEpollNet>(), "");

} // end namespace udepot



#endif /* ifndef _UDEPOT_NET_TRT_EPOLL_HH_ */
