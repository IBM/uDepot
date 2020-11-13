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
#ifndef	_SALSA_CDS_QUEUE_H_
#define	_SALSA_CDS_QUEUE_H_
#include "data-structures/list.h"
#include "generic/compiler.h"
#include "libos/os-atomic.h"
#include "libos/os-debug.h"
#include "libos/os-lock.h"

struct cds_queue_base {
	os_spinlock_t        sl;
	struct cds_list_head lst;
	os_atomic64_t        size;
} __attribute__((aligned(CACHE_LINE_SIZE)));

struct cds_queue {
	struct cds_queue_base qbase;
	os_atomic32_t        wait_nr;
	os_waitqueue_head_t  wq;
} __attribute__((aligned(CACHE_LINE_SIZE)));

/* cds_queue_base */

static inline void cds_queue_base_init(struct cds_queue_base *const q)
{
	assert(0 == ((uintptr_t) (&q->sl) & (sizeof(u64) - 1U)));
	assert(0 == ((uintptr_t) (&q->size) & (sizeof(u64) - 1U)));
	os_spinlock_init(&q->sl);
	CDS_INIT_LIST_HEAD(&q->lst);
	os_atomic64_zero(&q->size);
}

static inline void cds_queue_base_enqueue_head(
	struct cds_queue_base *const q,
	struct cds_list_head *const l)
{
	os_spinlock_lock(&q->sl);
	cds_list_add(l, &q->lst);
	os_atomic64_inc(&q->size);
	os_spinlock_unlock(&q->sl);
}

static inline void cds_queue_base_enqueue_tail(
	struct cds_queue_base *const q,
	struct cds_list_head *const l)
{
	os_spinlock_lock(&q->sl);
	cds_list_add_tail(l, &q->lst);
	os_atomic64_inc(&q->size);
	os_spinlock_unlock(&q->sl);
}

static inline u64 cds_queue_base_enqueue_tail_ret_cnt(
	struct cds_queue_base *const q,
	struct cds_list_head *const l)
{
	u64 ret;
	os_spinlock_lock(&q->sl);
	cds_list_add_tail(l, &q->lst);
	ret = os_atomic64_inc_return(&q->size);
	os_spinlock_unlock(&q->sl);
	return ret;
}

static inline void cds_queue_base_enqueue_tail_irqsave(
	struct cds_queue_base *const q,
	struct cds_list_head *const l)
{
	unsigned long flags;
	os_spinlock_lock_irqsave(&q->sl, flags);
	cds_list_add_tail(l, &q->lst);
	os_atomic64_inc(&q->size);
	os_spinlock_unlock_irqrestore(&q->sl, flags);
}

static inline void cds_queue_base_enqueue_tail_bare(
	struct cds_queue_base *const q,
	struct cds_list_head *const l)
{
	cds_list_add_tail(l, &q->lst);
	os_atomic64_inc(&q->size);
}

static inline void cds_queue_base_del(
	struct cds_queue_base *const q,
	struct cds_list_head *const l)
{
	os_spinlock_lock(&q->sl);
	cds_list_del(l);
	assert(0 < os_atomic64_read(&q->size));
	os_atomic64_dec(&q->size);
	os_spinlock_unlock(&q->sl);
}

static inline void cds_queue_base_del_bare(
	struct cds_queue_base *const q,
	struct cds_list_head *const l)
{
	cds_list_del(l);
	assert(0 < os_atomic64_read(&q->size));
	os_atomic64_dec(&q->size);
}

static inline void cds_queue_base_del_irqsave(
	struct cds_queue_base *const q,
	struct cds_list_head *const l)
{
	unsigned long flags;
	os_spinlock_lock_irqsave(&q->sl, flags);
	cds_list_del(l);
	assert(0 < os_atomic64_read(&q->size));
	os_atomic64_dec(&q->size);
	os_spinlock_unlock_irqrestore(&q->sl, flags);
}

static inline int cds_queue_base_is_empty(struct cds_queue_base *const q)
{
	int empty;
	os_spinlock_lock(&q->sl);
	empty = cds_list_empty(&q->lst);
	os_spinlock_unlock(&q->sl);
	return empty;
}

static inline int cds_queue_base_is_empty_irqsave(struct cds_queue_base *const q)
{
	int empty;
	unsigned long flags;
	os_spinlock_lock_irqsave(&q->sl, flags);
	empty = cds_list_empty(&q->lst);
	os_spinlock_unlock_irqrestore(&q->sl, flags);
	return empty;
}


static inline u64 cds_queue_base_size(struct cds_queue_base *const q)
{
	return os_atomic64_read(&q->size);
}

static inline void cds_queue_base_lock(struct cds_queue_base *const q)
{
	os_spinlock_lock(&q->sl);
}

static inline void cds_queue_base_unlock(struct cds_queue_base *const q)
{
	os_spinlock_unlock(&q->sl);
}

static inline os_atomic64_t *cds_queue_base_size_ref(struct cds_queue_base *const q)
{
	return &q->size;
}


/* cds_queue */

static inline void
cds_queue_init(struct cds_queue *const q)
{
	assert(0 == ((uintptr_t) (&q->wq) & (sizeof(u64) - 1U)));
	cds_queue_base_init(&q->qbase);
	os_waitqueue_head_init(&q->wq);
	os_atomic32_zero(&q->wait_nr);
}


#define CDSQL_WRAP_BARE(fn_ret_t, fn_name)                \
static inline fn_ret_t cds_queue_ ## fn_name(             \
	struct cds_queue *const q,                        \
	struct cds_list_head *const l) {                  \
	return cds_queue_base_ ## fn_name(&q->qbase, l);  \
}

CDSQL_WRAP_BARE(void, enqueue_head)
CDSQL_WRAP_BARE(void, enqueue_tail)
CDSQL_WRAP_BARE(u64,  enqueue_tail_ret_cnt)
CDSQL_WRAP_BARE(void, enqueue_tail_irqsave)
CDSQL_WRAP_BARE(void, enqueue_tail_bare)
CDSQL_WRAP_BARE(void, del)
CDSQL_WRAP_BARE(void, del_bare)
CDSQL_WRAP_BARE(void, del_irqsave)

#define CDSQ_WRAP_BARE(fn_ret_t, fn_name)                 \
static inline fn_ret_t cds_queue_ ## fn_name(             \
	struct cds_queue *const q) {                      \
	return cds_queue_base_ ## fn_name(&q->qbase);     \
}

CDSQ_WRAP_BARE(int, is_empty)
CDSQ_WRAP_BARE(int, is_empty_irqsave)
CDSQ_WRAP_BARE(u64, size)
CDSQ_WRAP_BARE(void, lock)
CDSQ_WRAP_BARE(void, unlock)
CDSQ_WRAP_BARE(os_atomic64_t *, size_ref)

#undef CDSQL_WRAP_BARE
#undef CDSQ_WRAP_BARE

static inline void cds_queue_enqueue_head_wakeup_all(
	struct cds_queue     *const q,
	struct cds_list_head *const l)
{
	cds_queue_enqueue_head(q, l);
	os_wakeup_all(&q->wq);
}


static inline void cds_queue_enqueue_tail_wakeup_all(
	struct cds_queue     *const q,
	struct cds_list_head *const l)
{
	cds_queue_enqueue_tail(q, l);
	os_wakeup_all(&q->wq);
}

static inline u64 cds_queue_enqueue_tail_wakeup_all_ret_cnt(
	struct cds_queue     *const q,
	struct cds_list_head *const l)
{
	const u64 ret = cds_queue_enqueue_tail_ret_cnt(q, l);
	os_wakeup_all(&q->wq);
	return ret;
}

static inline void cds_queue_enqueue_tail_wakeup_all_interruptible(
	struct cds_queue     *const q,
	struct cds_list_head *const l)
{
	cds_queue_enqueue_tail(q, l);
	os_wakeup_all_interruptible(&q->wq);
}

static inline void cds_queue_enqueue_tail_wakeup(
	struct cds_queue     *const q,
	struct cds_list_head *const l)
{
	cds_queue_enqueue_tail(q, l);
	os_wakeup(&q->wq);
}

static inline void cds_queue_enqueue_tail_wakeup_interruptible(
	struct cds_queue     *const q,
	struct cds_list_head *const l)
{
	cds_queue_enqueue_tail(q, l);
	os_wakeup_interruptible(&q->wq);
}

static inline void cds_queue_enqueue_tail_wakeup_interruptible_irqsave(
	struct cds_queue     *const q,
	struct cds_list_head *const l)
{
	cds_queue_enqueue_tail_irqsave(q, l);
	os_wakeup_interruptible(&q->wq);
}

static inline void cds_queue_enqueue_tail_wakeup_all_irqsave(
	struct cds_queue     *const q,
	struct cds_list_head *const l)
{
	cds_queue_enqueue_tail_irqsave(q, l);
	os_wakeup_all(&q->wq);
}

static inline void cds_queue_enqueue_tail_wakeup_irqsave(
	struct cds_queue     *const q,
	struct cds_list_head *const l)
{
	cds_queue_enqueue_tail_irqsave(q, l);
	os_wakeup(&q->wq);
}

static inline void cds_queue_wakeup_all(
	struct cds_queue     *const q)
{
	os_wakeup_all(&q->wq);
}

static inline void cds_queue_wakeup_all_interruptible(
	struct cds_queue     *const q)
{
	os_wakeup_all_interruptible(&q->wq);
}

static inline void cds_queue_wakeup(
	struct cds_queue     *const q)
{
	os_wakeup(&q->wq);
}

static inline void cds_queue_wakeup_interruptible(
	struct cds_queue     *const q)
{
	os_wakeup_interruptible(&q->wq);
}

static inline void cds_queue_wakeup_interruptible_nr(
	struct cds_queue     *const q,
	const u32              nr)
{
	os_wakeup_interruptible_nr(&q->wq, nr);
}

/* unmodified */

static inline int cds_queue_wait_nr(struct cds_queue *const q)
{
	return os_atomic32_read(&q->wait_nr);
}


#define	cds_queue_wait_event_timeout(q, cond, timeout)	do {	\
		os_wait_event_timeout(((q)->wq), (cond), (timeout));	\
	} while(0)

#define	cds_queue_wait_event(q, cond)	do {	\
		os_wait_event(((q)->wq), (cond));	\
	} while(0)

#define	cds_queue_wait_event_interruptiple(q, cond)	do {	\
		os_wait_event_interruptible(((q)->wq), (cond));	\
	} while(0)

#define	cds_queue_wait_event_interruptiple_timeout(q, cond, timeout)	do {	\
		os_wait_event_interruptible_timeout(((q)->wq), (cond), (timeout)); \
	} while(0)

#define	cds_queue_wait_event_interruptiple_exclusive(q, cond)	do {	\
		os_atomic32_inc(&(q)->wait_nr);				\
		os_wait_event_interruptible_exclusive(((q)->wq), (cond)); \
		os_atomic32_dec(&(q)->wait_nr);				\
	} while(0)

#define	cds_queue_base_pop_head(queue, type, member)				\
	({								\
		type *ptr = NULL;					\
		os_spinlock_lock(&(queue)->sl);				\
		if (!cds_list_empty(&(queue)->lst)) {			\
			ptr = cds_list_first_entry(&(queue)->lst, type, member); \
			cds_list_del(&ptr->list);			\
			assert(0 < os_atomic64_read(&(queue)->size));	\
			os_atomic64_dec(&(queue)->size);		\
		}							\
		os_spinlock_unlock(&(queue)->sl);			\
		ptr;							\
	})


#define	cds_queue_base_pop_head_ret_cnt(queue, type, member, cnt_ptr)	\
	({								\
		type *ptr = NULL;					\
		os_spinlock_lock(&(queue)->sl);				\
		if (!cds_list_empty(&(queue)->lst)) {			\
			ptr = cds_list_first_entry(&(queue)->lst, type, member); \
			cds_list_del(&ptr->list);			\
			assert(0 < os_atomic64_read(&(queue)->size));	\
			*(cnt_ptr) = os_atomic64_dec_return(&(queue)->size); \
		}							\
		os_spinlock_unlock(&(queue)->sl);			\
		ptr;							\
	})

#define	cds_queue_base_pop_head_irqsave(queue, type, member)				\
	({								\
		type *ptr = NULL;					\
		unsigned long flags;					\
		os_spinlock_lock_irqsave(&(queue)->sl, flags);		\
		if (!cds_list_empty(&(queue)->lst)) {			\
			ptr = cds_list_first_entry(&(queue)->lst, type, member); \
			cds_list_del(&ptr->list);			\
			assert(0 < os_atomic64_read(&(queue)->size));	\
			os_atomic64_dec(&(queue)->size);		\
		}							\
		os_spinlock_unlock_irqrestore(&(queue)->sl, flags);		\
		ptr;							\
	})



#define	cds_queue_pop_head(q, t, m)                  cds_queue_base_pop_head(             &(q)->qbase, t, m)
#define	cds_queue_pop_head_ret_cnt(q, t, m, cp)      cds_queue_base_pop_head_ret_cnt(     &(q)->qbase, t, m, cp)
#define	cds_queue_pop_head_irqsave(q, t, m)          cds_queue_base_pop_head_irqsave(     &(q)->qbase, t, m)

// iterators
#define queue_base_for_each(q) list_for_each_entr

#endif	/* _SALSA_CDS_QUEUE_H_ */
