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

#include "uDepot/net/trt-memcache.hh"

#include <sstream>
#include <typeindex>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <thread>

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_epoll.hh"
#include "trt_util/arg_pool.hh"
extern "C" {
#include "trt_util/net_helpers.h"
}

#include "util/debug.h"
#include "kv-mbuff.hh"
#include "uDepot/mbuff-cache.hh"
#include "uDepot/kv-wire.hh"
#include "util/socket-peer-conf.hh"
#include "util/sys-helpers.hh"
#include "uDepot/net/mc-helpers.hh"

namespace udepot {

struct McTaskArg {
	KV_MbuffInterface &srv_kv;
	Mbuff mb1, mb2;
	MbuffCacheBase &srv_mb_cache;
        MemcacheTrtServer &srv;
	McTaskArg(KV_MbuffInterface &kv, MbuffCacheBase &mbc, MemcacheTrtServer &srv)
		: srv_kv(kv)
		, mb1(kv.mbuff_type_index())
		, mb2(kv.mbuff_type_index())
		, srv_mb_cache(mbc)
		, srv(srv) {}
};
using McTaskArgPool = ArgPool<McTaskArg>;

// TODO: de-duplicate this
class ConnectionTrtEpoll : public ConnectionBase {
public:
	ConnectionTrtEpoll() : fd_(-1) {}
	ConnectionTrtEpoll(int fd): fd_(fd) {}
	ConnectionTrtEpoll(ConnectionTrtEpoll const&) = delete;
	void operator=(ConnectionTrtEpoll const&) = delete;

	virtual ssize_t send(const void *buff, size_t len, int flags) override final {
		const ssize_t n = trt::Epoll::send(fd_, buff, len, flags);
		#if !defined(NDEBUG)
                if (0 < n)
                        send_bytes_.fetch_add(static_cast<size_t>(n), std::memory_order_relaxed);
		#endif
                return n;
	}
	virtual ssize_t recv(void *buff, size_t len, int flags) override final {
		const ssize_t n = trt::Epoll::recv(fd_, buff, len, flags);
		#if !defined(NDEBUG)
                if (0 < n)
                        recv_bytes_.fetch_add(static_cast<size_t>(n), std::memory_order_relaxed);
		#endif
                return n;
	}
	virtual ssize_t sendmsg(const struct msghdr *msg, int flags) override final {
		const ssize_t n = trt::Epoll::sendmsg(fd_, msg, flags);
		#if !defined(NDEBUG)
                if (0 < n)
                        send_bytes_.fetch_add(static_cast<size_t>(n), std::memory_order_relaxed);
		#endif
                return n;
	}
	virtual ssize_t recvmsg(struct msghdr *msg, int flags) override final {
		const ssize_t n = trt::Epoll::recvmsg(fd_, msg, flags);
		#if !defined(NDEBUG)
                if (0 < n)
                        recv_bytes_.fetch_add(static_cast<size_t>(n), std::memory_order_relaxed);
		#endif
                return n;
	}

private:
	int fd_;
};

// ensure that tread_init() is not called twice, etc.
static thread_local bool t_initialized_g = false;
// server interface
KV_MbuffInterface *kv_g(nullptr);
MbuffCache<TrtLock> *mb_cache_g(nullptr);
MemcacheTrtServer *srv_g(nullptr);
// pool for task arguments
static thread_local std::unique_ptr<ArgPool<McTaskArg>> McTaskArgPool_g(nullptr);
constexpr size_t TASK_ARG_POOL_SIZE = 1024;

//-----------------------------------------------------------------------------
// Net
//


MemcacheTrtNet::MemcacheTrtNet(Conf cnf) {}

int MemcacheTrtNet::global_init(void)
{
	return 0;
}

int MemcacheTrtNet::thread_init(void)
{
	// initialize epoll thread-local state and spawn poller task
	assert(!t_initialized_g);
	assert(trt::T::in_trt());
	trt::Epoll::init();
	trt::Epoll::set_wait_timeout(100 /* ms */);
	trt::T::spawn(trt::Epoll::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);

	// create thread-local task argument pool
	assert(McTaskArgPool_g == nullptr);
	assert(kv_g != nullptr);
	void *targs_raw = operator new[](sizeof(McTaskArg)*TASK_ARG_POOL_SIZE);
	McTaskArg *targs = static_cast<McTaskArg *>(targs_raw);
	for (size_t i = 0; i < TASK_ARG_POOL_SIZE; i++) {
		new (targs + i) McTaskArg(*kv_g, *mb_cache_g, *srv_g);
	}
	auto release_fn = [] (McTaskArg *args) {
		for (size_t i = 0; i < TASK_ARG_POOL_SIZE; i++) {
			McTaskArg *a = args + i;
			a->~McTaskArg();
		}
		std::free(args);
	};
	McTaskArgPool *targs_pool = new McTaskArgPool(TASK_ARG_POOL_SIZE, targs, release_fn);
	McTaskArgPool_g = std::unique_ptr<McTaskArgPool>(targs_pool);

	t_initialized_g = true;
	return 0;
}

int MemcacheTrtNet::thread_exit(void)
{
	assert(t_initialized_g);
	trt::Epoll::stop();
	McTaskArgPool_g = nullptr;
	t_initialized_g = false;
	return 0;
}

int MemcacheTrtNet::global_exit(void)
{
	return 0;
}


//-----------------------------------------------------------------------------
// Server
//


static void *
task_serve_mc_request(void *arg)
{
	int cfd = (int)(uintptr_t)arg;
	McTaskArg *targ;

	for (;;) {
		// try to get a McTaskArg from the pool. If we can't, yield.
		targ = McTaskArgPool_g->get_arg();
		if (targ)
			break;
		// if this happens too often, something's wrong. have a message for now
		UDEPOT_MSG("Cannot get McTaskArg. Yielding");
		trt::T::yield();
	}

	ConnectionTrtEpoll scon(cfd);
	memcache::msg_buff buff;
	Mbuff &mbuff = targ->mb1;
	Mbuff &keymbuff = targ->mb2;
        const size_t net_cap = targ->srv.get_net_cap();
	for (; -1 != cfd; ) {
                if (net_cap) {
                        const size_t net_bytes = scon.recv_bytes_ + scon.send_bytes_;
                        if (net_cap < net_bytes) {
                                UDEPOT_MSG("Network cap reached. Usage=%lu Cap=%lu. Aborting.",
                                        net_bytes, net_cap);
                                break;
                        }
                }
		buff.reset();
		mbuff.reslice(0);

		memcache::cmd cmd = memcache::parser::read_cmd(scon, buff);
		if (0 == cmd.err_) {
			cmd.handle(scon, targ->srv_kv, targ->srv_mb_cache, mbuff, keymbuff);
			if (0 == cmd.err_)
				continue;
		}

		// only if error either at read_cmd, or handle
		if (ENOMEM == cmd.err_) {
			trt::T::yield();
			continue;
		}
		if (ECONNRESET == cmd.err_)
			UDEPOT_DBG("recv on cfd=%d returned 0, deleting cfd.", cfd);
		else
			UDEPOT_ERR("unknown error %s %d.", strerror(cmd.err_), cmd.err_);

		break;
	}
	// we are done, close the fd via the epoll wrapper so that it will also
	// de-register it.
	trt::Epoll::close(cfd);
	cfd = - 1;
	targ->srv_kv.mbuff_free_buffers(mbuff);
	targ->srv_kv.mbuff_free_buffers(keymbuff);

	// return the McTaskArg in the pool
	McTaskArgPool_g->put_arg(targ);

	return nullptr;
}

static void *
task_mc_accept(void *arg)
{
	const int fd = (int) (uintptr_t) arg;
	// accept new connections. For each, spawn a new task to serve the client
	while (true) {
		struct sockaddr cli_addr;
		socklen_t cli_addr_len;
		UDEPOT_DBG("accept");
		int afd = trt::Epoll::accept_ll(fd, &cli_addr, &cli_addr_len);
		UDEPOT_DBG("accept returned: %d", afd);
		if (afd == -1) {
			// if we are shutting down, we will get a -1 here eith an errno of
			// ESHUTDOWN, which means that we should just return. There might be
			// other cases (different errors) where we want to re-try. For now,
			// we allways return.
			UDEPOT_MSG("accept_ll: returned %d (%s). Exiting loop\n", errno, strerror(errno));
			return nullptr;
		}

		const int optval = 1;
		int rc = setsockopt(afd, SOL_TCP, TCP_NODELAY, &optval, sizeof(optval));
		if (-1 == rc) {
			UDEPOT_ERR("setsockopt %s err=%d.\n", strerror(errno), errno);
			abort();
		}

		trt::TaskFn fn = task_serve_mc_request;
		trt::TaskFnArg targ = (void *)(uintptr_t)afd;
		trt::Epoll::register_and_spawn(afd, EPOLLIN | EPOLLOUT, fn, targ,
					trt::Epoll::SpawnPolicy::Distribute);
	}

	return nullptr;
}

// MemcacheTrtServer::Conf
const std::string MemcacheTrtServer::Conf::DEFAULT_PORT = "11211"; // default memcache port is 11211
const std::string MemcacheTrtServer::Conf::DEFAULT_PROTOCOL = "tcp";

MemcacheTrtServer::Conf::Conf() : listen_conf_m() { }

int
MemcacheTrtServer::Conf::from_string(std::string s)
{
        std::size_t find = s.find(',');
        std::string serv_conf_s;
        if (find == std::string::npos) {
                serv_conf_s = s;
                UDEPOT_MSG("par1 %s. net cap = %lu", serv_conf_s.c_str(),
                        listen_conf_m.net_cap_bytes_m);
        } else {
                serv_conf_s = s.substr(0, find);
                listen_conf_m.net_cap_bytes_m = std::stoul(s.substr(find + 1));
                UDEPOT_MSG("par1 %s par2 %s. net cap = %lu", serv_conf_s.c_str(),
                        s.substr(find + 1).c_str(), listen_conf_m.net_cap_bytes_m);
        }
	listen_conf_m.service_m  = DEFAULT_PORT;
	listen_conf_m.protocol_m = DEFAULT_PROTOCOL;
	int ret = listen_conf_m.parse(serv_conf_s);
	return ret;
}

std::string
MemcacheTrtServer::Conf::parse_format_help()
{
	std::stringstream sstream;
	sstream << SocketPeerConf::parse_format_help()
	        << "default port:" << DEFAULT_PORT     << std::endl
	        << "default prot:" << DEFAULT_PROTOCOL << std::endl
	        << "Followed by an optional Network cap in bytes, 0 for unlimited, after a comma" << std::endl;
	return sstream.str();
}

MemcacheTrtServer::MemcacheTrtServer(MemcacheTrtNet &net, KV_MbuffInterface &srv_kv, Conf conf)
	: srv_conf_m(conf), srv_kv_m(srv_kv), srv_thread_exit_m(false), srv_listen_fd_m(-1)
{
	assert(kv_g == nullptr);
	kv_g = &srv_kv_m;
	assert(mb_cache_g == nullptr);
	mb_cache_g = new MbuffCache<TrtLock> (*kv_g);
	assert(srv_g == nullptr);
        srv_g = this;
}

MemcacheTrtServer::~MemcacheTrtServer()
{
        delete mb_cache_g;
        mb_cache_g = nullptr;
}

void
MemcacheTrtServer::mc_stats(void)
{
	constexpr u32 MC_STATS_SAMPLE_INTERVAL_SEC	(6*3600);
	for (u32 i = 0; !srv_thread_exit_m; ++i) {
		if (0 == (i % MC_STATS_SAMPLE_INTERVAL_SEC))
			UDEPOT_MSG("udepot-used-bytes=%lu", srv_kv_m.get_kvmbuff_size());

		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
}

int
MemcacheTrtServer::start()
{
	int rc = memcache::parser::start();
	if (0 != rc)
		return rc;

	assert(srv_listen_fd_m == -1);
	if (!srv_conf_m.is_valid()) {
		UDEPOT_ERR("Invalid server configuration. Not starting server.");
		return 0;
	}

	struct addrinfo *rsai = srv_conf_m.listen_conf_m.getaddrinfo(true);
	srv_listen_fd_m = socket(rsai->ai_family, rsai->ai_socktype, rsai->ai_protocol);
	if (-1 == srv_listen_fd_m) {
		UDEPOT_ERR("socket failed w %s %d.", strerror(errno), errno);
		abort();
	}

	const int optval = 1;
	rc = setsockopt(srv_listen_fd_m, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	if (-1 == rc) {
		UDEPOT_ERR("setsockopt %s err=%d.\n", strerror(errno), errno);
		abort();
	}

	// try to bind it
	rc = bind(srv_listen_fd_m, rsai->ai_addr, rsai->ai_addrlen);
	freeaddrinfo(rsai); // no longer needed
	if (-1 == srv_listen_fd_m) {
		UDEPOT_ERR("bind %s err=%d.\n", strerror(errno), errno);
		abort();
	}

	rc = trt::Epoll::listen(srv_listen_fd_m, 50);
	if (-1 == rc) {
		UDEPOT_ERR("listen %s err=%d.\n", strerror(errno), errno);
		abort();
	}

	trt::T::spawn(task_mc_accept, (void *)(uintptr_t) srv_listen_fd_m, nullptr, true, trt::TaskType::TASK);
	srv_stats_thr_m = std::thread(&MemcacheTrtServer::mc_stats, this);

	return 0;
}

int
MemcacheTrtServer::stop()
{
	UDEPOT_MSG("server received shutdown request");
	memcache::parser::stop();
	srv_thread_exit_m = true;
	if (srv_stats_thr_m.joinable())
		srv_stats_thr_m.join();
	return 0;
}

// client
MemcacheTrtClient::Conf::Conf(SocketPeerConf server_conf) : connect_conf_m(server_conf) {}
MemcacheTrtClient::Conf::Conf() : connect_conf_m() {}

int
MemcacheTrtClient::Conf::from_string(std::string s)
{
	connect_conf_m.service_m  = MemcacheTrtServer::Conf::DEFAULT_PORT;
	connect_conf_m.protocol_m = MemcacheTrtServer::Conf::DEFAULT_PROTOCOL;
	int ret = connect_conf_m.parse(s);
	return ret;
}

std::string
MemcacheTrtClient::Conf::parse_format_help()
{
	std::stringstream sstream;
	sstream << SocketPeerConf::parse_format_help()
	        << "default port:" << MemcacheTrtServer::Conf::DEFAULT_PORT << std::endl
	        << "default prot:" << MemcacheTrtServer::Conf::DEFAULT_PROTOCOL << std::endl;
	return sstream.str();
}

MemcacheTrtClient::MemcacheTrtClient(MemcacheTrtNet &unused, MbuffAllocIface &unused2, Conf cnf)
	: cli_conf_m(cnf),
	  cli_fd_m(-1)
{}

int
MemcacheTrtClient::start(void)
{
	return connect();
}

int
MemcacheTrtClient::stop(void)
{
	if (is_connected())
		disconnect();
	return 0;
}

int
MemcacheTrtClient::connect(void)
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
MemcacheTrtClient::disconnect(void)
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
