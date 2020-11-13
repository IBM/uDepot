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
#ifndef	_SALSA_COMPILER_H_
#define	_SALSA_COMPILER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>

#include "libos/os-types.h"

#ifndef	CACHE_LINE_SIZE
#define	CACHE_LINE_SIZE	64U
#endif

#define	SECTOR_BSIZE	512U
#define SECTOR_BITS	9U
#define B2SEC(b)	((b) >> SECTOR_BITS)
#define SEC2B(s)	((s) << SECTOR_BITS)

#ifdef	__GNUC__
#ifndef likely
#define	likely(x)	__builtin_expect((x),1)
#endif
#ifndef unlikely
#define unlikely(x)	__builtin_expect((x),0)
#endif
#else  /* __GNUC__ */
#ifndef likely
#define likely(x)	(x)
#endif
#ifndef unlikely
#define unlikely(x)	(x)
#endif
#endif	/* !__GNUC__ */

# define __force        __attribute__((force))
#define ACCESS_ONCE(x) (*(volatile typeof(x) *) &(x))

#define	BUILD_BUG_ON(cond)	typedef char static_assertion[(0 != (cond)) ? -1 : 1]

#define	os_container_of(ptr, type, member) ({	\
			const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
			(type *)( (char *)__mptr - offsetof(type, member) );})


#ifndef	BITS_PER_BYTE
#define	BITS_PER_BYTE			(8U)
#endif
#define	BITS_PER_BYTE_SHIFT_BITS	(3U)

#ifndef	BYTES_TO_BITS
#define	BYTES_TO_BITS(bytes)	((bytes) << BITS_PER_BYTE_SHIFT_BITS)
#endif

#ifndef	BITS_PER_LONG
#define	BITS_PER_LONG	(sizeof(long) << BITS_PER_BYTE_SHIFT_BITS)
#endif

#define	ATOu64	"lu"

#define is_power_of_two(x)	((0 < (x)) && (0 == ((x) & ((x) - 1))))

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
/* if you see the current behaviour counter-intuitive and would like to   */
/* change it, no problem, just first get in touch with SAT@zrl            */

/* compile-time assert begin */
#define STATIC_ASSERT(_COND,_MSG) typedef __attribute__((unused)) char static_assertion_##_MSG[(!!(_COND))*2-1]
#define COMPILE_TIME_ASSERT5(X,I,L) STATIC_ASSERT(X,static_assertion_at_line_##I##L)
#define COMPILE_TIME_ASSERT4(X,I,L) COMPILE_TIME_ASSERT5(X,I,L)
#define COMPILE_TIME_ASSERT3(X,L) STATIC_ASSERT(X,static_assertion_at_line_##L)
#define COMPILE_TIME_ASSERT2(X,L) COMPILE_TIME_ASSERT3(X,L)
#define COMPILE_TIME_ASSERT(X)    COMPILE_TIME_ASSERT2(X,__LINE__)
#define COMPILE_TIME_ASSERT_INFO(X,I) COMPILE_TIME_ASSERT4(X,I,__LINE__)

/* check that a _VAR is positive and a power of two */
#define STATIC_ASSERT_POW2(_VAR) STATIC_ASSERT(_VAR && !(_VAR & (_VAR - 1)), _VAR##_not_power_of_two)
/* compile-time assert end */

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

static inline void round_power_two(u32 *const n, u8 *const bits)
{
	u32 v    = 1U;
	u8 nbits = 0U;
	while (v < *n) {
		v <<= 1;
		nbits++;
	}
	*bits = nbits;
	*n    = v;
}

#define max2(a,b) \
  ({ __typeof__ (a) _a = (a); \
      __typeof__ (b) _b = (b); \
    _a >= _b ? _a : _b; })

#define min2(a,b) \
  ({ __typeof__ (a) _a = (a); \
      __typeof__ (b) _b = (b); \
    _a <= _b ? _a : _b; })

static inline u32 min2_u32(const u32 val1, const u32 val2)
{
	return val1 <= val2 ? val1 : val2;
}

static inline u8 pow2_to_nr_bits(const u64 val1)
{
	u64 v = val1;
	u8 nbits = 0;
	STATIC_ASSERT_POW2(val1);
	while (v >>= 1)
		nbits++;
	return nbits;
}

#endif	/* _SALSA_COMPILER_H_ */
