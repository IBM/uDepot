/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// Spawn an echo server using trt+epoll

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h> // TCP_NODELAY
#include <string.h> // strerror

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_epoll.hh"


extern "C" {
	#include "trt_util/net_helpers.h"
}

using namespace trt;

static void *
echo_task(void *arg)
{
	char buff[1024];
	ssize_t r_ret, s_ret;
	int fd = (int)(uintptr_t)arg;

	const int optval = 1;
	if (setsockopt(fd, SOL_TCP, TCP_NODELAY, &optval, sizeof(optval)) == -1) {
		fprintf(stderr, "setsockopt %s err=%d.\n", strerror(errno), errno);
		return nullptr;
	}

	while (true) {

		r_ret = Epoll::recv(fd, buff, sizeof(buff), 0);
		if (r_ret < 0) {
			perror("recv");
			break;
		} else if (r_ret == 0) {
			break;
		}

		s_ret = Epoll::send(fd, buff, r_ret, 0);
		if (s_ret < 0) {
			perror("send");
			continue;
		} else if (s_ret != r_ret) {
			fprintf(stderr, "Partial write: FIXME!\n");
			abort();
		}
	}

	Epoll::close(fd);

	return nullptr;
}

static void *
echo_srv(void *arg)
{
	int err, fd;
	struct url url;
	const char *url_str = (const char *)arg;
	struct addrinfo *ai_list, *ai_b;

	// initialize epoll and start poller
	Epoll::init();
	T::spawn(Epoll::poller_task, nullptr, nullptr, true, TaskType::TASK);

	// bind and listen to URL given by the user
	err = url_parse(&url, url_str);
	if (err) {
		fprintf(stderr, "url_parse failed\n");
		exit(1);
	}
	ai_list = url_getaddrinfo(&url, true);
	fd = ai_bind(ai_list, &ai_b);
	printf("listen to: %s\n", url_str);
	err = Epoll::listen(fd, 128);
	if (err == -1) {
		perror("listen failed");
		exit(1);
	}

	struct sockaddr cli_addr;
	socklen_t cli_addr_len;
	while (true) {
		printf("accept\n");
		int accept_fd = Epoll::accept(fd, &cli_addr, &cli_addr_len);
		printf("accept returned: %d\n", accept_fd);
		if (accept_fd == -1) {
			perror("accept");
		}

		T::spawn(echo_task, (void *)(uintptr_t)accept_fd, nullptr, true, TaskType::TASK);
	}

	url_free_fields(&url);
	freeaddrinfo(ai_b);
	return nullptr;
}

int main(int argc, char *argv[])
{
	Controller c;
	const char server_url[] = "tcp://*:8000";
	c.spawn_scheduler(echo_srv, (void *)server_url, TaskType::TASK);

	while (true) {
		sleep(10);
	}

	return 0;
}
