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

#ifndef _UDEPOT_PROFILING_HH_
#define _UDEPOT_PROFILING_HH_

#include <util/ticks.hh>

#if defined(UDEPOT_CONF_SDT)

#include <sys/sdt.h>
# if 0 // enable this this for custom sampling
#define PROBE_TICKS_START(id)                                           \
	static thread_local uint32_t __count_ ## id = 0;                    \
	uint64_t __ticks_ ## id ## __start__ = 0;                           \
	bool __sample ## id = (__count_ ## id)++ % 32 == 0;                \
	if (__sample ## id)                                                 \
		__ticks_ ## id ## __start__ = ticks::get_ticks();               \

#define PROBE_TICKS_END(id)  \
do {                                                                      \
	if (__sample ## id) {                                                 \
		uint64_t t = ticks::get_ticks() - (__ticks_ ## id ## __start__);  \
		DTRACE_PROBE1(udepot, id, t);                                     \
	}                                                                     \
} while (0)

#define PROBE_TICKS_END2(id,x1)  \
do {                                                                      \
	if (__sample ## id) {                                                 \
		uint64_t t = ticks::get_ticks() - (__ticks_ ## id ## __start__);  \
		DTRACE_PROBE2(udepot, id, t, x1);                                 \
	}                                                                     \
} while (0)
#else
#define PROBE_TICKS_START(id) \
	uint64_t __ticks_ ## id ## __start__ = ticks::get_ticks();

#define PROBE_TICKS_END(id)  \
do { \
	uint64_t t = ticks::get_ticks() - (__ticks_ ## id ## __start__);  \
	DTRACE_PROBE1(udepot, id, t);                                     \
} while (0)

#define PROBE_TICKS_END2(id, x1)  \
do { \
	uint64_t t = ticks::get_ticks() - (__ticks_ ## id ## __start__);  \
	DTRACE_PROBE2(udepot, id, t, x1);                                 \
} while (0)
#endif

#else // !UDEPOT_CONF_SDT define operations as no-ops

#define DTRACE_PROBE(prov,probe)                                          do {} while (0)
#define DTRACE_PROBE1(prov,probe,p1)                                      do {} while (0)
#define DTRACE_PROBE2(prov,probe,p1,p2)                                   do {} while (0)
#define DTRACE_PROBE3(prov,probe,p1,p2,p3)                                do {} while (0)
#define DTRACE_PROBE4(prov,probe,p1,p2,p3,p4)                             do {} while (0)
#define DTRACE_PROBE5(prov,probe,p1,p2,p3,p4,p5)                          do {} while (0)
#define DTRACE_PROBE6(prov,probe,p1,p2,p3,p4,p5,p6)                       do {} while (0)
#define DTRACE_PROBE7(prov,probe,p1,p2,p3,p4,p5,p6,p7)                    do {} while (0)
#define DTRACE_PROBE8(prov,probe,p1,p2,p3,p4,p5,p6,p7,p8)                 do {} while (0)
#define DTRACE_PROBE9(prov,probe,p1,p2,p3,p4,p5,p6,p7,p8,p9)              do {} while (0)
#define DTRACE_PROBE10(prov,probe,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10)         do {} while (0)
#define DTRACE_PROBE11(prov,probe,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11)     do {} while (0)
#define DTRACE_PROBE12(prov,probe,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12) do {} while (0)


#define PROBE_TICKS_START(id) do {} while (0)
#define PROBE_TICKS_END(id)   do {} while (0)
#define PROBE_TICKS_END2(id)   do {} while (0)

#endif // UDEPOT_CONF_SDT


#endif // _UDEPOT_PROFILING_HH_
