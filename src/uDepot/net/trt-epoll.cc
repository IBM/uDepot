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

#include <sstream>
#include <typeindex>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_epoll.hh"
#include "trt_util/arg_pool.hh"
extern "C" {
	#include "trt_util/net_helpers.h"
}

#include "util/socket-peer-conf.hh"
#include "uDepot/net/serve-kv-request.hh"
#include "uDepot/net/trt-epoll.hh"
#include "uDepot/net/helpers.hh"
#include "util/debug.h"
#include "util/profiling.hh"

#ifndef SO_EE_ORIGIN_ZEROCOPY
#define SO_EE_ORIGIN_ZEROCOPY           5
#endif

#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY     60
#endif

#ifndef SO_EE_CODE_ZEROCOPY_COPIED
#define SO_EE_CODE_ZEROCOPY_COPIED      1
#endif

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY    0x4000000
#endif

namespace udepot {

struct TaskArg {
	KV_MbuffInterface &srv_kv;
	Mbuff mb1, mb2;

	// (host, port) strings, kept for helpful debugging messages
	std::tuple<std::string,std::string> peer_addr;

	TaskArg(KV_MbuffInterface &kv)
	    : srv_kv(kv)
	    , mb1(kv.mbuff_type_index())
	    , mb2(kv.mbuff_type_index()) {}
};
using TaskArgPool = ArgPool<TaskArg>;

// ensure that tread_init() is not called twice, etc.
static thread_local bool t_initialized__ = false;
// server interface
KV_MbuffInterface *kv__(nullptr);
// pool for task arguments
static thread_local std::unique_ptr<ArgPool<TaskArg>> TaskArgPool__(nullptr);
const size_t TASK_ARG_POOL_SIZE = 1024;

// TODO: de-duplicate this
class ConnectionTrtEpoll : public ConnectionBase {
public:
	ConnectionTrtEpoll() : fd_(-1) {}
	ConnectionTrtEpoll(int fd): fd_(fd) {}
	ConnectionTrtEpoll(ConnectionTrtEpoll const&) = delete;
	void operator=(ConnectionTrtEpoll const&) = delete;

	virtual ssize_t send(const void *buff, size_t len, int flags) override final {
		return trt::Epoll::send(fd_, buff, len, flags);
	}
	virtual ssize_t recv(void *buff, size_t len, int flags) override final {
		return trt::Epoll::recv(fd_, buff, len, flags);
	}

	virtual ssize_t sendmsg(const struct msghdr *msg, int flags) override final {
		//flags |= MSG_ZEROCOPY;
		return trt::Epoll::sendmsg(fd_, msg, flags);
	}

	virtual ssize_t recvmsg(struct msghdr *msg, int flags) override final {
		return trt::Epoll::recvmsg(fd_, msg, flags);
	}

private:
	int fd_;
};

//-----------------------------------------------------------------------------
// Net
//


TrtEpollNet::TrtEpollNet(Conf cnf) {}

int TrtEpollNet::global_init(void) {
	return 0;
}

int TrtEpollNet::thread_init(void) {
	// initialize epoll thread-local state and spawn poller task
	assert(!t_initialized__);
	assert(trt::T::in_trt());
	trt::Epoll::init();
	trt::Epoll::set_wait_timeout(100 /* ms */);
	trt::T::spawn(trt::Epoll::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);

	// create thread-local task argument pool
	assert(TaskArgPool__ == nullptr);
	assert(kv__ != nullptr);
	void *targs_raw = std::malloc(sizeof(TaskArg)*TASK_ARG_POOL_SIZE);
	if (!targs_raw)
		return ENOMEM;

	TaskArg *targs = static_cast<TaskArg *>(targs_raw);
	for (size_t i=0; i<TASK_ARG_POOL_SIZE; i++) {
		new (targs + i) TaskArg(*kv__);
	}

	auto release_fn = [] (TaskArg *args) {
		for (size_t i=0; i<TASK_ARG_POOL_SIZE; i++) {
			TaskArg *a = args + i;
			a->~TaskArg();
		}
		std::free(args);
	};

	TaskArgPool *targs_pool = new TaskArgPool(TASK_ARG_POOL_SIZE, targs, release_fn);
	TaskArgPool__ = std::unique_ptr<TaskArgPool>(targs_pool);

	t_initialized__ = true;
	return 0;
}

int TrtEpollNet::thread_exit(void) {
	assert(t_initialized__);
	trt::Epoll::stop();
	TaskArgPool__ = nullptr;
	t_initialized__ = false;
	return 0;
}

int TrtEpollNet::global_exit(void) {
	return 0;
}


//-----------------------------------------------------------------------------
// Server
//


TrtEpollServer::Conf::Conf() : listen_conf_m() { }

static void *
task_serve(void *arg)
{
	int fd = (int)(uintptr_t)arg;
	TaskArg *targ;

	for (;;) {
		// try to get a TaskArg from the pool. If we can't, yield.
		targ = TaskArgPool__->get_arg();
		if (targ)
			break;
		// if this happens too often, something's wrong. have a message for now
		UDEPOT_MSG("Cannot get TaskArg. Yielding");
		trt::T::yield();
	}

	ConnectionTrtEpoll cli(fd);
	targ->peer_addr = sock_get_remote_address(fd);
	for (;;) {
		//DTRACE_PROBE1(udepot, trt_epoll_serve_entry, &trt::T::self());
		int err = serve_kv_request(cli, targ->srv_kv, targ->mb1, targ->mb2);
		//DTRACE_PROBE1(udepot, trt_epoll_serve_exit, &trt::T::self());
		if (err) {
			if (ECONNRESET != err) {
				UDEPOT_ERR("client %s:%s err=%d (%s)",
					std::get<0>(targ->peer_addr).c_str(),
					std::get<1>(targ->peer_addr).c_str(),
					err, strerror(err));
			}
			break;
		}
		trt::T::yield();
	}
	UDEPOT_DBG("done serving");

	// return the TaskArg in the pool
	TaskArgPool__->put_arg(targ);

	// we are done, close the fd via the epoll wrapper so that it will also
	// de-register it.
	trt::Epoll::close(fd);
	return nullptr;
}

static void *
task_accept(void *arg)
{
	int fd = (int)(uintptr_t)(arg);
	// accept new connections. For each, spawn a new task to serve the client
	while (true) {
		struct sockaddr cli_addr;
		socklen_t cli_addr_len;

		UDEPOT_DBG("accept\n");
		int afd = trt::Epoll::accept_ll(fd, &cli_addr, &cli_addr_len);
		UDEPOT_DBG("accept returned: %d\n", afd);
		if (afd == -1) {
			perror("accept");
		}

		// XXX: FIXME (abort)
		int optval = 1;
		int rc = setsockopt(afd, SOL_TCP, TCP_NODELAY, &optval, sizeof(optval));
		if (-1 == rc) {
			UDEPOT_ERR("setsockopt %s err=%d.\n", strerror(errno), errno);
			abort();
		}

		trt::TaskFn fn = task_serve;
		trt::TaskFnArg targ = (void *)(uintptr_t)afd;
		// NB: Initially I used EPOLLIN | EPOLLOUT but this results in many
		// spurious wake ups.
		trt::Epoll::register_and_spawn(afd, EPOLLIN, fn, targ,
		                               trt::Epoll::SpawnPolicy::Distribute);
	}

	return nullptr;
}

// TrtEpollServer::Conf
const std::string TrtEpollServer::Conf::DEFAULT_PORT = "43345";
const std::string TrtEpollServer::Conf::DEFAULT_PROTOCOL = "tcp";

int
TrtEpollServer::Conf::from_string(std::string s) {
	listen_conf_m.service_m  = DEFAULT_PORT;
	listen_conf_m.protocol_m = DEFAULT_PROTOCOL;
	int ret = listen_conf_m.parse(s);
	return ret;
}

std::string
TrtEpollServer::Conf::parse_format_help() {
	std::stringstream sstream;
	sstream << SocketPeerConf::parse_format_help()
	        << "default port:" << DEFAULT_PORT     << std::endl
	        << "default prot:" << DEFAULT_PROTOCOL << std::endl;
	return sstream.str();
}

TrtEpollServer::TrtEpollServer(TrtEpollNet &net, KV_MbuffInterface &srv_kv, Conf conf)
 : srv_conf_m(conf), srv_kv_m(srv_kv), srv_listen_fd_m(-1) {
	assert(kv__ == nullptr);
	kv__ = &srv_kv_m;
}

int
TrtEpollServer::start() {
	assert(srv_listen_fd_m == -1);
	if (srv_conf_m.listen_conf_m.is_invalid()) {
		return 0;
	}

	UDEPOT_MSG("Listening on: %s", srv_conf_m.listen_conf_m.str().c_str());
	struct addrinfo *rsai = srv_conf_m.listen_conf_m.getaddrinfo(true);
	srv_listen_fd_m = socket(rsai->ai_family, rsai->ai_socktype, rsai->ai_protocol);
	if (-1 == srv_listen_fd_m) {
		perror("socket");
		abort();
	}

	int optval = 1;
	int rc = setsockopt(srv_listen_fd_m, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	if (-1 == rc) {
		perror("setsockopt");
		abort();
	}

	//optval=1;
	//rc = setsockopt(srv_listen_fd_m, SOL_SOCKET, SO_ZEROCOPY, &optval, sizeof(optval));
	//if (-1 == rc) {
	//	UDEPOT_ERR("setsockopt %s err=%d.\n", strerror(errno), errno);
	//	abort();
	//}

	// try to bind it
	rc = bind(srv_listen_fd_m, rsai->ai_addr, rsai->ai_addrlen);
	freeaddrinfo(rsai); // no longer needed
	if (-1 == srv_listen_fd_m) {
		perror("bind");
		abort();
	}

	rc = trt::Epoll::listen(srv_listen_fd_m, 50);
	if (-1 == rc) {
		perror("listen");
		abort();
	}

	trt::T::spawn(task_accept, (void *)(uintptr_t)srv_listen_fd_m, nullptr, true, trt::TaskType::TASK);

	return 0;
}

int
TrtEpollServer::stop() {
	return 0;
}

//-----------------------------------------------------------------------------
// Client
//

TrtEpollClient::Conf::Conf() {
	fprintf(stderr, "%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
	abort();
}

int
TrtEpollClient::Conf::from_string(std::string s) {
	fprintf(stderr, "%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
	abort();
}

std::string
TrtEpollClient::Conf::parse_format_help() {
	fprintf(stderr, "%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
	abort();
}

TrtEpollClient::TrtEpollClient(TrtEpollNet &net, MbuffAllocIface &mb_alloc, Conf conf)
{
	fprintf(stderr, "%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
	abort();
}

int
TrtEpollClient::start() {
	fprintf(stderr, "%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
	abort();
}

int
TrtEpollClient::stop() {
	fprintf(stderr, "%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
	abort();
}

} // end namespace udepot
