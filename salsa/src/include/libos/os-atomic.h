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
#ifndef _SALSA_ATOMIC_H_
#define _SALSA_ATOMIC_H_

#include "libos/os-types.h"

/* User space */

#include "generic/compiler.h"

typedef volatile s32			os_atomic32_t;
typedef volatile s64			os_atomic64_t;

#define os_atomic32_add(_IADDRP, _COUNT)	__sync_add_and_fetch(_IADDRP, _COUNT)
#define os_atomic32_sub(_IADDRP, _COUNT) 	__sync_sub_and_fetch(_IADDRP, _COUNT)
#define os_atomic32_set(_IADDRP, _COUNT)	(*((volatile s32 *) (_IADDRP)) = _COUNT)
#define os_atomic32_zero(_IADDRP)		os_atomic32_set(_IADDRP, 0U)
#define os_atomic32_read(_IADDRP)		(*((volatile s32 *) (_IADDRP)))
#define os_atomic32_inc(_IADDRP)		__sync_add_and_fetch(_IADDRP, 1U)
#define os_atomic32_dec(_IADDRP)		__sync_sub_and_fetch(_IADDRP, 1U)

#define os_atomic64_add(_IADDRP, _COUNT)	__sync_add_and_fetch(_IADDRP, _COUNT)
#define os_atomic64_sub(_IADDRP, _COUNT) 	__sync_sub_and_fetch(_IADDRP, _COUNT)
#define os_atomic64_set(_IADDRP, _COUNT)	(*((volatile s64 *) (_IADDRP)) = _COUNT)
#define os_atomic64_zero(_IADDRP)		os_atomic64_set(_IADDRP, 0U)
#define os_atomic64_read(_IADDRP)		(*((volatile s64 *) (_IADDRP)))
#define os_atomic64_inc(_IADDRP)		__sync_add_and_fetch(_IADDRP, 1U)
#define os_atomic64_dec(_IADDRP)		__sync_sub_and_fetch(_IADDRP, 1U)

#define os_atomic32_add_return(_IADDRP, _COUNT)	__sync_add_and_fetch(_IADDRP, _COUNT)
#define os_atomic32_sub_return(_IADDRP, _COUNT) __sync_sub_and_fetch(_IADDRP, _COUNT)
#define os_atomic32_inc_return(_IADDRP)		__sync_add_and_fetch(_IADDRP, 1U)
#define os_atomic32_dec_return(_IADDRP)		__sync_sub_and_fetch(_IADDRP, 1U)


#define os_atomic64_add_return(_IADDRP, _COUNT)	__sync_add_and_fetch(_IADDRP, _COUNT)
#define os_atomic64_sub_return(_IADDRP, _COUNT) __sync_sub_and_fetch(_IADDRP, _COUNT)
#define os_atomic64_inc_return(_IADDRP)		__sync_add_and_fetch(_IADDRP, 1U)
#define os_atomic64_dec_return(_IADDRP)		__sync_sub_and_fetch(_IADDRP, 1U)

#define os_mem_barrier()			__sync_synchronize()

#define os_atomic32_cmpxchg(_IADDRP,_OLDVAL,_NEWVAL)	__sync_val_compare_and_swap(_IADDRP,_OLDVAL,_NEWVAL)

#define os_atomic64_cmpxchg(_IADDRP,_OLDVAL,_NEWVAL)	__sync_val_compare_and_swap(_IADDRP,_OLDVAL,_NEWVAL)

#define os_set_bit(_IADDRP, _POS)   __sync_or_and_fetch((volatile int *)_IADDRP + (_POS >> 5), 1UL << (_POS & 31))
#define os_clear_bit(_IADDRP, _POS) __sync_and_and_fetch((volatile int *)_IADDRP + (_POS >> 5), ~(1UL << (_POS & 31)))
#define os_test_bit(_IADDRP, _POS)	((*((volatile char*)_IADDRP + (_POS >> 3))) & (1UL << (_POS & 7)))

/**
 * General comment:
 * All these atomic bit manipulation functions assume that the bit
 * array passed as parameter is at least 32-bit aligned
 */

/**
 * Return value:
 * 0: Test and set successful,   bit was 0 and now is 1
 * 1: Test and set unsuccessful, bit was found to be 1
 */
static inline int os_test_and_set_bit(const unsigned long *addr, int bit)
{
	os_atomic32_t *const p = (os_atomic32_t *) ((int *) addr + (bit >> 5));
	union {
		u32 u;
		s32 s;
	} oldval, newval;
        const u32 bitmask = 1U << (bit & 31);
        do {
                oldval.s = os_atomic32_read(p);
                if (0 != (oldval.u & bitmask))
                        return 1;       /* bit is already set */
                newval.u = oldval.u | bitmask;
        } while (oldval.s != os_atomic32_cmpxchg(p, oldval.s, newval.s));
        return 0;
}

/**
 * Return value:
 * 1: Test and set successful,   bit was 1 and now is 0
 * 0: Test and set unsuccessful, bit was found to be 0
 */
static inline int os_test_and_clear_bit(const unsigned long *addr, int bit)
{
	os_atomic32_t *const p = (os_atomic32_t *) ((int *) addr + (bit >> 5));
	union {
		u32 u;
		s32 s;
	} oldval, newval;
        const u32 bitmask = 1U << (bit & 31);
        do {
                oldval.s = os_atomic32_read(p);
                if (0 == (oldval.u & bitmask))
                        return 1;       /* bit is already set */
                newval.u = oldval.u & ~bitmask;
        } while (oldval.s != os_atomic32_cmpxchg(p, oldval.s, newval.s));
        return 0;
}

static inline int os_find_first_bit(unsigned long *addr, u32 size)
{
	int offset = 0;
	int i = *(int *) addr;
	int bit = __builtin_ffs(i) - 1;
	while (-1 == bit && size) {
		size   -= sizeof(int) << BITS_PER_BYTE_SHIFT_BITS;
		offset += sizeof(int) << BITS_PER_BYTE_SHIFT_BITS;
		addr   += sizeof(int);
		i = *(int *) addr;
		bit = __builtin_ffs(i) - 1;
	}
	if (-1 != bit)
		offset += bit;
	return offset;
}

static inline int os_find_next_bit(unsigned long *addr, u32 size, int cur_bit)
{
	/* words to skip */
	int *a = (int *) addr;
	int skip = cur_bit >> 5;
	int ignore = cur_bit & 31;
	int val;
	int offset = cur_bit;
	int bit;
	a += skip;
	size -= skip;
	val = *a & ~(1 << ignore);
	bit = __builtin_ffs(val) - 1;
	while (-1 == bit && size) {
		size   -= sizeof(int) << BITS_PER_BYTE_SHIFT_BITS;
		offset += sizeof(int) << BITS_PER_BYTE_SHIFT_BITS;
		val = *++a;
		bit = __builtin_ffs(val) - 1;
	}
	if (-1 != bit)
		offset += bit;
	return offset;
}

/* addr assumed to be a char or void *, size in bits */
#define os_for_each_set_bit(bit, addr, size)				\
	for ((bit) = os_find_first_bit((addr), size);			\
	     (bit) < size;						\
	     (bit) = os_find_next_bit((addr), (size), (bit) + 1))

#define	os_compiler_fence()			 asm volatile("" : : : "memory")

#endif /* _SALSA_ATOMIC_H_ */
