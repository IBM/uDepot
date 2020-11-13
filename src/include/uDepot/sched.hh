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

#ifndef	_UDEPOT_SCHED_H_
#define	_UDEPOT_SCHED_H_

#include <pthread.h>
#include "trt/uapi/trt.hh"

namespace udepot {

class PthreadSched {
public:
	static inline void yield(void) { pthread_yield(); }
};

class TrtSched {
public:
	static inline void yield(void) { trt::T::yield(); }
};

}


#endif /* _UDEPOT_SCHED_H_ */

