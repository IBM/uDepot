/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

#ifndef MISC_H__
#define MISC_H__

#if defined(__linux__)
	/* pthread spinlock wrappers */

	#include <stdbool.h>
	#include <pthread.h>
	#include <errno.h> /* EBUSY */
	#include <assert.h>
	#define spinlock_t       pthread_spinlock_t

	static inline void
	spinlock_init(spinlock_t *lock)
	{
		int err __attribute__((unused)) = pthread_spin_init(lock, 0);
		assert(!err);
	}

	static inline void
	spin_lock(spinlock_t *lock)
	{
		int err __attribute__((unused)) = pthread_spin_lock(lock);
		assert(!err);
	}

	static inline void
	spin_unlock(spinlock_t *lock)
	{
		int err __attribute__((unused)) = pthread_spin_unlock(lock);
		assert(!err);
	}

	static inline bool
	spin_try_lock(spinlock_t *lock)
	{
		int err = pthread_spin_trylock(lock);
		if (err == EBUSY)
			return false;
		assert(!err);
		return true;
	}
#elif defined(__APPLE__)
	#include <libkern/OSAtomic.h>
	#define spinlock_t OSSpinLock
	#define spinlock_init(x) do { *(x) = 0; } while (0)
	#define spin_lock(x) OSSpinLockLock(x)
	#define spin_unlock(x) OSSpinLockUnlock(x)
#endif

#include <stdlib.h> // malloc
#include <stdio.h> // perror

#define xmalloc(s) ({              \
	void *ret_ = malloc(s);    \
	if (ret_ == NULL) {        \
		perror("malloc");  \
		exit(1);           \
	}                          \
	ret_;})

#define xmemalign(memptr, alignment, size) ({ \
    int err_ = posix_memalign(memptr, alignment, size); \
    if (err_) {                                         \
        fprintf(stderr, "posix_memalign failed");       \
        exit(1);                                        \
    }                                                   \
})

#define xrealloc(ptr, s) ({           \
	void *ret_ = realloc(ptr, s); \
	if (ret_ == NULL) {           \
		perror("realloc");    \
		exit(1);              \
	}                             \
	ret_;})

#define xpthread_create(thr, attr, fn, arg) do {        \
        int ret__ = pthread_create(thr, attr, fn, arg); \
        if (ret__ < 0) {                                \
               perror("pthread_create");                \
               exit(1);                                 \
        }                                               \
} while (0)

#define xpthread_creat(thr, fn, arg) xpthread_create(thr, NULL, fn, arg) 

#define DIV_ROUNDUP(n,d) (((n) + (d) - 1) / (d))

#define dbg_print_str__ "%4ld>>>>> %s() [%s +%d]"
#define dbg_print_arg__ trtgettid(), __FUNCTION__, __FILE__, __LINE__
#define dbg_print(msg ,fmt, args...)\
    printf(dbg_print_str__ " " msg fmt , dbg_print_arg__ , ##args)
    //printf(dbg_print_str__ " " msg "\033[31m" fmt "\033[0m" , dbg_print_arg__ , ##args)

#define XDEBUG
/*
#define msg(fmt,args...)      dbg_print("msg:",fmt, ##args)
#if defined(XDEBUG)
    #define dmsg(fmt,args...) dbg_print("dbg:",fmt, ##args)
#else
    #define dmsg(fmt,args...) do { } while (0)
#endif
*/

// gettid
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
static inline long trtgettid(void)
{
	return syscall(SYS_gettid);
}

#define tmsg(fmt, args...) do { printf("%4ld> " fmt, trtgettid(), ##args); } while (0)

static inline char *
ul_hstr(unsigned long ul)
{
	#define UL_HSTR_NR 16
	static __thread int i=0;
	static __thread char buffs[UL_HSTR_NR][16];
	char *b = buffs[i++ % UL_HSTR_NR];
	#undef UL_HSTR_NR

	char m;
	double t;
	if (ul < 1000) {
		m = ' ';
		t = (double)ul;
	} else if (ul < 1000*1000) {
		m = 'K';
		t = (double)ul/(1000.0);
	} else if (ul < 1000*1000*1000) {
		m = 'M';
		t = (double)ul/(1000.0*1000.0);
	} else {
		m = 'G';
		t = (double)ul/(1000.0*1000.0*1000.0);
	}

	snprintf(b, 16, "%5.1lf%c", t, m);
	return b;

}

#if !defined(NYI)
#define NYI() do { \
	fprintf(stderr, "%s:%s(): NYI\n", __FILE__, __FUNCTION__); \
	abort(); \
} while (0)
#endif

#define	likely(x)	__builtin_expect((x),1)
#define unlikely(x)	__builtin_expect((x),0)

#define MAX(x,y)        \
({ __typeof__(x) x__ = (x); \
   __typeof__(y) y__ = (y); \
   x__ > y__ ? x__ : y__; })

#define MIN(x,y)        \
({ __typeof__(x) x__ = (x); \
   __typeof__(y) y__ = (y); \
   x__ < y__ ? x__ : y__; })


#define CL_BYTES 128
#define CACHE_ALIGNED __attribute__((aligned(CL_BYTES)))

#endif /* MISC_H__ */
