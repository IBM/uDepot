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

#ifndef _TICKS_HH_
#define _TICKS_HH_

namespace ticks {

#if defined(__i386__) || defined(__x86_64__)
static inline uint64_t get_ticks(void)
{
	uint32_t hi,low;
	uint64_t ret;

	__asm__ __volatile__ ("rdtsc" : "=a"(low), "=d"(hi));

	ret = hi;
	ret <<= 32;
	ret |= low;

	return ret;
}
#elif defined(__ia64__)
#include <asm/intrinsics.h>
static inline uint64_t get_ticks(void)
{
	uint64_t ret = ia64_getreg(_IA64_REG_AR_ITC);
	ia64_barrier();

	return ret;
}
#elif defined(__sparc__)
// linux-2.6.28/arch/sparc64/kernel/time.c
static inline uint64_t get_ticks(void)
{
	uint64_t t;
	__asm__ __volatile__ (
		"rd     %%tick, %0\n\t"
		"mov    %0,     %0"
		: "=r"(t)
	);
	return t & (~(1UL << 63));
}
#elif defined(__PPC64__)
//
// use the TB (time base register) for now. It has a lower frequency than the
// cpu clock, but that's the best I could find.
//
// $ cat /proc/cpuinfo  | grep timebase
// timebase        : 512000000
//
// read upper, read lower, re-read upper and if there is no overflow, return
// result.
static inline uint64_t get_ticks(void)
{
	unsigned int hi, lo, hi2;
	do {
		asm volatile(
			"mftbu %0; mftbl %1; mftbu %2"
			: "=r" (hi), "=r" (lo), "=r" (hi2)
		);
	} while (hi2 != hi);
	return (uint64_t)hi * 1000000000 + lo;
}
#else
#error "dont know how to count ticks"
#endif

} // end namespace ticks

#endif // _TICKS_HH_
