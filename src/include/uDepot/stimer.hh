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

// Simple timer targeted for ad-hoc time measurements
// starts timer at construction, prints duration at destructor
//
// Example:
//  STimer t("REMOTE GET")
//  return cli->remote_get(key, val_out);

#ifndef STIMER_HH_
#define STIMER_HH_

#include <iostream>
#include <chrono>

namespace udepot {

struct STimer {
	using Clock = std::chrono::high_resolution_clock;
	using Unit  = std::chrono::microseconds;

	std::string       st_msg;
	Clock::time_point st_t0;

	STimer(std::string msg)
	    : st_msg(msg)
	    , st_t0(Clock::now()) {}

	~STimer() {
		auto dt = Clock::now() - st_t0;
		std::cout << st_msg
		          << ": "
		          << std::chrono::duration_cast<Unit>(dt).count()
		          << " us"
		          << std::endl;
	}

	STimer() = delete;
	STimer(const STimer&) = delete;
	//STimer& operator=(const& STimer) = delete;
};

}
#endif /* ifndef STIMER_HH_
 */

