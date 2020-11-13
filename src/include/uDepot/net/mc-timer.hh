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
#ifndef	_UDEPOT_MC_TIMER_
#define	_UDEPOT_MC_TIMER_

#include <atomic>
#include <time.h>
#include <thread>
#include <unistd.h>

#include "util/debug.h"

namespace memcache {
class mc_timer {
public:
	mc_timer():process_start_m(time(nullptr)), current_time_m(process_start_m) {
		if ((time_t) -1 == process_start_m) {
			UDEPOT_ERR("time() returned invalid time (-1) error =%s.",
				strerror(errno));
		}
	}
	~mc_timer() { }
	time_t cur_time() const { return time(nullptr); }
#if	0 // disable thread for now, expiry time not expected to be in the common case
	int start() {
		alive_m = true;
		try {
			timer_thread_m = std::thread(&mc_timer::timer_thread, this);
		} catch (...) {
			UDEPOT_ERR("timer thread failed to be created, aborting.");
			return ENOMEM;
		}
		return 0;
	}
	void stop() {
		alive_m = false;
		if (timer_thread_m.joinable())
			timer_thread_m.join();
	}
#endif
	int start () {
		return 0;
	}
	void stop () { }
private:
	const time_t process_start_m;
	std::atomic<time_t> current_time_m;
#if	0 // disable thread for now, expiry time not expected to be in the common case
	std::thread timer_thread_m;
	std::atomic<bool> alive_m;
	void timer_thread(void);
#endif
};
};

#endif	// _UDEPOT_MC_TIMER_
