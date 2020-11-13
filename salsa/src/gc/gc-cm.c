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
#include "gc/private/gc-common.h"
#include "gc/private/gc-queue.h"

#include <asm-generic/errno.h>

#include "data-structures/hlist-hash.h"
#include "generic/compiler.h"
#include "gc/gc-io-work.h"
#include "gc/gc-parameters.h"
#include "libos/os-atomic.h"
#include "libos/os-debug.h"
#include "libos/os-lock.h"
#include "libos/os-malloc.h"
#include "libos/os-string.h"
#include "libos/os-thread.h"
#include "libos/os-time.h"
#include "sto-ctlr/sto-ctlr.h"
#include "sto-ctlr/scm-seg-alloc.h"
#include "sto-ctlr/private/sto-segment.h"

struct gc_cm {
	struct gc          base;
	struct hlist_hash *h;
	const u32     stream_nr;
	struct gc_queue    fifo[];
};

static const struct gc_fns cm_fns;

void gc_cm_get_fns(
	const enum gc_type              type,
	const struct gc_fns **const fns_out)
{
	if (GC_CONTAINER_MARKER == type)
		*fns_out = &cm_fns;
}

static int gc_cm_ctr(
	const struct gc_parameters *const params,
	struct gc                 **const gc_out)
{
	int err = 0;
	struct sto_capacity_mgr *scm = params->scm;
	struct gc_cm *gc = NULL;
	u32 stream_nr = params->bin_nr;
	const u64 seg_nr = scm_get_seg_nr(scm);
	const u64 nhbuckets = seg_nr / 8 < 16384 && 1 < (seg_nr / 8) ?
		seg_nr / 8 : 16384;
	gc = os_malloc(sizeof(*gc) + sizeof(*gc->fifo) * (stream_nr + 1));
	if (unlikely(NULL == gc)) {
		err = ENOMEM;
		goto fail1;
	}
	memset(gc, 0, sizeof(*gc) + sizeof(*gc->fifo) * (stream_nr + 1));
	*((u32 *)&gc->stream_nr) = stream_nr;

	err = hlist_hash_init(nhbuckets, false, &gc->h);
	if (unlikely(0 != err)) {
		ERR("hlist_hash_init failed with %d, err.", err);
		goto fail2;
	}
	assert(0 == err);
	*gc_out = (struct gc *) gc;
	return 0;

fail2:
	os_free(gc);
fail1:
	return err;
}

static int gc_cm_init(struct gc *const gc_in)
{
	struct gc_cm *gc = (struct gc_cm *) gc_in;
	int i;
	for (i = 0; i <= gc->stream_nr; ++i)
		gc_queue_init(&gc->fifo[i]);
	return 0;
}

static int gc_cm_sto_ctlr_pre_dtr(
	struct gc *const gc_in,
	const u8    ctlr_id)
{
	struct gc_cm *const gc = (struct gc_cm *) gc_in;
	int i;
	struct segment *p, *q;
	for (i = 0; i <= gc->stream_nr; ++i) {
		CDS_LIST_HEAD(l);
		os_spinlock_lock(&gc->fifo[i].sl);
		cds_list_for_each_entry_safe(p, q, &gc->fifo[i].lst, list) {
			if (ctlr_id != p->ctlr_id)
				continue;
			cds_list_del(&p->list);
			cds_list_add_tail(&p->list, &l);
		}
		os_spinlock_unlock(&gc->fifo[i].sl);
		cds_list_for_each_entry_safe(p, q, &l, list) {
			const int rc __attribute__((unused)) = hlist_hash_delete(gc->h, &p->hlist);
			assert(0 == rc);
			cds_list_del(&p->list);
			os_atomic32_zero(&p->valid_nr);
			scm_put_free_seg(gc_in->scm, p, 0);
		}
	}
	return 0;
}

static void gc_cm_init_stats(struct gc *const gc_in)
{
	struct gc_cm *gc = (struct gc_cm *) gc_in;
	int i;
	for (i = 0; i <= gc->stream_nr; ++i)
		gc_queue_init_stats(&gc->fifo[i]);
}

static void gc_cm_print_list_size(const struct gc *gc_in)
{
	struct gc_cm *gc = (struct gc_cm *) gc_in;
	int i;
	struct segment *p;
	for (i = 0; i <= gc->stream_nr; ++i) {
		int n = 0;
		os_spinlock_lock(&gc->fifo[i].sl);
		cds_list_for_each_entry(p, &gc->fifo[i].lst, list) {
			++n;
		}
		os_spinlock_unlock(&gc->fifo[i].sl);
		MSG("GC FIFO-%d-SIZE=%d", i, n);
	}
	for (i = 0; i <= gc->stream_nr; ++i) {
		MSG("GC FIFO-%d-PAGE-RELOCS=%ld",
			i, os_atomic64_read(&gc->fifo[i].page_relocs));
		MSG("GC FIFO-%d-SEG-RELOCS=%ld",
			i, os_atomic64_read(&gc->fifo[i].seg_relocs));
		MSG("GC FIFO-%d-LEN=%ld",
			i, os_atomic64_read(&gc->fifo[i].lst_len));
		MSG("GC FIFO-%d-THRESHOLD=%ld",
			i, os_atomic64_read(&gc->fifo[i].threshold));
	}
}

static void gc_cm_print_info(const struct gc *const gc_in)
{
	MSG("GC_TYPE                    =%02u_%s",
	    gc_in->gc_type, gc_string[gc_in->gc_type]);
	MSG("CONTAINERS                 =%u", gc_in->fns->get_stream_nr(gc_in));
}

static int gc_cm_dtr(struct gc *gc_in)
{
	int err =0;
	struct gc_cm *gc = (struct gc_cm *) gc_in;
	if (unlikely(NULL == gc)) {
		err = EINVAL;
		goto out0;
	}
	hlist_hash_exit(gc->h);
	os_free(gc);
out0:
	return err;
}

static u32 gc_cm_get_stream_nr(const struct gc *const gc_in)
{
	struct gc_cm *gc = (struct gc_cm *) gc_in;
	return gc->stream_nr;
}

static u32 gc_cm_get_bin_nr(const struct gc *const gc_in)
{
	return 1;
}

static inline u32 gc_cm_stream_to_container(
	const struct gc_cm   *const gc,
	const struct segment *const seg)
{
	const u32 stream_nr = segment_get_stream(seg);
	return stream_nr < gc->stream_nr ? stream_nr : gc->stream_nr - 1;
}

static void gc_cm_seg_destaged(
	struct gc  *const gc_in,
	struct segment *const seg)
{
	struct gc_cm *const gc = (struct gc_cm *) gc_in;
	int rc;
	u32 lvl = gc_cm_stream_to_container(gc, seg);
	u32 valid_nr = segment_valid_nr(seg);
	if (valid_nr == gc_in->seg_size) {
		lvl = gc->stream_nr; /* age queue last one */
	} else {
		assert(lvl < gc->stream_nr);
	}
	os_spinlock_lock(&gc->fifo[lvl].sl);
	cds_list_add_tail(&seg->list, &gc->fifo[lvl].lst);
	os_spinlock_unlock(&gc->fifo[lvl].sl);
	segment_set_private(seg, lvl);
	rc = hlist_hash_insert(gc->h, &seg->hlist);
	assert(0 == rc);
	DBG("inserted seg%p in queue %d", seg, lvl);
}

static void gc_cm_page_invalidate(
	struct gc  *const gc_in,
	struct segment *const seg,
	const u32        grain_nr)
{
	struct gc_cm *const gc = (struct gc_cm *) gc_in;
	int rc;
	u32 lvl = gc_cm_stream_to_container(gc, seg);
	segment_putn(seg, grain_nr);

	hlist_hash_bucket_lock(gc->h, &seg->hlist);
	rc = hlist_hash_bare_lookup(gc->h, &seg->hlist);
	if (0 != rc) {
		/* not in the GC queues */
		hlist_hash_bucket_unlock(gc->h, &seg->hlist);
		return;
	}
	if (segment_get_private(seg) == gc->stream_nr) {
		/* move it out of the all valid queue */
		lvl = segment_get_private(seg);
		os_spinlock_lock(&gc->fifo[lvl].sl);
		cds_list_del(&seg->list);
		os_spinlock_unlock(&gc->fifo[lvl].sl);
		lvl = gc_cm_stream_to_container(gc, seg);
		assert(lvl < gc->stream_nr);
		os_spinlock_lock(&gc->fifo[lvl].sl);
		cds_list_add_tail(&seg->list, &gc->fifo[lvl].lst);
		os_spinlock_unlock(&gc->fifo[lvl].sl);
		segment_set_private(seg, lvl);
	}
	hlist_hash_bucket_unlock(gc->h, &seg->hlist);
	DBG("inserted seg%p in queue %d", seg, lvl);
}

static inline  int gc_cm_relocate_seg(
	struct gc_cm             *const gc,
	struct gc_io_work_parent *const gciowp,
	struct segment           *const seg,
	const u32                  stream,
	os_atomic32_t            *const exit)
{
	int rc, retry_nr = 0;
	struct sto_ctlr *const sc = gc->base.ctl_data[seg->ctlr_id].sc;
	const gc_reloc_fn_t fn = gc->base.ctl_data[seg->ctlr_id].sc_reloc_fn;
	assert(NULL != sc && NULL != fn);
	do {
		DBG("Reloc seg=%p stream=%u ctlr-id=%u", seg, stream, seg->ctlr_id);
		gc_io_work_parent_init(gciowp, seg);
		rc = (fn) (sc, gciowp);
		gc_queue_add_page_relocs(
			&gc->fifo[stream], os_atomic32_read(&gciowp->cnt));
		if (unlikely(1000 <= ++retry_nr)) {
			ERR("stuck in reloc loop seg=%p sc-id=%u valid-nr=%u",
				seg, seg->ctlr_id, segment_valid_nr(seg));
			break;
		}
	} while (EAGAIN == rc && 0 == os_atomic32_read(exit));
	return rc;
}

static os_thread_fun_return
gc_cm_run(void *arg)
{
#define	GC_CM_WINDOW	2048
	struct os_thread_arg *gcarg = arg;
	struct gc_cm *const gc = (struct gc_cm *) gcarg->private;
	const u32 id = gcarg->id;
	struct sto_capacity_mgr *const scm = gc->base.scm;
	os_atomic64_t *const nfree_segs = scm_seg_queues_get_free_ref(&scm->seg_qs);
	const u32 stream_nr = gc->base.fns->get_stream_nr(&gc->base);
	const u32 seg_size = gc->base.seg_size;
	os_atomic32_t *const exit = &gc->base.exit;
	const u64 LOW_WATERMARK = gc->base.LOW_WATERMARK;
	const u64 HIGH_WATERMARK = gc->base.HIGH_WATERMARK;
	struct gc_io_work_parent *const gciowp = &gc->base.gciowp[id];
	os_free(gcarg);
	gcarg = NULL;

	MSG("%s with LOW_WATERMARK=%"PRIu64" HIGH_WATERMARK=%"PRIu64" streams=%u" \
	    " id=%u", gc_string[gc->base.gc_type], LOW_WATERMARK,
	    HIGH_WATERMARK, stream_nr, id);

	for (; 0 == os_atomic32_read(exit); ) {
		os_wait_event_timeout(gc->base.low_watermark_waitq,
				os_atomic64_read(nfree_segs) < LOW_WATERMARK ||
				1 == os_atomic32_read(exit),
				GC_WAIT_WATERMARK_TIMEOUT);
		for (; 0 == os_atomic32_read(exit) &&
			     os_atomic64_read(nfree_segs) < HIGH_WATERMARK; ) {
 			struct segment *best_seg = NULL;
			u32 min = seg_size;
			u32 lvl;
			int i, rc, best_lvl = 0;
			for (i = stream_nr - 1; i >= 0; --i) {
				struct segment *p;
				int iter = 0;
				os_spinlock_lock(&gc->fifo[i].sl);
				cds_list_for_each_entry(p, &gc->fifo[i].lst, list) {
					u32 cur = segment_valid_nr(p);
					assert(seg_size >= cur);
					if (min > cur) {
						min = cur;
						best_seg = p;
						best_lvl = i;
					}
					if (GC_CM_WINDOW / stream_nr == ++iter)
						break;
				}
				os_spinlock_unlock(&gc->fifo[i].sl);
			}
			if (unlikely(NULL == best_seg)) {
				os_msleep(1);
				continue; /* queues are empty */
			}
			/* try to extract */
			rc = hlist_hash_delete(gc->h, &best_seg->hlist);
			if (unlikely(ENODATA == rc)) {
				os_schedule();
				continue;
			}

			lvl = gc_cm_stream_to_container(gc, best_seg);
			os_spinlock_lock(&gc->fifo[lvl].sl);
			cds_list_del(&best_seg->list);
			os_spinlock_unlock(&gc->fifo[lvl].sl);

			gc_seg_selected(&gc->base, best_seg);
			rc = gc_cm_relocate_seg(gc, gciowp, best_seg, best_lvl, exit);
			if (unlikely(0 != rc)) {
				gc_cm_seg_destaged(&gc->base, best_seg);
				DBG("lsa_ctlr_relocate_segment rc=%d.", rc);
				continue;
			}
			/* push it to the free list */
			scm_put_free_seg(scm, best_seg, 0);
			gc_seg_released(&gc->base, &gc->fifo[best_lvl],
					os_atomic32_read(&gciowp->cnt));
		}
	}
	os_atomic32_set(&gc->base.gc_done[id], 1);
	return 0;

}

static const struct gc_fns cm_fns = {
	.ctr             = gc_cm_ctr,
	.init            = gc_cm_init,
	.init_stats      = gc_cm_init_stats,
	.dtr             = gc_cm_dtr,
	.run             = gc_cm_run,
	.seg_write       = gc_cm_seg_destaged,
	.seg_relocate    = gc_cm_seg_destaged,
	.page_invalidate = gc_cm_page_invalidate,
	.get_stream_nr   = gc_cm_get_stream_nr,
	.get_bin_nr      = gc_cm_get_bin_nr,
	.print_list_size = gc_cm_print_list_size,
	.print_info      = gc_cm_print_info,
	.sto_ctlr_pre_dtr= gc_cm_sto_ctlr_pre_dtr,
};
