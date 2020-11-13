/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "trt_util/ticks.hh"
#include "trt_util/sdt.h"
extern "C" {
	#include "trt_util/net_helpers.h"
}

struct conf {
	size_t op_size; // size of packet to send
	size_t nops;    // total operations
};

static void
print_conf(struct conf *cnf) {
	printf("CONF: op_size:%zd nops:%zd\n", cnf->op_size, cnf->nops);
}

static void
run_echo_cli(struct conf *cnf, int fd) {

	char b_send[cnf->op_size] = {0};
	char b_recv[cnf->op_size];

	for (size_t i=0; i < cnf->nops; i++) {
		ssize_t ret;

		uint64_t t = ticks::get_ticks();

		ret = send(fd, b_send, sizeof(b_send), 0);
		if (ret != (ssize_t)sizeof(b_send)) {
			fprintf(stderr, "send returned:%zd expected:%zd\n", ret, sizeof(b_send));
			exit(1);
		}

		ret = recv(fd, b_recv, sizeof(b_recv), 0);
		if (ret != (ssize_t)sizeof(b_recv)) {
			fprintf(stderr, "recv returned:%zd expected:%zd\n", ret, sizeof(b_recv));
			exit(1);
		}

		t = ticks::get_ticks() - t;
		DTRACE_PROBE1(trt, net_echo_client_op_ticks, t);
	}
}

int main(int argc, char *argv[]) {
	struct url url;
	int err, sockfd;
	struct conf cnf;
	struct addrinfo *ai;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <url>\n", argv[0]);
		fprintf(stderr, "URL examples: 147.102.3.92:1000\n");
		exit(0);
	}

	err = url_parse(&url, argv[1]);
	if (err) {
		url_free_fields(&url);
		fprintf(stderr, "Cannot parse URL: %s\n", argv[1]);
		exit(1);
	}

	ai = url_getaddrinfo(&url, false);
	if (!ai) {
		url_free_fields(&url);
		fprintf(stderr, "url_getaddrinfo failed. URL: %s\n", argv[1]);
		exit(1);
	}

	sockfd = ai_connect(ai, NULL);
	if (sockfd == -1) {
		url_free_fields(&url);
		perror("ai_connect()");
		exit(1);
	}

	cnf.op_size = 22;
	cnf.nops = 1000*1000;
	print_conf(&cnf);
	run_echo_cli(&cnf, sockfd);

	//url_free_fields(&url);
	freeaddrinfo(ai);
	return 0;
}
