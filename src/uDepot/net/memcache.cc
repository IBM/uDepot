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

#include "uDepot/net/memcache.hh"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <thread>
#include <netdb.h>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <type_traits> // is_standard_layout

#include "uDepot/net/mc-helpers.hh"
#include "uDepot/net/socket.hh"
#include "uDepot/kv-wire.hh"
#include "kv-mbuff.hh"
#include "uDepot/mbuff-cache.hh"
#include "util/sys-helpers.hh"
#include "util/debug.h"

namespace udepot {

// helpers

static inline std::tuple<std::string, std::string>
sock_get_ip_and_port(int sockfd)
{
	char host[128], port[128];
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	int ret;

	if (getsockname(sockfd, (struct sockaddr *)&addr, &addrlen) == -1) {
		perror("getsockname");
		abort();
	}

	if ((ret = getnameinfo((struct sockaddr *)&addr, addrlen,
	                host, sizeof(host),
	                port, sizeof(port),
	                NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
		UDEPOT_ERR("getnameinfo: error: %d (%s)", ret, gai_strerror(ret));
		abort();
	}

	return std::make_tuple(std::string(host), std::string(port));
}


// MemcacheNet
int MemcacheNet::global_init() { return 0; }
int MemcacheNet::thread_init() { return 0; }
int MemcacheNet::thread_exit() { return 0; }
int MemcacheNet::global_exit() { return 0; }


// MemcacheServer::Conf
const std::string MemcacheServer::Conf::DEFAULT_PORT = "11211"; // default memcache port is 11211
const std::string MemcacheServer::Conf::DEFAULT_PROTOCOL = "tcp";

MemcacheServer::Conf::Conf() : listen_conf_m() {}
MemcacheServer::Conf::Conf(SocketPeerConf lc) : listen_conf_m(lc) {}

int
MemcacheServer::Conf::from_string(std::string s) {
	listen_conf_m.service_m  = DEFAULT_PORT;
	listen_conf_m.protocol_m = DEFAULT_PROTOCOL;
	int ret = listen_conf_m.parse(s);
	return ret;
}

std::string
MemcacheServer::Conf::parse_format_help()
{
	std::stringstream sstream;
	sstream << SocketPeerConf::parse_format_help()
	        << "default port:" << DEFAULT_PORT     << std::endl
	        << "default prot:" << DEFAULT_PROTOCOL << std::endl;
	return sstream.str();
}

// MemcacheServer
MemcacheServer::MemcacheServer(MemcacheNet &unused, KV_MbuffInterface &kv, Conf cnf)
	: srv_kv_mbuff_m(kv),
	  mb_cache_m(kv),
	  srv_mbuff1_m(kv.mbuff_type_index()),
	  srv_mbuff2_m(kv.mbuff_type_index()),
	  srv_conf_m(cnf),
	  srv_thread_ready_m(false),
	  srv_thread_exit_m(false),
	  srv_listen_fd_m(-1),
	  srv_epoll_fd_m(-1)
	  { }

int
MemcacheServer::start(void)
{
	int rc = memcache::parser::start();
	if (0 != rc)
		return rc;

	if (!srv_conf_m.is_valid()) {
		UDEPOT_ERR("Invalid server configuration. Not starting server.");
		return -1;
	}

	UDEPOT_MSG("Staring server socket thread.");
	srv_thread_m = std::thread(&MemcacheServer::socket_key_value_server, this);
	while (!srv_thread_ready_m)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	return 0;
}

int
MemcacheServer::bind_and_listen(void)
{
	assert(srv_listen_fd_m == -1);
	struct addrinfo *rsai = srv_conf_m.listen_conf_m.getaddrinfo(true);
	srv_listen_fd_m = socket(rsai->ai_family, rsai->ai_socktype, rsai->ai_protocol);
	if (-1 == srv_listen_fd_m) {
		perror("socket");
		abort();
	}

	const int optval = 1;
	int rc = setsockopt(srv_listen_fd_m, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	if (-1 == rc) {
		perror("setsockopt");
		abort();
	}

	// try to bind it
	rc = bind(srv_listen_fd_m, rsai->ai_addr, rsai->ai_addrlen);
	freeaddrinfo(rsai); // no longer needed
	if (-1 == rc) {
		perror("bind");
		abort();
	}

	rc = listen(srv_listen_fd_m, 50);
	if (-1 == rc) {
		perror("listen");
		abort();
	}

	return 0;
}

void
MemcacheServer::remove_client(int cli_fd)
{
	UDEPOT_MSG("Removing client (fd=%d)", cli_fd);
	close(cli_fd);
	auto &v = srv_client_fds_m;
	v.erase(std::remove(v.begin(), v.end(), cli_fd), v.end());
}

void MemcacheServer::close_all(void)
{
	// close all client sockets
	for (auto vit = srv_client_fds_m.begin(); vit != srv_client_fds_m.end(); ++vit)
		close((*vit));

	// close listening server socket
	close(srv_epoll_fd_m);
}

void MemcacheServer::init_fds(void)
{
	int err;
	std::string srv_ip, srv_port;

	err = bind_and_listen();
	if (err) {
		UDEPOT_ERR("bind_and_listen() failed with %d.", err);
		abort();
	}

	std::tie(srv_ip, srv_port) = sock_get_ip_and_port(srv_listen_fd_m);
	if (srv_port != srv_conf_m.listen_conf_m.service_m) {
		UDEPOT_ERR("Failed to bind requested port %s, binded %s instead.\n",
			srv_conf_m.listen_conf_m.service_m.c_str(), srv_port.c_str());
		abort();	// TODO: gracefully close and return error
	}
	// ready for connections
	UDEPOT_MSG("Server listening on %s:%s", srv_ip.c_str(), srv_port.c_str());

	srv_epoll_fd_m = epoll_create(42 /* argument ignored in recent kernels */ );
	if (-1 == srv_epoll_fd_m) {
		perror("epool_create");
		abort();
	}

	// add an event for the listen socket
	struct epoll_event listen_ev = { 0 };
	listen_ev.events = EPOLLIN; //  | EPOLLOUT | EPOLLET;
	listen_ev.data.fd = srv_listen_fd_m;
	int rc = epoll_ctl(srv_epoll_fd_m, EPOLL_CTL_ADD, srv_listen_fd_m, &listen_ev);
	if (-1 == rc) {
		perror("epoll_ctl: EPOLL_CTL_ADD");
		abort();
	}
}

void MemcacheServer::socket_key_value_server(void)
{
	constexpr size_t max_epoll_events = 100;
	struct epoll_event events[max_epoll_events] = { { 0 } };
	std::atomic<u32> active;
	active = 0;
	// std::mutex mtx;
	std::vector<std::thread> threads;

	init_fds();
	srv_thread_ready_m = true;

	while(!srv_thread_exit_m || srv_client_fds_m.size() > 0 || active) {
		const s32 nfds = epoll_wait(srv_epoll_fd_m, events, max_epoll_events, 5000);
		if (-1 == nfds) {
			if (EINTR == errno)
				continue; // timeout
			UDEPOT_ERR("epoll_wait %s err=%d.\n", strerror(errno), errno);
			abort();
		}

		for (s32 n = 0; n < nfds; ++n) {
			int cfd = events[n].data.fd;
			if (srv_listen_fd_m == cfd) {
				struct sockaddr client_addr { 0 };
				socklen_t client_addr_len = { 0 };
				// control path request
				const s32 sfd = accept(srv_listen_fd_m, &client_addr, &client_addr_len);
				if (-1 == cfd) {
					UDEPOT_ERR("accept socket %s err=%d.\n", strerror(errno), errno);
					exit(errno);
				}
				const int optval = 1;
				int rc = setsockopt(sfd, SOL_TCP, TCP_NODELAY, &optval, sizeof(optval));
				if (-1 == rc) {
					UDEPOT_ERR("setsockopt %s err=%d.\n", strerror(errno), errno);
					abort();
				}
				active++;
				threads.push_back(std::thread(&MemcacheServer::handle_request_thread,
									this, sfd, &active, threads.size()));
				continue; // on with the next one
			}
		}
	}

	for (auto &thr : threads)
		if (thr.joinable())
			thr.join();

	// close listening server socket
	close(srv_listen_fd_m);
	// close_all();
}

void MemcacheServer::handle_request_thread(s32 cfd, std::atomic<u32> *active, const u32 tid)
{
	ConnectionSocket scon(cfd);
	UDEPOT_MSG("request handling thread request on new socket=%d tid=%u.", cfd, tid);
	memcache::msg_buff buff;
	Mbuff mbuff(srv_kv_mbuff_m.mbuff_type_index());
	Mbuff keymbuff(srv_kv_mbuff_m.mbuff_type_index());
	for (; !srv_thread_exit_m && -1 != cfd; ) {
		buff.reset();
		mbuff.reslice(0);

		memcache::cmd cmd = memcache::parser::read_cmd(scon, buff);
		if (0 == cmd.err_) {
			cmd.handle(scon, srv_kv_mbuff_m, mb_cache_m, mbuff, keymbuff);
			if (0 == cmd.err_)
				continue;
		}
		// only if error either at read_cmd, or handle
		if (ENOMEM == cmd.err_) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		if (ECONNRESET == cmd.err_)
			UDEPOT_MSG("recv on cfd=%d returned 0, deleting cfd.", cfd);
		else
			UDEPOT_ERR("unknown error %s %d on thread %u.", strerror(cmd.err_), cmd.err_, tid);
		break;
	}
	close(cfd);
	cfd = - 1;
	srv_kv_mbuff_m.mbuff_free_buffers(mbuff);
	srv_kv_mbuff_m.mbuff_free_buffers(keymbuff);

	(*active)--;
}

int MemcacheServer::stop(void)
{
	memcache::parser::stop();
	assert(!srv_thread_exit_m);
	srv_thread_exit_m = true;
	if (srv_thread_m.joinable())
		srv_thread_m.join();
	return 0;
}

MemcacheServer::~MemcacheServer()
{
	srv_thread_exit_m = true;
	if (srv_thread_m.joinable())
		srv_thread_m.join();
}

// MemcacheClient

MemcacheClient::Conf::Conf(SocketPeerConf server_conf) : connect_conf_m(server_conf) {}
MemcacheClient::Conf::Conf() : connect_conf_m() {}

int
MemcacheClient::Conf::from_string(std::string s) {
	connect_conf_m.service_m  = MemcacheServer::Conf::DEFAULT_PORT;
	connect_conf_m.protocol_m = MemcacheServer::Conf::DEFAULT_PROTOCOL;
	int ret = connect_conf_m.parse(s);
	return ret;
}

std::string
MemcacheClient::Conf::parse_format_help()
{
	std::stringstream sstream;
	sstream << SocketPeerConf::parse_format_help()
	        << "default port:" << MemcacheServer::Conf::DEFAULT_PORT << std::endl
	        << "default prot:" << MemcacheServer::Conf::DEFAULT_PROTOCOL << std::endl;
	return sstream.str();
}

MemcacheClient::MemcacheClient(MemcacheNet &unused, Conf cnf)
	: cli_conf_m(cnf),
	  cli_fd_m(-1) {}

MemcacheClient::MemcacheClient(MemcacheNet &unused, MbuffAllocIface &unused2, Conf cnf)
	: cli_conf_m(cnf),
	  cli_fd_m(-1) {}

int
MemcacheClient::start(void)
{
	return connect();
}

int
MemcacheClient::stop(void)
{
	if (is_connected())
		disconnect();
	return 0;
}

int
MemcacheClient::connect(void)
{
	int wait, rc;
	struct addrinfo *rsai;

	if (is_connected()) {
		UDEPOT_ERR("request to connect, but socket is valid.");
		return -1;
	}

	rsai = cli_conf_m.connect_conf_m.getaddrinfo(false);
	if (rsai == nullptr) {
		UDEPOT_ERR("%s: Not starting\n", __PRETTY_FUNCTION__);
		return -1;
	}

	cli_fd_m = socket(rsai->ai_family, rsai->ai_socktype, rsai->ai_protocol);
	if (cli_fd_m == -1) {
		perror("socket");
		goto error_addrinfo;
	}

	wait = 0;
	do {
		rc = ::connect(cli_fd_m, rsai->ai_addr, rsai->ai_addrlen);
		if (-1 == rc && ECONNREFUSED == errno && ++wait < 100) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			rc = ECONNREFUSED;
		}
	} while(ECONNREFUSED == rc);

	if (0 != rc) {
		UDEPOT_MSG("connecting to server=%s at port=%s failed err=%d (%s)\n",
			cli_conf_m.connect_conf_m.node_m.c_str(),
			cli_conf_m.connect_conf_m.service_m.c_str(),
			errno, strerror(errno));
		goto error_socket;
	}

	freeaddrinfo(rsai); // no longer needed

	UDEPOT_MSG("Connected to server %s:%s.\n",
		cli_conf_m.connect_conf_m.node_m.c_str(),
		cli_conf_m.connect_conf_m.service_m.c_str());
	return 0;

error_socket:
	close(cli_fd_m);
	cli_fd_m = -1;

error_addrinfo:
	assert(rsai != nullptr);
	freeaddrinfo(rsai);
	return -1;
}

int
MemcacheClient::disconnect(void)
{
	int err;
	if (cli_fd_m == -1) {
		UDEPOT_ERR("%s: request to disconnect, but socket is invalid. Bailing out", __PRETTY_FUNCTION__);
		return -1;
	}
	err = close(cli_fd_m);
	if (err) {
		UDEPOT_ERR("%s: close() %s\n",  __PRETTY_FUNCTION__, strerror(errno));
	}
	cli_fd_m = -1;
	return err;
}

} // end namespace udepot
