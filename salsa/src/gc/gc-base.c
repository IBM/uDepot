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

#include "gc/gc.h"
#include "gc/gc-io-work.h"
#include "gc/private/gc-common.h"
#include "gc/private/gc-queue.h"

#include <asm-generic/errno.h>

#include "generic/compiler.h"
#include "libos/os-atomic.h"
#include "libos/os-debug.h"
#include "libos/os-lock.h"
#include "libos/os-malloc.h"
#include "libos/os-string.h"
#include "libos/os-thread.h"
#include "libos/os-time.h"
#include "sto-ctlr/scm-seg-alloc.h"
#include "sto-ctlr/sto-ctlr.h"

struct gc_base {
	struct gc        base;
	struct gc_queue  fifo;
};

static const struct gc_fns base_circular_fns;
static const struct gc_fns base_greedy_fns;

void gc_base_get_fns(
	const enum gc_type              type,
	const struct gc_fns **const fns_out)
{
	if (GC_GREEDY_WINDOW == type)
		*fns_out = &base_greedy_fns;
	else if (GC_CIRCULAR_BUFFER == type)
		*fns_out = &base_circular_fns;
}

static int gc_base_ctr(
	const struct gc_parameters *const params,
	struct gc                 **const gc_out)
{
	int err = 0;
	struct gc_base *gc = NULL;
	gc = os_malloc(sizeof(*gc));
	if (unlikely(NULL == gc)) {
		err = ENOMEM;
		goto fail0;
	}
	memset(gc, 0, sizeof(*gc));
	assert(0 == err);
	*gc_out = (struct gc *) gc;
	return 0;
fail0:
	return err;
}

static int gc_base_init(struct gc *const gc_in)
{
	struct gc_base *gc= (struct gc_base *) gc_in;
	gc_queue_init(&gc->fifo);
	return 0;
}

static void gc_base_init_stats(struct gc *const gc_in)
{
	struct gc_base *gc= (struct gc_base *) gc_in;
	gc_queue_init_stats(&gc->fifo);
}

static void gc_base_print_list_size(const struct gc *const gc_in)
{
	struct gc_base *gc= (struct gc_base *) gc_in;
	struct segment *p;
	int n = 0;
	os_spinlock_lock(&gc->fifo.sl);
	cds_list_for_each_entry(p, &gc->fifo.lst, list) {
		++n;
	}
	os_spinlock_unlock(&gc->fifo.sl);
	MSG("GC FIFO-SIZE=%d", n);
	MSG("GC FIFO-PAGE-RELOCS=%ld",
		os_atomic64_read(&gc->fifo.page_relocs));
	MSG("GC FIFO-SEG-RELOCS=%ld",
		os_atomic64_read(&gc->fifo.seg_relocs));
	MSG("GC FIFO-LEN=%ld",
		os_atomic64_read(&gc->fifo.lst_len));
	MSG("GC FIFO-THRESHOLD=%ld",
		os_atomic64_read(&gc->fifo.threshold));
}

static void gc_base_print_info(const struct gc *const gc_in)
{
	MSG("GC_TYPE                    =%02u_%s",
	    gc_in->gc_type, gc_string[gc_in->gc_type]);
}

static int gc_base_dtr (struct gc *gc_in)
{
	int err =0;
	struct gc_base *gc= (struct gc_base *) gc_in;
	if (unlikely(NULL == gc)) {
		err = EINVAL;
		goto out0;
	}
	os_free(gc);
out0:
	return err;
}

static u32 gc_base_get_bin_nr(const struct gc *const gc_in)
{
	return 1;
}

static void gc_base_seg_destaged(
	struct gc  *const gc_in,
	struct segment *const seg)
{
	struct gc_base *gc= (struct gc_base *) gc_in;
	os_spinlock_lock(&gc->fifo.sl);
	cds_list_add_tail(&seg->list, &gc->fifo.lst);
	os_spinlock_unlock(&gc->fifo.sl);
}

static void gc_base_page_invalidate(
	struct gc      *const gc_in,
	struct segment *const seg,
	const u32        grain_nr)
{
	assert(NULL != seg);
	segment_putn(seg, grain_nr);
}

static inline void gc_base_get_first_seg(
	struct gc_base *const gc,
	struct segment    **const seg)
{
	os_spinlock_lock(&gc->fifo.sl);
	if (likely(!cds_list_empty(&gc->fifo.lst))) {
		*seg = cds_list_first_entry(
			&gc->fifo.lst, struct segment, list);
		cds_list_del(&(*seg)->list);
	}
	os_spinlock_unlock(&gc->fifo.sl);
}

static inline void gc_base_add_seg_to_head(
	struct gc_base  *const gc,
	struct segment *const seg)
{
	os_spinlock_lock(&gc->fifo.sl);
	cds_list_add(&seg->list, &gc->fifo.lst);
	os_spinlock_unlock(&gc->fifo.sl);
}

static inline void gc_base_add_seg_to_tail(
	struct gc_base  *const gc,
	struct segment *const seg)
{
	os_spinlock_lock(&gc->fifo.sl);
	cds_list_add_tail(&seg->list, &gc->fifo.lst);
	os_spinlock_unlock(&gc->fifo.sl);
}

static int gc_base_sto_ctlr_pre_dtr(
	struct gc *const gc_in,
	const u8    ctlr_id)
{
	struct gc_base *const gc = (struct gc_base *) gc_in;
	struct segment *p, *q;
	CDS_LIST_HEAD(l);
	os_spinlock_lock(&gc->fifo.sl);
	cds_list_for_each_entry_safe(p, q, &gc->fifo.lst, list) {
		if (ctlr_id != p->ctlr_id)
			continue;
		cds_list_del(&p->list);
		cds_list_add_tail(&p->list, &l);
	}
	os_spinlock_unlock(&gc->fifo.sl);
	cds_list_for_each_entry_safe(p, q, &l, list) {
		cds_list_del(&p->list);
		os_atomic32_zero(&p->valid_nr);
		scm_put_free_seg(gc_in->scm, p, 0);
	}
	return 0;
}

static os_thread_fun_return
gc_circular_run(void *arg)
{
	struct os_thread_arg *gcarg = arg;
	struct gc_base *const gc = (struct gc_base *) gcarg->private;
	const u32 id = gcarg->id;
	struct sto_capacity_mgr *const scm = gc->base.scm;
	os_atomic64_t *const nfree_segs = scm_seg_queues_get_free_ref(&scm->seg_qs);
	os_atomic32_t *const exit = &gc->base.exit;
	const u64 LOW_WATERMARK = gc->base.LOW_WATERMARK;
	const u64 HIGH_WATERMARK = gc->base.HIGH_WATERMARK;
	struct gc_io_work_parent *const gciowp = &gc->base.gciowp[id];
	os_free(gcarg);
	gcarg = NULL;

	MSG("CIRCULAR GC with LOW_WATERMARK=%"PRIu64 \
		" HIGH_WATERMARK=%"PRIu64, LOW_WATERMARK, HIGH_WATERMARK);

	for (; 0 == os_atomic32_read(exit); ) {
		os_wait_event_interruptible(gc->base.low_watermark_waitq,
			os_atomic64_read(nfree_segs) <= LOW_WATERMARK ||
			1 == os_atomic32_read(exit));
		DBG("Starting garbage collection.");
		for (; 0 == os_atomic32_read(exit) &&
			     HIGH_WATERMARK > os_atomic64_read(nfree_segs); ) {
			/*
			 * Relocate segments in a strictly serial fashion
			 */
			struct segment *seg = NULL;
			int rc;
			u32 relocs;
			gc_reloc_fn_t fn = NULL;
			struct sto_ctlr *sc = NULL;
			gc_base_get_first_seg(gc, &seg);
			if (NULL == seg) {
				os_msleep(1);
				continue;
			}

			DBG("Chose candidate segment for reloc: idx=%"PRIu64,
				scm_get_seg_idx(scm, seg));

			gc_seg_selected(&gc->base, seg);
			gc_io_work_parent_init(gciowp, seg);
			fn = gc->base.ctl_data[seg->ctlr_id].sc_reloc_fn;
			sc = gc->base.ctl_data[seg->ctlr_id].sc;
			rc = (fn) (sc, gciowp);
			relocs = os_atomic32_read(&gciowp->cnt);
			gc_queue_add_page_relocs(&gc->fifo, relocs);
			if (unlikely(0 != rc)) {
				gc_base_add_seg_to_tail(gc, seg);
				/* gc_base_add_seg_to_head(gc, seg); */
				if (EAGAIN != rc && ENOSYS != rc)
					ERR("ctlr_relocate_segment " \
						" failed with %d.", rc);
				continue;
			}
			/* push it to the free list */
			scm_put_free_seg(scm, seg, 0);
			gc_seg_released(&gc->base, &gc->fifo, relocs);
		}
	}
	os_atomic32_set(&gc->base.gc_done[id], 1);
	return 0;
}

static os_thread_fun_return
gc_greedy_run(void *arg)
{
#define	LSA_MAX_GC_WINDOW	1024
	struct os_thread_arg *gcarg = arg;
	struct gc_base *const gc = (struct gc_base *) gcarg->private;
	const u32 id = gcarg->id;
	struct sto_capacity_mgr *const scm = gc->base.scm;
	os_atomic64_t *const nfree_segs = scm_seg_queues_get_free_ref(&scm->seg_qs);
	os_atomic32_t *const exit = &gc->base.exit;
	const u32 seg_size = gc->base.seg_size;
	const u64 LOW_WATERMARK = gc->base.LOW_WATERMARK;
	const u64 HIGH_WATERMARK = gc->base.HIGH_WATERMARK;
	struct gc_io_work_parent *const gciowp = &gc->base.gciowp[id];
	os_free(gcarg);
	gcarg = NULL;

	MSG("GREEDY WINDOW GC with LOW_WATERMARK=%"PRIu64 \
		" HIGH_WATERMARK=%"PRIu64, LOW_WATERMARK, HIGH_WATERMARK);

	for (; 0 == os_atomic32_read(exit); ) {
		os_wait_event_timeout(gc->base.low_watermark_waitq,
			os_atomic64_read(nfree_segs) <= LOW_WATERMARK ||
			1 == os_atomic32_read(exit), 1);
		DBG("Starting garbage collection.");
		for (; 0 == os_atomic32_read(exit) &&
			     HIGH_WATERMARK > os_atomic64_read(nfree_segs); ) {
			struct segment *seg = NULL;
			struct segment *best_seg = NULL;
			u32 i = 0;
			u32 min = seg_size + 1;
			int rc;
			u32 relocs = 0;
			gc_reloc_fn_t fn = NULL;
			struct sto_ctlr *sc = NULL;
			os_spinlock_lock(&gc->fifo.sl);
			cds_list_for_each_entry(seg, &gc->fifo.lst, list) {
				u32 cur = segment_valid_nr(seg);
				if (cur < min) {
					best_seg = seg;
					min = cur;
				}
				if (i++ == LSA_MAX_GC_WINDOW)
					break;
			}
			if (unlikely(NULL == best_seg)) {
				os_spinlock_unlock(&gc->fifo.sl);
				os_msleep(1);
				continue;
			}
			cds_list_del(&best_seg->list);
			os_spinlock_unlock(&gc->fifo.sl);
			DBG("Chose candidate segment for reloc: idx=%"PRIu64,
				scm_get_seg_idx(scm, best_seg));
			gc_seg_selected(&gc->base, best_seg);
			gc_io_work_parent_init(gciowp, best_seg);
			fn = gc->base.ctl_data[best_seg->ctlr_id].sc_reloc_fn;
			sc = gc->base.ctl_data[best_seg->ctlr_id].sc;
			rc = (fn) (sc, gciowp);
			relocs = os_atomic32_read(&gciowp->cnt);
			gc_queue_add_page_relocs(&gc->fifo, relocs);
			if (unlikely(0 != rc)) {
				gc_base_add_seg_to_head(gc, best_seg);
				if (EAGAIN != rc)
					ERR("lsa_ctlr_relocate_segment " \
						" failed with %d.", rc);
				continue;
			}
			/* push it to the free list */
			scm_put_free_seg(scm, best_seg, 0);
			gc_seg_released(&gc->base, &gc->fifo, relocs);
		}
	}
	os_atomic32_set(&gc->base.gc_done[id], 1);
	return 0;
}

static const struct gc_fns base_greedy_fns = {
	.ctr             = gc_base_ctr,
	.init            = gc_base_init,
	.init_stats      = gc_base_init_stats,
	.dtr             = gc_base_dtr,
	.run             = gc_greedy_run,
	.seg_write       = gc_base_seg_destaged,
	.seg_relocate    = gc_base_seg_destaged,
	.page_invalidate = gc_base_page_invalidate,
	.get_bin_nr       = gc_base_get_bin_nr,
	.print_list_size = gc_base_print_list_size,
	.print_info      = gc_base_print_info,
	.sto_ctlr_pre_dtr= gc_base_sto_ctlr_pre_dtr
};

static const struct gc_fns base_circular_fns = {
	.ctr             = gc_base_ctr,
	.init            = gc_base_init,
	.init_stats      = gc_base_init_stats,
	.dtr             = gc_base_dtr,
	.run             = gc_circular_run,
	.seg_write       = gc_base_seg_destaged,
	.seg_relocate    = gc_base_seg_destaged,
	.page_invalidate = gc_base_page_invalidate,
	.get_bin_nr       = gc_base_get_bin_nr,
	.print_list_size = gc_base_print_list_size,
	.print_info      = gc_base_print_info,
	.sto_ctlr_pre_dtr= gc_base_sto_ctlr_pre_dtr
};
