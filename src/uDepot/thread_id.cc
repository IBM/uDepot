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

#include <limits>
#include <mutex>
#include <cassert>

#include "uDepot/thread-id.hh"

namespace udepot {

thread_local unsigned ThreadId::TLS_id_      = ThreadId::TLS_id_unitialized;
unsigned              ThreadId::TLS_id_next_ = 0;
std::mutex            ThreadId::lock_;

int ThreadId::allocate(unsigned &tid) {
	std::lock_guard<std::mutex> lg(ThreadId::lock_);
	if (TLS_id_ != ThreadId::TLS_id_unitialized)
		return EALREADY;
	// assert(TLS_id_ == ThreadId::TLS_id_unitialized);
	TLS_id_ = TLS_id_next_;
	if (TLS_id_next_ < std::numeric_limits<unsigned>::max())
		TLS_id_next_++;
	tid = TLS_id_;
	return 0;
}


int ThreadId::release() {
	if (TLS_id_ == ThreadId::TLS_id_unitialized)
		return EALREADY;
	TLS_id_ = TLS_id_unitialized;
	return 0;
}

unsigned ThreadId::get() {
	return TLS_id_;
}

}
