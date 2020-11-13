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

// ubench for the uDepot network (using the NOP operation)

#include <string.h>
#include <unistd.h>
#include <iostream>

#include "uDepot/mbuff-alloc.hh"
#include "trt_util/io_ptr.hh"
#include "uDepot/net/socket.hh"
#include "util/profiling.hh"

using MbuffAllocMalloc = udepot::MbuffAllocIO<IoBuffMalloc>;

struct BenchConf {
	std::string cli_conf;
	size_t nops;

	BenchConf() : cli_conf(), nops(1000*100) {}

	void print(void) {
		std::cout << "CONF: cli_conf:" << cli_conf << " nops:" << nops << std::endl;
	}
};

template<typename Net>
static void bench(BenchConf &bconf) {
	int err;
	MbuffAllocMalloc mb_alloc;
	typename Net::Conf net_conf;
	typename Net::SocketNet net(net_conf);

	net.global_init();
	net.thread_init();

	typename Net::Client::Conf cli_conf;
	err = cli_conf.from_string(bconf.cli_conf);
	if (err) {
		fprintf(stderr, "Configuration error: conf:%s err:%d (%s)\n", bconf.cli_conf.c_str(), errno, strerror(errno));
		net.thread_exit();
		net.global_exit();
		return;
	}

	typename Net::Client cli(net, mb_alloc, cli_conf);
	err = cli.connect();
	if (err) {
		fprintf(stderr, "Client failed to connect: %d (%s)\n", errno, strerror(errno));
		net.thread_exit();
		net.global_exit();
		return;
	}

	for (size_t i=0; i < bconf.nops; i++) {
		//PROBE_TICKS_START(net_ubench_nop);
		err = cli.nop();
		if (err) {
			fprintf(stderr, "Error sending NOOP to server: %s\n", strerror(err));
			break;
		}
		//PROBE_TICKS_END(net_ubench_nop);
	}

	net.thread_exit();
	net.global_exit();
}

using Net = udepot::SocketNet;

static void
help(FILE *f, const char *progname) {
	fprintf(f, "Usage: %s -c <cli_conf> [-n nops]\n", progname);
	fprintf(stderr, "cli_conf: %s\n", Net::Client::Conf::parse_format_help().c_str());
}

int main(int argc, char *argv[])
{
	int o;
	BenchConf bconf;

	while ((o = getopt(argc, argv, "c:n:h")) != -1) {
		switch (o) {
			case 'c':
			bconf.cli_conf = std::string(optarg);
			break;

			case 'n':
			bconf.nops = atol(optarg);
			break;

			case 'h':
			help(stdout, argv[0]);
			exit(0);

			default:
			help(stderr, argv[0]);
			exit(1);
		}
	}

	if (bconf.cli_conf.size() == 0) {
		fprintf(stderr, "client configuration (-c) not specified\n");
		help(stderr, argv[0]);
		exit(1);
	}

	bconf.print();
	bench<Net>(bconf);
	return 0;
}
