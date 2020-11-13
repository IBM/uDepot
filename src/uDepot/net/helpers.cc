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
#include <tuple>
#include <sys/socket.h>
#include <netdb.h>

#include "util/debug.h"

#include "uDepot/net/helpers.hh"

namespace udepot {

std::tuple<std::string, std::string>
sock_get_local_address(int sockfd)
{
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	const size_t b_size = 128;
	int err;

	// error (empty) return value
	auto ret = std::make_tuple(std::string(), std::string());

	// allocate in the heap to be trt-friendly
	char *b1 = (char *)std::malloc(b_size);
	char *b2 = (char *)std::malloc(b_size);
	if (!b1 || !b2) {
		UDEPOT_ERR("malloc failed");
		goto end;
	}

	err = getsockname(sockfd, (struct sockaddr *)&addr, &addrlen);
	if (err == -1)
		goto end;

	err = getnameinfo((struct sockaddr *)&addr, addrlen, b1, b_size, b2, b_size, NI_NUMERICHOST | NI_NUMERICSERV);
	if (err)
		goto end;

	ret = std::make_tuple(std::string(b1), std::string(b2));

end:
	if (b1)
		std::free(b1);
	if (b2)
		std::free(b2);

	return ret;
}


std::tuple<std::string, std::string>
sock_get_remote_address(int sockfd)
{
	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	const size_t b_size = 128;
	int err;

	// error (empty) return value
	auto ret = std::make_tuple(std::string(), std::string());

	// allocate in the heap to be trt-friendly
	char *b1 = (char *)std::malloc(b_size);
	char *b2 = (char *)std::malloc(b_size);
	if (!b1 || !b2) {
		UDEPOT_ERR("malloc failed");
		goto end;
	}

	err = getpeername(sockfd, (struct sockaddr *)&addr, &addrlen);
	if (err == -1)
		goto end;

	err = getnameinfo((struct sockaddr *)&addr, addrlen, b1, b_size, b2, b_size, NI_NUMERICHOST | NI_NUMERICSERV);
	if (err)
		goto end;

	ret = std::make_tuple(std::string(b1), std::string(b2));

end:
	if (b1)
		std::free(b1);
	if (b2)
		std::free(b2);

	return ret;
}

} // end namespace udepot
