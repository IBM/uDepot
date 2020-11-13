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

#include <algorithm>
#include <cstring>
#include <cassert>
#include <netdb.h>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <type_traits> // is_standard_layout

#include "uDepot/kv-wire.hh"
#include "kv-mbuff.hh"
#include "uDepot/net/socket.hh"
#include "uDepot/net/connection.hh"
#include "uDepot/net/serve-kv-request.hh"
#include "uDepot/net/helpers.hh"
#include "util/sys-helpers.hh"
#include "util/debug.h"

namespace udepot {


// SocketNet
int SocketNet::global_init() { return 0; }
int SocketNet::thread_init() { return 0; }
int SocketNet::thread_exit() { return 0; }
int SocketNet::global_exit() { return 0; }

// SocketServer::Conf
const std::string SocketServer::Conf::DEFAULT_PORT = "43345";
const std::string SocketServer::Conf::DEFAULT_PROTOCOL = "tcp";

SocketServer::Conf::Conf() : listen_conf_m() {}
SocketServer::Conf::Conf(SocketPeerConf lc) : listen_conf_m(lc) {}

int
SocketServer::Conf::from_string(std::string s) {
	listen_conf_m.service_m  = DEFAULT_PORT;
	listen_conf_m.protocol_m = DEFAULT_PROTOCOL;
	int ret = listen_conf_m.parse(s);
	return ret;
}

std::string
SocketServer::Conf::parse_format_help()
{
	std::stringstream sstream;
	sstream << SocketPeerConf::parse_format_help()
	        << "default port:" << DEFAULT_PORT     << std::endl
	        << "default prot:" << DEFAULT_PROTOCOL << std::endl;
	return sstream.str();
}

// SocketServer
SocketServer::SocketServer(SocketNet &unused, KV_MbuffInterface &kv, Conf cnf)
	: srv_kv_mbuff_m(kv),
	  srv_mbuff1_m(kv.mbuff_type_index()),
	  srv_mbuff2_m(kv.mbuff_type_index()),
	  srv_conf_m(cnf),
	  srv_thread_ready_m(false),
	  srv_thread_exit_m(false),
	  srv_listen_fd_m(-1),
	  srv_epoll_fd_m(-1)
	  { }

int
SocketServer::start(void)
{
	if (srv_conf_m.listen_conf_m.is_invalid()) {
		// this is valid, could be without server
		return 0;
		#if	0
		UDEPOT_ERR("Invalid listen configuration. Not starting server.");
		return -1;
		#endif
	}

	UDEPOT_MSG("Staring server socket thread.");
	srv_thread_m = std::thread(&SocketServer::socket_key_value_server, this);
	while (!srv_thread_ready_m)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	return 0;
}

int
SocketServer::bind_and_listen(void)
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

int
SocketServer::accept_client(void)
{
	char host[128], port[128];
	struct sockaddr_storage client_addr = {0};
	socklen_t client_addr_len = 0;
	struct epoll_event ev = {0};
	int cli_fd, ret;

	client_addr_len = sizeof(client_addr);
	cli_fd = accept(srv_listen_fd_m, (struct sockaddr *)&client_addr, &client_addr_len);
	if (-1 == cli_fd) {
		perror("accept");
		abort();
	}

	if ((ret = getnameinfo((struct sockaddr *)&client_addr, client_addr_len,
	                host, sizeof(host),
	                port, sizeof(port),
	                NI_NUMERICHOST | NI_NUMERICSERV)) != 0) {
		UDEPOT_ERR("getnameinfo for client: error: %d (%s)", ret, gai_strerror(ret));
	} else
		UDEPOT_DBG("Accepted connection from: %s:%s (fd=%d)", host, port, cli_fd);

	ev.events = EPOLLIN; //  | EPOLLOUT | EPOLLET;
	ev.data.fd = cli_fd;
	int rc = epoll_ctl(srv_epoll_fd_m, EPOLL_CTL_ADD, cli_fd, &ev);
	if (-1 == rc) {
		perror("epoll_ctl");
		abort();
	}

	srv_client_fds_m.push_back(cli_fd);
	return 0;
}

void
SocketServer::remove_client(int cli_fd)
{
	UDEPOT_DBG("Removing client (fd=%d)", cli_fd);
	close(cli_fd);
	auto &v = srv_client_fds_m;
	v.erase(std::remove(v.begin(), v.end(), cli_fd), v.end());
}

void SocketServer::close_all(void)
{
	// close all client sockets
	for (auto vit = srv_client_fds_m.begin(); vit != srv_client_fds_m.end(); ++vit)
		close((*vit));

	// close listening server socket
	close(srv_epoll_fd_m);
}

void SocketServer::init_fds(void)
{
	int err;
	std::string srv_ip, srv_port;

	err = bind_and_listen();
	if (err) {
		UDEPOT_ERR("%s: bind_and_listen() failed.", __PRETTY_FUNCTION__);
		abort();
	}

	std::tie(srv_ip, srv_port) = sock_get_local_address(srv_listen_fd_m);
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

// returns error or 0
int SocketServer::recv_from_client(int cli_fd)
{
	ConnectionSocket cli(cli_fd);
	int err = serve_kv_request(cli, srv_kv_mbuff_m, srv_mbuff1_m, srv_mbuff2_m);
	if (err)
		remove_client(cli_fd);
	// ECONNRESET is used to mark that we got an EOF. No real error.
	if (err == ECONNRESET)
		err = 0;
	return err;
}

void SocketServer::socket_key_value_server(void)
{
	constexpr size_t max_epoll_events = 100;
	struct epoll_event events[max_epoll_events] = { { 0 } };

	init_fds();
	srv_thread_ready_m = true;

	while(!srv_thread_exit_m || srv_client_fds_m.size() > 0) {
		const s32 nfds = epoll_wait(srv_epoll_fd_m, events, max_epoll_events, 5000);
		if (-1 == nfds) {
			if (EINTR == errno)
				continue; // timeout
			perror("epoll_wait");
			abort();
		}

		for (s32 n = 0; n < nfds; ++n) {
			int cfd = events[n].data.fd;
			if (srv_listen_fd_m == cfd) {
				// control path request
				int err = accept_client();
				if (err) {
					UDEPOT_ERR("%s: accept_client() failed\n", __PRETTY_FUNCTION__);
					abort();
				}
				continue; // on with the next one
			}
			// read the request
			UDEPOT_DBG("checking epoll request on socket=%d.", cfd);
			int err = recv_from_client(cfd);
			if (err) {
				UDEPOT_MSG("recv_from_client failed: %s (%d)\n", strerror(err), err);
			}
		}
	}

	close_all();
}

int SocketServer::stop(void)
{
	assert(!srv_thread_exit_m);
	srv_thread_exit_m = true;
	if (srv_thread_m.joinable())
		srv_thread_m.join();
	return 0;
}

SocketServer::~SocketServer()
{
	srv_thread_exit_m = true;
	if (srv_thread_m.joinable())
		srv_thread_m.join();
}

// SocketClient

SocketClient::Conf::Conf(SocketPeerConf server_conf) : connect_conf_m(server_conf) {}
SocketClient::Conf::Conf() : connect_conf_m() {}

int
SocketClient::Conf::from_string(std::string s) {
	connect_conf_m.service_m  = SocketServer::Conf::DEFAULT_PORT;
	connect_conf_m.protocol_m = SocketServer::Conf::DEFAULT_PROTOCOL;
	int ret = connect_conf_m.parse(s);
	return ret;
}

std::string
SocketClient::Conf::parse_format_help()
{
	std::stringstream sstream;
	sstream << SocketPeerConf::parse_format_help()
	        << "default port:" << SocketServer::Conf::DEFAULT_PORT << std::endl
	        << "default prot:" << SocketServer::Conf::DEFAULT_PROTOCOL << std::endl;
	return sstream.str();
}

SocketClient::SocketClient(SocketNet &unused, MbuffAllocIface &mb_alloc, Conf cnf)
	: cli_conf_m(cnf),
	  cli_fd_m(-1),
	  cli_req_id_m(0),
	  cli_mb_alloc_m(mb_alloc),
	  cli_local_addr_m(std::string(), std::string()),
	  cli_remote_addr_m(std::string(), std::string()) { }


bool SocketClient::is_disconnected(void)
{
	return cli_fd_m == -1;
}

uint64_t SocketClient::get_next_reqid(void)
{
	return cli_req_id_m++;
}

int SocketClient::connect(void)
{
	int rc;
	struct addrinfo *rsai;
	const int optval = 1;

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

	rc = setsockopt(cli_fd_m, SOL_TCP, TCP_NODELAY, &optval, sizeof(optval));
	if (-1 == rc) {
		UDEPOT_ERR("setsockopt %s err=%d.\n", strerror(errno), errno);
		goto error_socket;
	}

	// retry if we get a "Connection refused errors" for a number of times
	for (size_t retries = 0; retries < 10; retries++) {
		rc = ::connect(cli_fd_m, rsai->ai_addr, rsai->ai_addrlen);
		if (rc == 0 && rc != ECONNREFUSED)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}

	if (0 != rc) {
		UDEPOT_MSG("connecting to server=%s at port=%s failed err=%d (%s)\n",
			cli_conf_m.connect_conf_m.node_m.c_str(),
			cli_conf_m.connect_conf_m.service_m.c_str(),
			errno, strerror(errno));
		goto error_socket;
	}

	freeaddrinfo(rsai); // no longer needed

	cli_local_addr_m   = sock_get_local_address(cli_fd_m);
	cli_remote_addr_m = sock_get_remote_address(cli_fd_m);

	UDEPOT_DBG("Connected %s", to_string().c_str());
	return 0;

error_socket:
	close(cli_fd_m);
	cli_fd_m = -1;

error_addrinfo:
	assert(rsai != nullptr);
	freeaddrinfo(rsai);
	return -1;
}

std::string
SocketClient::to_string(void) {
	std::stringstream ss;
	ss << "fd:" << cli_fd_m << " "
	   <<  "<local:"  << std::get<0>(cli_local_addr_m)
	   <<  ":"        << std::get<1>(cli_local_addr_m)
	   <<  ",remote:" << std::get<0>(cli_remote_addr_m)
	   <<  ":"        << std::get<1>(cli_remote_addr_m)
	   << ">";
	return ss.str();
}

int
SocketClient::disconnect(void)
{
	int err;

	UDEPOT_DBG("Calling disconnect on %s", to_string().c_str());

	cli_local_addr_m  = std::make_tuple(std::string(), std::string());
	cli_remote_addr_m  = std::make_tuple(std::string(), std::string());

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

int
SocketClient::start(void)
{
	return connect();
}

int SocketClient::stop(void)
{
	if (is_connected())
		disconnect();
	return 0;
}

int SocketClient::recvResHdr(ResHdr *reshdr, uint64_t req_id) {
	int err;
	std::tie(err, std::ignore) = recv_full(cli_fd_m, reshdr, sizeof(*reshdr), 0);
	if (-1 == err) {
		UDEPOT_ERR("recv on fd=%d failed: %s (%d)", cli_fd_m, strerror(errno), errno);
		return errno;
	} else if (!reshdr->verify_magic()) {
		UDEPOT_ERR("Wrong magic number\n");
		return EBADMSG;
	} else if (reshdr->req_id != req_id) {
		UDEPOT_ERR("Wrong request id\n");
		return EINVAL;
	}
	return 0;
}

// NB: This receives a ResGetHdr, while recvResHdr receives a ResHdr
int SocketClient::recvGetResHdr(ResGetHdr *reshdr, uint64_t req_id) {
	int err;
	std::tie(err, std::ignore) = recv_full(cli_fd_m, reshdr, sizeof(*reshdr), 0);
	if (-1 == err) {
		UDEPOT_ERR("recv failed: %s (%d)\n", strerror(errno), errno);
		return errno;
	} else if (!reshdr->hdr.verify_magic()) {
		UDEPOT_ERR("Wrong magic number\n");
		return EBADMSG;
	} else if (reshdr->hdr.req_id != req_id) {
		UDEPOT_ERR("Wrong request id\n");
		return EINVAL;
	}
	return 0;
}

// send request
//   todo: use sendmsg to send header+body with a single syscall
int SocketClient::sendReq(ReqHdr const& reqhdr, Mbuff const& body)
{
    int err;
	// send request header
	std::tie(err, std::ignore) = send_full(cli_fd_m, &reqhdr, sizeof(reqhdr), MSG_MORE);
	if (-1 == err) {
		UDEPOT_ERR("hdr send failed: %s (%d)\n", strerror(errno), errno);
		return errno;
	}

	// send request body
	err = body.send(cli_fd_m, 0 /* offset */, body.get_valid_size(), MSG_NOSIGNAL);
	if (err) {
		UDEPOT_ERR("body send failed: %s (%d)\n", strerror(err), err);
		return err;
	}

	return 0;
}

int SocketClient::nop(void) {
    if (is_disconnected())
        return EBADF;

    int err;

    // send request
    uint64_t myreqid = get_next_reqid();
    ReqHdr reqhdr = ReqHdr::mkNOP(myreqid);
	std::tie(err, std::ignore) = send_full(cli_fd_m, &reqhdr, sizeof(reqhdr), 0);
	if (-1 == err) {
		UDEPOT_ERR("hdr send failed: %s (%d)\n", strerror(errno), errno);
		return errno;
	}

	// recv reply
	ResHdr reshdr;
	err = recvResHdr(&reshdr, myreqid);
	if (err) {
		disconnect();
		return err;
	}

	return reshdr.error;
}

int SocketClient::remote_get(Mbuff const& key, Mbuff &val_out) {
    if (is_disconnected())
        return EBADF;

	// create header and send request
    size_t key_size  = key.get_valid_size();
    uint64_t myreqid = get_next_reqid();
    int err;

	ReqHdr reqhdr = ReqHdr::mkGET(myreqid, key_size);
	err = sendReq(reqhdr, key);
	if (err) {
		disconnect();
		return err;
	}

	// recv GET header
	ResGetHdr reshdr;
	err = recvGetResHdr(&reshdr, myreqid);
	if (err) {
		disconnect();
		return err;
	} else if (reshdr.hdr.error) {
		return reshdr.hdr.error;
	}

	// receive value
	cli_mb_alloc_m.mbuff_add_buffers(val_out, reshdr.val_len);
	if (val_out.get_free_size() < reshdr.val_len) {
		UDEPOT_ERR("recv failed: Cannot add buffers to mbuff (available size:%zd, need:%zd) \n", val_out.get_free_size(), reshdr.val_len);
		disconnect();
		return ENOMEM;
	}

	err = val_out.append_recv(cli_fd_m, reshdr.val_len, 0 /* flags */);
	if (err) {
		UDEPOT_ERR("recv failed: %s (%d)\n", strerror(err), err);
		disconnect();
		return err;
	}

    return 0;
}

int SocketClient::remote_put(Mbuff &keyval, size_t key_len) {
	assert(keyval.get_valid_size() > key_len); // NB: value should not be 0 length
	if (is_disconnected())
		return EBADF;

	// create header and send request
	uint64_t myreqid = get_next_reqid();
	size_t val_len = keyval.get_valid_size() - key_len;
	int err;
	ReqHdr reqhdr = ReqHdr::mkPUT(myreqid, key_len, val_len);
	err = sendReq(reqhdr, keyval);
	if (err) {
		disconnect();
		return err;
	}

	// recv header
	ResHdr reshdr;
	err = recvResHdr(&reshdr, myreqid);
	if (err) {
		disconnect();
		return err;
	}

	return reshdr.error;
}

int SocketClient::remote_del(Mbuff const& key) {
	return ENOTSUP;
}

} // end namespace udepot
