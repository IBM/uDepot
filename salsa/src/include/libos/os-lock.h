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
#ifndef _SALSA_LOCK_H_
#define _SALSA_LOCK_H_


#include <pthread.h>

/* RW Lock */
typedef pthread_rwlock_t		os_rwlock_t;

#define os_rwlock_init(lock_p)		pthread_rwlock_init(lock_p, NULL)
#define os_rwlock_destroy(lock_p)	pthread_rwlock_destroy(lock_p)
#define os_rwlock_down_read(lock_p)	pthread_rwlock_rdlock(lock_p)
#define os_rwlock_down_write(lock_p)	pthread_rwlock_wrlock(lock_p)
#define os_rwlock_up_read(lock_p)	pthread_rwlock_unlock(lock_p)
#define os_rwlock_up_write(lock_p)	pthread_rwlock_unlock(lock_p)

/* Spinlock */
typedef pthread_spinlock_t		os_spinlock_t;

#define os_spinlock_init(lock_p)	pthread_spin_init(lock_p, PTHREAD_PROCESS_SHARED)
#define os_spinlock_destroy(lock_p)	pthread_spin_destroy(lock_p)
#define os_spinlock_lock(lock_p)	pthread_spin_lock(lock_p)
#define os_spinlock_unlock(lock_p)	pthread_spin_unlock(lock_p)
#define os_spinlock_lock_irqsave(lock_p, f)	({(void) f; pthread_spin_lock(lock_p);})
#define os_spinlock_unlock_irqrestore(lock_p,f )	({(void) f; pthread_spin_unlock(lock_p);})

/* Mutex */
typedef pthread_mutex_t			os_mutex_t;

#define os_mutex_init(mutex_p)		pthread_mutex_init(mutex_p, NULL)
#define os_mutex_destroy(mutex_p) 	pthread_mutex_destroy(mutex_p)
#define os_mutex_lock(mutex_p)		pthread_mutex_lock(mutex_p)
#define os_mutex_trylock(mutex_p)	pthread_mutex_trylock(mutex_p)
#define os_mutex_unlock(mutex_p)	pthread_mutex_unlock(mutex_p)

/* Waitqueue head */

#include <time.h>
#include <sys/time.h>

struct pthread_waitqueue_head {
	pthread_cond_t  cond;
	pthread_mutex_t mtx;
};

typedef struct pthread_waitqueue_head		os_waitqueue_head_t;
#define	os_waitqueue_head_init(p)\
	({int __err = 0; do { \
			__err = pthread_cond_init(&((p)->cond), NULL); \
			if (0 == __err) \
				__err = pthread_mutex_init(&((p)->mtx), NULL); \
		} while (0); __err;})

#if	!(defined SALSA_USPACE)
/* !SALSA_USPACE */
#define os_wakeup_all(p)	do { \
		pthread_mutex_lock(&((p)->mtx)); \
		pthread_cond_broadcast(&((p)->cond)); \
		pthread_mutex_unlock(&((p)->mtx)); \
	} while (0)

#define os_wakeup_all_interruptible(p)	os_wakeup_all(p)

#define os_wakeup(p)	do { \
		pthread_mutex_lock(&((p)->mtx)); \
		pthread_cond_signal(&((p)->cond)); \
		pthread_mutex_unlock(&((p)->mtx)); \
	} while (0)

#define os_wakeup_interruptible(p)	os_wakeup(p)

#define os_wakeup_interruptible_nr(p, nr)	os_wakeup(p)

#define os_wait_event(p, condition)	do { \
		pthread_mutex_lock(&(&(p))->mtx); \
		while (!(condition)) \
			pthread_cond_wait(&(&(p))->cond, &(&(p))->mtx); \
		pthread_mutex_unlock(&(&(p))->mtx); \
	} while (0)

#define os_wait_event_interruptible(p, condition)	do { \
		pthread_mutex_lock(&(&(p))->mtx); \
		while (!(condition)) \
			pthread_cond_wait(&(&(p))->cond, &(&(p))->mtx); \
		pthread_mutex_unlock(&(&(p))->mtx); \
	} while (0)

#define os_wait_event_interruptible_exclusive(p, condition)	os_wait_event_interruptible(p, condition)

#define os_wait_event_timeout(p, condition, msec)	do { \
		int rc = 0; \
		struct timespec when; \
		clock_gettime(CLOCK_MONOTONIC, &when); \
		when.tv_nsec += msec * 1000 * 1000UL; \
		when.tv_sec += (msec / 1000UL + when.tv_nsec / (1000UL * 1000 * 1000) ); \
		when.tv_nsec %= 1000 * 1000 * 1000UL; /* handle the overflow in nsec */ \
		pthread_mutex_lock(&(&(p))->mtx); \
		while (!(condition) && 0 == rc) \
			rc = pthread_cond_timedwait(&(&(p))->cond, &(&(p))->mtx, &when); \
		pthread_mutex_unlock(&(&(p))->mtx); \
	} while (0)

#define os_wait_event_interruptible_timeout(p, condition, msec)	os_wait_event_timeout(p, condition, msec)

#else	/* !SALSA_USPACE */
/* SALSA_USPACE */
#define os_wakeup_all(p) (void)(p)

#define os_wakeup_all_interruptible(p)

#define os_wakeup(p)

#define os_wakeup_interruptible(p)

#define os_wakeup_interruptible_nr(p, nr)

#define os_wait_event(p, condition)	do { \
		while (!(condition)) \
			; \
	} while (0)

#define os_wait_event_interruptible(p, condition)	do { \
		while (!(condition)) \
			; \
	} while (0)

#define os_wait_event_timeout(p, condition, msec)	do {		\
		volatile int wait = 0;					\
		while (!(condition) && (100 * (msec) > wait++))		\
			;						\
	} while (0)

#define os_wait_event_interruptible_timeout(p, condition, msec)	os_wait_event_timeout(p, condition, msec)

#endif	/* !SALSA_USPACE */

/* completion variable */
struct os_completion {
	os_waitqueue_head_t	wq;
	volatile int            condition;
};
typedef struct os_completion		os_completion_t;

#define	os_init_completion(p)		do {		  \
		os_waitqueue_head_init(&(p)->wq);	  \
		(p)->condition = 0;			  \
	} while (0)

#define os_complete(p)			do {		 \
		pthread_mutex_lock(&((p)->wq.mtx));	 \
		(p)->condition = 1;			 \
		pthread_cond_broadcast(&((p)->wq.cond)); \
		pthread_mutex_unlock(&((p)->wq.mtx));	 \
	} while (0)

#define os_wait_for_completion(p)	do {				\
		pthread_mutex_lock(&(p)->wq.mtx);			\
		while(0 == (p)->condition)				\
			pthread_cond_wait(&(p)->wq.cond, &(p)->wq.mtx); \
		pthread_mutex_unlock(&(p)->wq.mtx);			\
	} while (0)

#endif /* _SALSA_LOCK_H_ */
