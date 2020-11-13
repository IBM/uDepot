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

#ifndef UDEPOT_STATS_HH__
#define UDEPOT_STATS_HH__

#include "util/delay_histogram.hh"

namespace udepot {

class uDepotStats {
	delay_histogram put_latency, get_latency, del_latency;
	uDepotStats();

public:
	/* static methods that use thread-local variables */

	static void tls_init();
	static void tls_init_if_needed();

	typedef delay_histogram::ctx put_ctx;
	static put_ctx put_start(void);
	static void put_stop(put_ctx);

	typedef delay_histogram::ctx get_ctx;
	static get_ctx get_start(void);
	static void get_stop(get_ctx);

	typedef delay_histogram::ctx del_ctx;
	static del_ctx del_start(void);
	static void del_stop(del_ctx);

	static void print();
};

}

#endif // UDEPOT_STATS_HH__

