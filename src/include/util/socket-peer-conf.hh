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

#ifndef SOCKET_PEER_CONF_HH_
#define SOCKET_PEER_CONF_HH_

#include <tuple>
#include <string>
#include <sstream>

struct addrinfo;

// Configuration for a socket peer (endpoint)
struct SocketPeerConf {
	std::string protocol_m; // "tcp" or "udp"
	std::string node_m;
	std::string service_m;
        size_t      net_cap_bytes_m; // network cap in bytes, 0 if unlimited
	// call getaddrinfo(3)  with the proper arguments and return the result.
	// The caller is responsible to call freeaddrinfo(3)
	struct addrinfo *getaddrinfo(bool server);

	// parse string and place result in fields
	int parse(std::string s);
	// return message about parse format
	static std::string parse_format_help();

	// true at construction time, and if parse() failed.
	bool is_invalid(void) const;
	bool is_valid(void)   const { return !is_invalid(); }

	bool operator==(const SocketPeerConf& rhs) const {
		return (protocol_m == rhs.protocol_m)
		    && (node_m == rhs.node_m)
		    && (service_m == rhs.service_m);
	}

	// return a string representation of this configuration
	std::string str(void) {
		std::stringstream sstream;
		sstream << protocol_m << "://" << (node_m.size() == 0 ? std::string("*") : node_m) << ":" << service_m;
		return sstream.str();
	}
        SocketPeerConf(): net_cap_bytes_m(0) {}
};

// implement std::less so that we can use it transparently with std::set
namespace std {
	template<> struct less<SocketPeerConf> {
		bool operator() (const SocketPeerConf& lhs, const SocketPeerConf& rhs) const {
			using t3s = std::tuple<std::string, std::string, std::string>;
			t3s lhs_ = std::make_tuple(lhs.protocol_m, lhs.node_m, lhs.node_m);
			t3s rhs_ = std::make_tuple(lhs.protocol_m, lhs.node_m, lhs.node_m);
			return lhs_ < rhs_;
		}
	};
}

#endif // SOCKET_PEER_CONF_HH_
