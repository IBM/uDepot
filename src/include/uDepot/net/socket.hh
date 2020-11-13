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

#ifndef _UDEPOT_NET_SOCKET_HH_
#define _UDEPOT_NET_SOCKET_HH_

#include <atomic>
#include <thread>
#include <vector>
#include <tuple>

#include "kv.hh"
#include "uDepot/kv-wire.hh"
#include "kv-mbuff.hh"
#include "util/socket-peer-conf.hh"
#include "uDepot/net.hh"

namespace udepot {

class SocketNet;
class SocketServer;
class SocketClient;

class SocketNet {
public:
	typedef SocketServer Server;
	typedef SocketClient Client;
	typedef SocketPeerConf ServerID;

	class Conf {};

	SocketNet(Conf conf) {}

	int global_init();
	int thread_init();
	int thread_exit();
	int global_exit();

	~SocketNet() {}
};

class SocketServer {
public:
	class Conf {
		friend SocketServer;
	public:
		static const std::string DEFAULT_PORT;
		static const std::string DEFAULT_PROTOCOL;

		//Conf(SocketServer &&);
		Conf(SocketPeerConf listen_conf);
		Conf();

		// build configuration from string
		int from_string(std::string s);
		static std::string parse_format_help();

	protected:
		SocketPeerConf  listen_conf_m;

	public:
		SocketPeerConf get_server_id() const {
			return listen_conf_m;
		}

		bool is_valid(void) {
			return listen_conf_m.is_valid();
		}
	};

private:
	KV_MbuffInterface  &srv_kv_mbuff_m;
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

public:
	SocketServer(SocketNet &net, KV_MbuffInterface &kv_mbuff, Conf cnf);

	int start();
	int stop();

	SocketPeerConf get_server_id() const {
		return srv_conf_m.get_server_id();
	}

	~SocketServer();

private:
	// called in the server thread
	void socket_key_value_server(void);
	int bind_and_listen(void);
	void init_fds(void);
	int accept_client(void);
	void remove_client(int cli_fd);
	void close_all(void);

	int recv_from_client(int cli_fd);
	int send_reply(int cli_fd, ResHdr const& hdr);
	int send_reply(int cli_fd, ResGetHdr const& hdr, Mbuff &val);
	int recv_body(int cli_fd, size_t body_size, Mbuff &body);
	//void handle_msg(udepot_sock_msg *msg);
};

class SocketClient {
public:
	class Conf {
		friend SocketClient;
	public:
		Conf();
		//Conf(SocketClient &&);
		Conf(SocketPeerConf server_conf);

		// build configuration from string
		int from_string(std::string s);
		static std::string parse_format_help();


	private:
		SocketPeerConf   connect_conf_m;

	public:
		SocketPeerConf get_server_id() const { return connect_conf_m; };
	};

	SocketClient(SocketNet &net, MbuffAllocIface &mbuff_alloc, Conf cnf);

	SocketPeerConf get_server_id() const {
		return cli_conf_m.get_server_id();
	}

private:
	Conf              cli_conf_m;
	int               cli_fd_m;
	uint64_t          cli_req_id_m;
	MbuffAllocIface  &cli_mb_alloc_m;

	std::tuple<std::string, std::string> cli_local_addr_m;
	std::tuple<std::string, std::string> cli_remote_addr_m;

public:
	int start();
	int stop();

	uint64_t         get_next_reqid();

	// The current policy is that whenever an error is occured (e.g,. socket
	// error, or received unexpected packet) we close the socket and move to
	// "disconnected" mode.
	bool is_disconnected(void);
	bool is_connected(void) { return !is_disconnected(); }
	// connect() or reconect()
	int connect();
	int disconnect();

	int remote_get(Mbuff const& key, Mbuff &val_out);
	int remote_put(Mbuff &keyval, size_t key_len);
	int remote_del(Mbuff const& key);
	int nop(void);

	// for debugging
	std::string to_string(void);

protected:
	int sendReq(ReqHdr const& reqhdr, Mbuff const& body);
	// receive a ResHdr. Returns 0 or -error
	int recvResHdr(ResHdr *hdr, uint64_t req_id);
	int recvGetResHdr(ResGetHdr *hdr, uint64_t req_id);
};

static_assert(NetCheck<SocketNet>(), "");

} // end namespace uDepot

#endif // _UDEPOT_NET_SOCKET_HH_
