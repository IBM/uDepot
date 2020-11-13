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

#ifndef _UDEPOT_NET_HELPERS_HH_
#define _UDEPOT_NET_HELPERS_HH_

#include <string>
#include <tuple>
#include <sys/socket.h>

namespace udepot {

// get local address from socket (ip, port) or empty stings if error
std::tuple<std::string, std::string>sock_get_local_address(int sockfd);

// get remote address from socket (ip, port) or empty stings if error
std::tuple<std::string, std::string> sock_get_remote_address(int sockfd);

} // end namespace udepot

#endif
