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

#include "uDepot/net/mc-timer.hh"

namespace memcache {
void mc_timer::timer_thread(void) {
	while (alive_m) {
		sleep(1);
		current_time_m = time(nullptr);
	}
}
};
