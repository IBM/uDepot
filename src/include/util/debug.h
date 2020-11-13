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
#ifndef _UDEPOT_DEBUG_H_
#define _UDEPOT_DEBUG_H_

#include <assert.h>
#include <stdio.h>
#include "util/types.h"

#define DBG_fmt   "%s:%d: %s() "
#define DBG_arg   __FILE__, __LINE__, __FUNCTION__
#define MSG_fmt   "%s:%d: %s() "
#define MSG_arg   __FILE__, __LINE__, __FUNCTION__
#define ERR_fmt   "%s:%d: %s() "
#define ERR_arg   __FILE__, __LINE__, __FUNCTION__

#define UDEPOT_ERR(_fmt, _arg...) do { \
	fprintf(stderr, ERR_fmt _fmt "\n", ERR_arg, ##_arg); \
	fflush(stderr); \
} while (0)

#define UDEPOT_MSG(_fmt, _arg...) do { \
	fprintf(stdout, MSG_fmt _fmt "\n", MSG_arg, ##_arg); \
	fflush(stdout); \
} while (0)

#define UDEPOT_DBG(_fmt, _arg...) do { \
	fprintf(stdout, DBG_fmt _fmt "\n", DBG_arg, ##_arg); \
	fflush(stdout); \
} while (0)

#if !defined(DEBUG)
 #undef  UDEPOT_DBG
#define UDEPOT_DBG(_fmt, _arg...)  do {} while (0)
#endif

// use assert() if you want something that's checked only if !NDEBUG is defined
#define UDEPOT_BUG_ON(x) do {                                               \
  if (x) {                                                                  \
    fprintf(stderr, ERR_fmt_str _format ": BUG: %s \n", ERR_fmt_args, #x);  \
    abort();                                                                \
  }                                                                         \
} while (0)


// XXX: Code below does not belong in debug.h

#define is_power_of_two(x)	((0 < (x)) && (0 == ((x) & ((x) - 1))))
#define unlikely(x)	__builtin_expect((x),0)

/* helper functions for compile-time calculation of the min # of bits    */
#define NUM_BITS2(_x) (((_x)&2)?1:0)
#define NUM_BITS4(_x) (((_x)&(0xC))?(2+NUM_BITS2((_x)>>2)):(NUM_BITS2(_x)))
#define NUM_BITS8(_x) (((_x)&0xF0)?(4+NUM_BITS4((_x)>>4)):(NUM_BITS4(_x)))
#define NUM_BITS16(_x) (((_x)&0xFF00)?(8+NUM_BITS8((_x)>>8)):(NUM_BITS8(_x)))
#define NUM_BITS32(_x) (((_x)&0xFFFF0000UL)?(16+NUM_BITS16((_x)>>16)):(NUM_BITS16(_x)))
#define NUM_BITS64(_x) (((_x)&0xFFFFFFFF00000000ULL)?(32+NUM_BITS32((_x)>>32)):(NUM_BITS32(_x)))

/* returns the minimum number of bits necessary to store the given number */
/* NOTE that this is not the min number of bits for storing X values!     */
/* for example, to store number 3, you need 2 bits, ie NUM_BITS(3) --> 2  */
/* however, to store number 4, you need 3 bits, ie NUM_BITS(4) --> 3      */
#define NUM_BITS(_x) ((_x)==0?0:NUM_BITS64(_x)+1)

namespace udepot {
static inline u64 udiv_round_up(const u64 dividend, const u64 divisor)
{
	return 0UL != divisor ? (dividend + divisor - 1U) / divisor : 1U;
}

static inline u64 uceil_up(const u64 val, const u64 divisor)
{
	return divisor * udiv_round_up(val, divisor);
}

static inline u64 align_up(const u64 val, const u64 alignment)
{
	return uceil_up(val, alignment);
}

static inline u64 align_down(const u64 val, const u64 alignment)
{
	return 0ULL != alignment ? val / alignment * alignment : 0ULL;
}

static inline u64 round_down_pow2(u64 val)
{
	int bit_nr = 0;
	while (val) {
		val >>= 1;
		bit_nr ++;
	}
	return 0 < bit_nr ? 1UL << (bit_nr - 1) : 0;
}


#define mem_barrier()			__sync_synchronize()

}; 				// end namespace udepot
#endif	/* _UDEPOT_DEBUG_H_ */
