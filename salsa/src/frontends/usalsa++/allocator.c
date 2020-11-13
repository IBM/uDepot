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

#include "frontends/usalsa++/allocator.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "data-structures/cds-queue.h"
#include "data-structures/cds-queue.h"
#include "data-structures/list.h"
#include "gc/gc.h"
#include "libos/os-atomic.h"
#include "libos/os-thread.h"
#include "libos/os-time.h"
#include "libos/os-types.h"
#include "sto-ctlr/private/sto-capacity-mgr-common.h"
#include "sto-ctlr/private/sto-segment.h"
#include "sto-ctlr/scm-seg-alloc.h"

struct allocator {
	struct cds_queue         stage_fifo; /* in-flight */
	os_atomic32_t            allocating;
	struct cds_queue         done_fifo;
	const u64                seg_size;  /* # of grains */
	struct sto_capacity_mgr *scm;
	void                    *sc;
	struct scm_gfs_flags     gfs_flags;
	os_atomic32_t            exit;
	os_thread_t              alloc_th;
	os_atomic32_t            alloc_th_done;
	const u8                 ctlr_id;
	const u8                 cache_size;
	const u64                reserved_per_seg;
	seg_md_callback_t        seg_md_fn;
}__attribute__((aligned));

static inline u64
allocator_seg_next_range(
	struct allocator *const alltr,
	struct segment *const seg,
	const u64      len)
{
	union {
		s64 s;
		u64 u;
	} idx, next_idx;
	do {
		idx.s = os_atomic32_read(&seg->nr3);
		next_idx.u = idx.u + len;
		if (alltr->seg_size <= idx.u || alltr->seg_size < next_idx.u)
			return SALSA_INVAL_GRAIN;
	} while (idx.s != os_atomic32_cmpxchg(&seg->nr3, idx.s, next_idx.s));
	return idx.u;
}

__attribute__((warn_unused_result))
static int
allocator_allocate_segment(struct allocator *const alltr)
{
	struct segment *seg = NULL;
	const int err = scm_get_free_seg(alltr->scm, alltr->gfs_flags, &seg);
	if (0 != err) {
		if (-EAGAIN != err)
			ERR("No more free segments err=%d.", err);
		else
			os_msleep(10);
		return ENOSPC;
	}
	DBG("id=%u allocated seg=%lu size=%lu", alltr->ctlr_id,
		scm_get_seg_idx(alltr->scm, seg), cds_queue_size(&alltr->stage_fifo));
	segment_set_ctlr(seg, alltr->ctlr_id);
	segment_set_state(seg, SEG_LSA_ALLOCATOR);
	os_atomic32_set(&seg->nr3, 0); /* cur */
	assert(alltr->seg_size <= INT_MAX);
	assert(scm_get_seg_idx(alltr->scm, seg) < scm_get_seg_nr(alltr->scm));
	assert(alltr->seg_size == segment_valid_nr(seg));
	os_atomic32_set(&seg->nr4, alltr->seg_size); /* free */

	/* reserve grains for md and call seg_md callback fn */
	if (0 < alltr->reserved_per_seg) {
		u64 grain = allocator_seg_next_range(alltr, seg, alltr->reserved_per_seg);
		const u32 free_after_reserved __attribute__((unused)) = os_atomic32_sub_return(&seg->nr4, alltr->reserved_per_seg);
		assert(grain != SALSA_INVAL_GRAIN);
		segment_putn(seg, alltr->reserved_per_seg); // they won't be invalidated by the user
		assert(0 < free_after_reserved);
		grain += scm_seg_to_grain(alltr->scm, seg);
		alltr->seg_md_fn(alltr->sc, grain + alltr->seg_size - alltr->reserved_per_seg, alltr->reserved_per_seg);
	}
	cds_queue_enqueue_tail_wakeup_all(&alltr->stage_fifo, &seg->list);

	return 0;
}

void allocator_wait_for_grains(struct allocator *const alltr)
{
	DBG("alltr=%p waiting for staging fifo to be populated", alltr);
	cds_queue_wait_event_interruptiple_timeout(&alltr->stage_fifo,
						1 == os_atomic32_read(&alltr->exit) ||
						!cds_queue_is_empty(&alltr->stage_fifo), 1000UL);
}

__attribute__((warn_unused_result))
u64
allocator_allocate_grains(struct allocator *const alltr, const u64 len)
{
	struct segment *p, *q, *seg = NULL;
	u64 grain = SALSA_INVAL_GRAIN;
	os_spinlock_lock(&alltr->stage_fifo.qbase.sl);
retry:
	cds_list_for_each_entry_safe(p, q, &alltr->stage_fifo.qbase.lst, list) {
		grain = allocator_seg_next_range(alltr, p, len);
		if (SALSA_INVAL_GRAIN != grain) {
			if (alltr->seg_size == grain + len)
				cds_queue_del_bare(&alltr->stage_fifo, &p->list);
			seg = p;
			assert(scm_get_seg_idx(alltr->scm, seg) < scm_get_seg_nr(alltr->scm));
			break;
		}

		// recycle
		const u64 rem = alltr->seg_size - os_atomic32_read(&p->nr3);
		DBG("unused %lu grains in seg=%p.", rem, p);
		segment_putn(p, rem); // they won't be invalidated by the user
		cds_queue_del_bare(&alltr->stage_fifo, &p->list);
		if (0 == os_atomic32_sub_return(&p->nr4, rem))
			cds_queue_enqueue_tail_wakeup(&alltr->done_fifo, &p->list);
	}
	if (SALSA_INVAL_GRAIN == grain &&
		cds_list_empty(&alltr->stage_fifo.qbase.lst) &&
		0 == os_atomic32_cmpxchg(&alltr->allocating, 0, 1)) {
		os_spinlock_unlock(&alltr->stage_fifo.qbase.sl);
		const int rc = allocator_allocate_segment(alltr);
		if (0 == rc) {
			cds_queue_wakeup_all(&alltr->stage_fifo);
			os_spinlock_lock(&alltr->stage_fifo.qbase.sl);
			os_atomic32_set(&alltr->allocating, 0);
			goto retry;
		}
		os_atomic32_set(&alltr->allocating, 0);
	} else {
		os_spinlock_unlock(&alltr->stage_fifo.qbase.sl);
	}

	return SALSA_INVAL_GRAIN != grain ?
		scm_seg_to_grain(alltr->scm, seg) + grain - alltr->reserved_per_seg :
		grain;
}

/* force a new segment in */
__attribute__((warn_unused_result))
u32
allocator_drain_remaining_grains(struct allocator *const alltr)
{
	struct segment *p, *q;
	u32 rem = 0;
	os_spinlock_lock(&alltr->stage_fifo.qbase.sl);
	cds_list_for_each_entry_safe(p, q, &alltr->stage_fifo.qbase.lst, list) {
		// recycle
		rem = alltr->seg_size - os_atomic32_read(&p->nr3);
		DBG("unused %lu grains in seg=%p.", rem, p);
		segment_putn(p, rem); // they won't be invalidated by the user
		cds_queue_del_bare(&alltr->stage_fifo, &p->list);
		if (0 == os_atomic32_sub_return(&p->nr4, rem))
			cds_queue_enqueue_tail_wakeup(&alltr->done_fifo, &p->list);
		break;
	}
	os_spinlock_unlock(&alltr->stage_fifo.qbase.sl);
	return rem;
}

void
allocator_release_grains(struct allocator *const alltr, const u64 grain, const u64 len)
{
	struct segment *const seg = scm_grain_to_segment(alltr->scm, grain);
	assert(len <= os_atomic32_read(&seg->nr4));
	if (0 == os_atomic32_sub_return(&seg->nr4, len))
		cds_queue_enqueue_tail_wakeup(&alltr->done_fifo, &seg->list);
}

void
allocator_invalidate_grains(struct allocator *const alltr, const u64 grain, const u64 len, const bool is_reloc)
{
	struct segment *const seg = scm_grain_to_segment(alltr->scm, grain);
	if (!is_reloc)
		gc_page_invalidate(alltr->scm->gc, seg, len);
	else
		gc_page_invalidate_reloc(alltr->scm->gc, seg, len);
}

static os_thread_fun_return lsa_allocator_thread(void *arg);

int
allocator_init(
	struct allocator          **const alltr_out,
	struct sto_capacity_mgr    *const scm,
	void                       *const sc,
	const u64                         reserved_per_seg,
	const struct scm_gfs_flags *const gfs_flags_p,
	const u8                          ctlr_id,
	seg_md_callback_t                 fn)
{
	int err = 0;
	const struct scm_gfs_flags     gfs_flags = *gfs_flags_p;
	struct allocator *const alltr = malloc(sizeof(*alltr));
	if (NULL == alltr) {
		err = ENOMEM;
		ERR("failed to allocate alltr");
		goto  fail0;
	}
	memset(alltr, 0, sizeof(*alltr));
	alltr->scm = scm;
	alltr->sc = sc;
	*(u64 *)&alltr->seg_size = scm_get_seg_size(scm);
	*(u8 *)&alltr->ctlr_id   = ctlr_id;
	*(u8 *)&alltr->cache_size = 1; /* == ctlr_id ? 2 : 1; */
	*(u64 *)&alltr->reserved_per_seg = reserved_per_seg;
	alltr->seg_md_fn = fn;
	alltr->gfs_flags = gfs_flags;
	alltr->gfs_flags.no_blocking = 1;
	if (alltr->seg_size < reserved_per_seg) {
		err = EINVAL;
		goto fail1;
	}

	cds_queue_init(&alltr->done_fifo);
	cds_queue_init(&alltr->stage_fifo);
	os_atomic32_set(&alltr->exit, 0);
	os_atomic32_set(&alltr->allocating, 0);
	err = os_init_thread(alltr, lsa_allocator_thread, &alltr->alloc_th,
			&alltr->alloc_th_done, 0, "lsa-alloc");
	ERR_CHK_PRNT_GOTO(0 != err, fail1, "init threads failed");
	*alltr_out = alltr;
	DBG("Salsa allocator init success, flag blocking=%u.", gfs_flags.no_blocking);
	return err;
fail1:
	free(alltr);
fail0:
	return err;
}

int
allocator_exit(struct allocator *const alltr)
{
	os_atomic32_set(&alltr->exit, 1);
	cds_queue_wakeup_all(&alltr->done_fifo);
	cds_queue_wakeup_all(&alltr->stage_fifo);

	while (0 == os_atomic32_read(&alltr->alloc_th_done))
		os_msleep(1);
	free(alltr);
	return 0;
}

static os_thread_fun_return lsa_allocator_thread(void *arg)
{
	struct os_thread_arg *carg = arg;
	struct allocator *const alltr = carg->private;
	free(carg);
	carg = NULL;
	for (; 0 == os_atomic32_read(&alltr->exit);) {
#if	0
		/* allocate one seg */
		int rc = allocator_allocate_segment(alltr);
		if (0 == rc)
			cds_queue_wakeup_all(&alltr->stage_fifo);
		else {
			os_schedule(); /* avoid soft lockup */
			continue;
		}

		if (cds_queue_size(&alltr->stage_fifo) < alltr->cache_size)
			continue; /* keep at least cache_size segments ready */
#endif
		cds_queue_wait_event_interruptiple(&alltr->done_fifo,
			1 == os_atomic32_read(&alltr->exit) ||
			!cds_queue_is_empty(&alltr->done_fifo));
		struct segment *const seg = cds_queue_pop_head(&alltr->done_fifo, struct segment, list);
		if (NULL != seg) {
			/* perform end-of-destage operations */
			gc_seg_destaged(alltr->scm->gc, seg);
			DBG("id=%u destaged seg=%lu size=%lu", alltr->ctlr_id,
				scm_get_seg_idx(alltr->scm, seg), cds_queue_size(&alltr->stage_fifo));
		}
	}

	os_atomic32_set(&alltr->alloc_th_done, 1);
	return 0;
}
