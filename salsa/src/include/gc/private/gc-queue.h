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
#ifndef	_SALSA_GC_QUEUE_H_
#define	_SALSA_GC_QUEUE_H_

#include "data-structures/list.h"
#include "generic/compiler.h"
#include "libos/os-atomic.h"
#include "libos/os-lock.h"

struct gc_queue {
	struct cds_list_head lst;
	os_spinlock_t        sl;
	os_atomic64_t        page_relocs;
	os_atomic64_t        seg_relocs;
	os_atomic64_t        lst_len;
	os_atomic64_t        threshold;
} __attribute__((aligned(CACHE_LINE_SIZE)));

static inline void gc_queue_init_stats(
	struct gc_queue *const fifo)
{
	os_atomic64_zero(&fifo->page_relocs);
	os_atomic64_zero(&fifo->seg_relocs);
}

static inline void gc_queue_init(
	struct gc_queue *const fifo)
{
	CDS_INIT_LIST_HEAD(&fifo->lst);
	os_atomic64_zero(&fifo->lst_len);
	os_spinlock_init(&fifo->sl);
	os_atomic64_zero(&fifo->threshold);
	gc_queue_init_stats(fifo);
}

static inline void gc_queue_add_page_relocs(
	struct gc_queue *const fifo,
	u32               relocs)
{
	os_atomic64_add(&fifo->page_relocs, relocs);
}

static inline void gc_queue_add_seg_reloc(
	struct gc_queue *const fifo)
{
	os_atomic64_inc(&fifo->seg_relocs);
}

static inline void gc_queue_enqueue(
	struct gc_queue      *const fifo,
        struct cds_list_head *const node)
{
	os_spinlock_lock(&fifo->sl);
	os_atomic64_inc(&fifo->lst_len);
	cds_list_add_tail(node, &fifo->lst);
	os_spinlock_unlock(&fifo->sl);
}

static inline void gc_queue_enqueue_head(
	struct gc_queue      *const fifo,
        struct cds_list_head *const node)
{
	os_spinlock_lock(&fifo->sl);
	os_atomic64_inc(&fifo->lst_len);
	cds_list_add(node, &fifo->lst);
	os_spinlock_unlock(&fifo->sl);
}

static inline void gc_queue_dequeue(
	struct gc_queue      *const fifo,
        struct cds_list_head *const node)
{
	os_spinlock_lock(&fifo->sl);
	if (!cds_list_empty(&fifo->lst))
		os_atomic64_dec(&fifo->lst_len);
	cds_list_del(node);
	os_spinlock_unlock(&fifo->sl);
}

#endif  /* _SALSA_GC_QUEUE_H_ */
