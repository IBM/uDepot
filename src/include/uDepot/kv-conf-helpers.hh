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
#ifndef _UDEPOT_KV_CONF_HELPERS_H_
#define _UDEPOT_KV_CONF_HELPERS_H_

#include <string>
#include <vector>
#include <cstdint>

#include "uDepot/net.hh"
#include "util/debug.h"

namespace udepot {

template<typename Net>
static inline
typename Net::Server::Conf
get_udepot_server_conf(const std::string &self_server) {
	int err;
	typename Net::Server::Conf conf;

	if (self_server.size() == 0)
		return conf;

	err = conf.from_string(self_server);
	if (err) {
		UDEPOT_ERR("Could not parse client configuration `%s`", self_server.c_str());
		UDEPOT_ERR("Help:%s", conf.parse_format_help().c_str());
		exit(1);
	}

	return conf;
}

template<typename Net>
static inline
std::vector<typename Net::Client::Conf>
get_udepot_clients_conf(const std::vector<std::string> &remote_servers) {
	std::vector<typename Net::Client::Conf> ret;
	for (auto rs: remote_servers) {
		typename Net::Client::Conf c;
		int err = c.from_string(rs);
		if (err) {
			UDEPOT_ERR("Could not parse client configuration `%s`\n", rs.c_str());
			UDEPOT_ERR("Help:%s", c.parse_format_help().c_str());
			exit(1);
		}
		ret.push_back(c);
	}
	return ret;
}

}; // namespace udepot

#endif	// _UDEPOT_KV_CONF_HELPERS_H_
