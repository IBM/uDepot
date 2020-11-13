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


#include <string>
#include <cstring> // memset
#include <tuple>
#include <sstream>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "util/socket-peer-conf.hh"
#include "util/debug.h"

extern "C" {
	#include "trt_util/net_helpers.h"
}

std::string
SocketPeerConf::parse_format_help()
{
	std::stringstream sstream;
	// TODO: test the strings below.
	sstream << "URL is prot://node:port, where prot and port are optional" << std::endl
	        << "Examples of valid URLs:" << std::endl
	        <<  "   {udp,tcp}://147.102.3.1:1234" << std::endl
	        <<  "   {udp,tcp}://*:1234" << std::endl
	        <<  "   147.102.3.1:1234" << std::endl
	        <<  "   *:1234" << std::endl
	        <<  "   147.102.3.1" << std::endl;

	return sstream.str();
}

int
SocketPeerConf::parse(std::string s)
{
	int err;
	struct url url = {0};
	err = url_parse(&url, s.c_str());
	if (!err) {
		if (url.prot)
			protocol_m = std::string(url.prot);
		if (url.node)
			node_m = std::string(url.node);
		if (url.serv)
			service_m = std::string(url.serv);
	}

	url_free_fields(&url);
	return err;
}

struct addrinfo *
SocketPeerConf::getaddrinfo(bool server)
{
	struct addrinfo hint, *ret;

	std::memset(&hint, 0, sizeof(struct ::addrinfo));

	hint.ai_family   = AF_INET;    /* IPv4 */
	hint.ai_socktype = SOCK_STREAM; /* TCP socket */
	hint.ai_flags    = 0;
	hint.ai_protocol = 0;          /* Any protocol */
	hint.ai_canonname = NULL;
	hint.ai_addr = NULL;
	hint.ai_next = NULL;

	const char *node;
	if (server) {
		// If the AI_PASSIVE flag is specified in hints.ai_flags, and
		// node is NULL, then the returned socket addresses will be
		// suitable for bind(2)ing a socket that will accept(2)
		// connections.  The returned socket address will contain the
		// "wildcard address" (INADDR_ANY for IPv4 addresses,
		// IN6ADDR_ANY_INIT for IPv6 address).  The wildcard address is
		// used by applications (typically servers) that intend to
		// accept connections on any of the hosts’s network addresses.
		// If node is not NULL, then the AI_PASSIVE flag is ignored.
		node = nullptr;
		hint.ai_flags = AI_PASSIVE;
	} else {
		// If the AI_PASSIVE flag is not set in hints.ai_flags, then the
		// returned socket addresses will be suitable for use with
		// connect(2), sendto(2), or sendmsg(2).  If node is NULL, then
		// the network address will be set to the loopback interface
		node = node_m == std::string("*") ? nullptr : node_m.c_str();
	}
	const char *service = service_m.size() == 0 ? nullptr : service_m.c_str();

	int rc = ::getaddrinfo(node, service, &hint, &ret);
	if (0 != rc || ret == NULL) {
		UDEPOT_ERR("%s: getaddrinfo: %s", __PRETTY_FUNCTION__, gai_strerror(rc));
		return nullptr;
	}
#if	0
	if (ret->ai_next) {
		UDEPOT_ERR("Dealing with multiple addresses not yet supported.");
		abort();
	}
#endif
	return ret;
}


bool SocketPeerConf::is_invalid() const {
	return protocol_m.size() == 0 && node_m.size() == 0 && service_m.size() == 0;
}
