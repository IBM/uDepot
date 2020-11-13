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

#ifndef THREAD_ID_HH_
#define THREAD_ID_HH_

#include <limits>
#include <mutex>

// thread ids stored in thread-local storage. They are consecutive. Currently,
// no garbage collection is done on release(), but we might want to implement
// it.

namespace udepot {
class ThreadId {
public:
	static const unsigned TLS_id_unitialized = std::numeric_limits<unsigned>::max();
	static int allocate(unsigned &tid);
	static unsigned get();
	static int release();
private:
	static thread_local unsigned  TLS_id_;
	static unsigned               TLS_id_next_;
	static std::mutex             lock_;
};

}

#endif
