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

#include <memory>
#include <array>
#include <cinttypes>
#include <mutex>
#include "uDepot/stats.hh"
#include "util/debug.h"

namespace udepot {

static std::mutex                                 lock__;
static std::vector<std::shared_ptr<uDepotStats>>  udepot_stats_all__;
static thread_local std::shared_ptr<uDepotStats>  udepot_stats__;

uDepotStats::uDepotStats() :
	put_latency(100, std::vector<uint64_t>({50000,100000,500000,1000000})),
	get_latency(100, std::vector<uint64_t>({50000,100000,500000,1000000})),
	del_latency(100, std::vector<uint64_t>({50000,100000,500000,1000000}))
{ }

void uDepotStats::tls_init() {
	udepot_stats__ = std::unique_ptr<uDepotStats>(new uDepotStats());
	lock__.lock();
	udepot_stats_all__.push_back(udepot_stats__);
	lock__.unlock();
}

void uDepotStats::tls_init_if_needed() {
	if (!udepot_stats__)
		tls_init();
}

uDepotStats::put_ctx uDepotStats::put_start(void) {
	tls_init_if_needed();
	return udepot_stats__->put_latency.start();
}

void uDepotStats::put_stop(uDepotStats::put_ctx ctx) {
	udepot_stats__->put_latency.stop(ctx);
}


uDepotStats::get_ctx uDepotStats::get_start(void) {
	tls_init_if_needed();
	return udepot_stats__->get_latency.start();
}

void uDepotStats::get_stop(uDepotStats::get_ctx ctx) {
	udepot_stats__->get_latency.stop(ctx);
}

uDepotStats::del_ctx uDepotStats::del_start(void) {
	tls_init_if_needed();
	return udepot_stats__->del_latency.start();
}

void uDepotStats::del_stop(uDepotStats::del_ctx ctx) {
	udepot_stats__->del_latency.stop(ctx);
}

static void
print_hist(std::vector<std::tuple<uint64_t, uint64_t>> vals) {
	for (auto v: vals) {
		printf("    <= %10" PRIu64 "  -> %5" PRIu64 "\n",std::get<0>(v), std::get<1>(v));
	}
}

void uDepotStats::print() {
	size_t i __attribute__((unused))=0;
	lock__.lock();
	for (auto s: udepot_stats_all__) {
		uint64_t count;
		UDEPOT_DBG("stats for thread: %zd", i++);

		count = s->get_latency.count();
		if (count) {
			double avg_ticks = (double)s->get_latency.ticks_total() / (double)count;
			printf(" GET: average: %lf (ticks)\n", avg_ticks);
			printf("      histogram (ticks and counts with sampling):\n");
			print_hist(s->get_latency.histogram());
		}

		count = s->put_latency.count();
		if (count) {
			double avg_ticks = (double)s->put_latency.ticks_total() / (double)count;
			printf(" PUT: average: %lf (ticks)\n", avg_ticks);
			printf("      histogram (ticks and counts with sampling):\n");
			print_hist(s->put_latency.histogram());
		}

		count = s->del_latency.count();
		if (count) {
			double avg_ticks = (double)s->del_latency.ticks_total() / (double)count;
			printf(" DEL: average: %lf (ticks)\n", avg_ticks);
			printf("      histogram (ticks and counts with sampling):\n");
			print_hist(s->del_latency.histogram());
		}
	}
	lock__.unlock();
}

} // end namespace
