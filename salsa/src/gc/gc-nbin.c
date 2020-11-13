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

enum {
	GC_SALSA_LOW_QUEUE  = 0,
	GC_SALSA_MED_QUEUE  = 1,
	GC_SALSA_HIGH_QUEUE = 2,
	GC_SALSA_QUEUE_LAST = 3
};

// high/low watermark for active pages per segment
/* #define GC_CHECK_ACTIVE */
const int GC_ACTIVE_HIGH_WATERMARK = 10;
const int GC_ACTIVE_LOW_WATERMARK  = 7;

#define XNP_BIN_ID		(SALSA_MAX_PRIVATE_ID - 2)
#define	XNP_CIRC_BUFF_MAX_LEN	16384
struct xnp_queue;
struct gc_salsa {
	struct gc          base __attribute__((aligned(CACHE_LINE_SIZE)));
	const u32          bin_nr;
	const u32          bin_div;
	const u32          stream_nr;
	/* XNP specific */
	const u32          xnp_levels;
	const u32          xnp_pct[2];
	struct gc_queue    xnp_wait[2];
	const u32          xnp_thr[2];
	os_atomic32_t      xnp_wait_nr[2];
	/* XNP_PATENT specific */
	struct xnp_queue  *xnp_circ_buf;
	const u32          xnp_circ_buf_len;
	const u64          xnp_max_capacity;
	os_atomic64_t      xnp_tot_capacity;
	os_atomic32_t      xnp_current;
	os_atomic32_t      zero_valid_nr;
	os_atomic32_t      cherry_picked_nr;
	os_rwlock_t        xnp_current_rwlock;
	/* interrupt safe structures */
	unsigned long     *seg_bmap; /* 1bit per segment in the system */
	/* fifos for all */
	struct gc_queue    fifo[];
} __attribute__((aligned(CACHE_LINE_SIZE)));

static const struct gc_fns salsa_fns;
static const struct gc_fns salsa_n_fns;
static const struct gc_fns salsa_n_xnp_hs_fns;
static const struct gc_fns salsa_n_xnp_intrpt_safe_fns;
static const struct gc_fns salsa_n_xnp_ovrw_hs_fns;
static const struct gc_fns salsa_n_xnp_ovrw_hs_patent_fns;
static const struct gc_fns salsa_n_hs_fns;
static const struct gc_fns salsa_n_dyn_thr_fns;

struct xnp_queue {
	struct cds_list_head l;
	os_spinlock_t        sl;
} __attribute__((aligned(CACHE_LINE_SIZE)));

void gc_salsa_get_fns(
	const enum gc_type          type,
	const struct gc_fns **const fns_out)
{
	if (GC_SALSA_NBIN == type)
		*fns_out = &salsa_n_fns;
	else if (GC_SALSA_3BIN == type)
		*fns_out = &salsa_fns;
	else if (GC_NBIN_HEAT_SEG == type)
		*fns_out = &salsa_n_hs_fns;
	else if (GC_NBIN_DYN_THRESHOLD == type)
		*fns_out = &salsa_n_dyn_thr_fns;
	else if (GC_NBIN_XNP_HEAT_SEG == type)
		*fns_out = &salsa_n_xnp_hs_fns;
	else if (GC_NBIN_XNP_OVRW_HEAT_SEG == type)
		*fns_out = &salsa_n_xnp_ovrw_hs_fns;
	else if (GC_NBIN_XNP_PATENT == type)
		*fns_out = &salsa_n_xnp_ovrw_hs_patent_fns;
	else if (GC_NBIN_XNP_INTRPT_SAFE == type)
		*fns_out = &salsa_n_xnp_intrpt_safe_fns;
	else
		assert(0);
	assert(*fns_out != &salsa_n_xnp_ovrw_hs_patent_fns);
	assert((*fns_out)->page_invalidate != salsa_n_xnp_ovrw_hs_patent_fns.page_invalidate);
}

static inline u32 gc_salsa_bin_div(
	const u32 seg_size,
	const u32 bin_nr)
{
	u32 bin_div;
	assert(bin_nr <= seg_size);

	bin_div = seg_size / bin_nr;
	if (0U != (seg_size % bin_nr))
		bin_div++;

	return bin_div;
}

static int gc_salsa_ctr(
	const struct gc_parameters *const params,
	struct gc                 **const gc_out)
{
	int err = 0;
	struct sto_capacity_mgr *scm = params->scm;
	const u32 seg_size = scm_get_seg_size(scm);
	const u64 seg_nr = scm_get_seg_nr(scm);
	struct gc_salsa *gc = NULL;
	u32 bin_nr = params->bin_nr;
	u32 max_bin_nr = params->bin_nr + 2;
	const u32 stream_nr = 1;
	assert(bin_nr != 0);
	if (seg_size < bin_nr || SALSA_MAX_PRIVATE_ID - 1 < bin_nr)
		bin_nr = min2(seg_size, SALSA_MAX_PRIVATE_ID - 2);

	gc = os_malloc(sizeof(*gc) + sizeof(*gc->fifo) * max_bin_nr);
	ERR_CHK_SET_PRNT_GOTO(NULL == gc, err, ENOMEM, fail0,
			"gc malloc failed size=%luKiB", sizeof(*gc) + sizeof(*gc->fifo) * max_bin_nr);

	memset(gc, 0, sizeof(*gc) + sizeof(*gc->fifo) * max_bin_nr);

	switch(params->gc) {
	case GC_SALSA_3BIN:
		*((u32 *)&gc->bin_nr) = 3;
		*((u32 *)&gc->bin_div) = 1;
		break;
	case GC_NBIN_XNP_INTRPT_SAFE:
		gc->seg_bmap = os_malloc((seg_nr / BITS_PER_LONG + 1) * sizeof(long));
		ERR_CHK_SET_PRNT_GOTO(NULL == gc->seg_bmap, err, ENOMEM, fail1,
				"seg_bmap alloc failed.");
		memset(gc->seg_bmap, 0, ((seg_nr / BITS_PER_LONG + 1) * sizeof(long)));
		/* falls through */
	case GC_NBIN_XNP_HEAT_SEG:
	case GC_NBIN_XNP_OVRW_HEAT_SEG:
	case GC_NBIN_DYN_THRESHOLD:
	{
		int i;
		u64 xnp_pct = params->xnp_pct < 30 ?
			params->xnp_pct : 30;
		u64 xnp_hot_pct = params->xnp_hot_pct < 30 ?
			params->xnp_hot_pct : 30;
		*((u32 *)&gc->stream_nr) = stream_nr;
		*((u32 *)&gc->bin_nr) = bin_nr + 1;
		*((u32 *)&gc->bin_div) = gc_salsa_bin_div(seg_size, bin_nr);
		*((u32 *)&gc->xnp_pct[0]) = xnp_pct;
		*((u32 *)&gc->xnp_pct[1]) = xnp_hot_pct;
		*((u32 *)&gc->xnp_levels) = 0 == xnp_hot_pct ? 1 : 2;

		for (i = 0; i < gc->xnp_levels; ++i) {
			const u32 thr = ((seg_nr - SALSA_MIN_REL_SEGMENTS -
				gc->stream_nr * 2) * gc->xnp_pct[i] / 100U );
			gc_queue_init(&gc->xnp_wait[i]);
			os_atomic32_zero(&gc->xnp_wait_nr[i]);
			/* 2 is the minimum size of the wait queues */
			*(u32 *)&gc->xnp_thr[i] = 1 < thr ? thr : 2;
			MSG("wait queue %i: thr=%u", i, gc->xnp_thr[i]);
		}
		break;
	}
	case GC_NBIN_XNP_PATENT:
	{
		int i;
		const u64 gc_avail_segs =
			seg_nr - params->high_watermark -
			gc->stream_nr * 2;
		u64 xnp_pct = 4 <= params->xnp_pct ?
			params->xnp_pct : 4;
		u64 xnp_capacity_thr;
		do {
			xnp_capacity_thr = gc_avail_segs / xnp_pct;
			xnp_pct--;
		} while (1 >= xnp_capacity_thr && 0 != xnp_pct);
		/* length of the xnp circular buffer */
		*((u32 *)&gc->xnp_circ_buf_len) =
			XNP_CIRC_BUFF_MAX_LEN > xnp_capacity_thr ?
			xnp_capacity_thr : XNP_CIRC_BUFF_MAX_LEN;
		*((u64 *)&gc->xnp_max_capacity) = gc_avail_segs / 4;
		*((u32 *)&gc->stream_nr) = stream_nr;
		*((u32 *)&gc->xnp_pct) = xnp_pct;
		*((u32 *)&gc->bin_nr) = bin_nr + 1;
		*((u32 *)&gc->bin_div) = gc_salsa_bin_div(seg_size, bin_nr);
		os_atomic64_zero(&gc->xnp_tot_capacity);
		os_atomic32_zero(&gc->xnp_current);
		os_rwlock_init(&gc->xnp_current_rwlock);
		gc->xnp_circ_buf = os_malloc(
			sizeof(*gc->xnp_circ_buf) * gc->xnp_circ_buf_len);
		ERR_CHK_SET_PRNT_GOTO((NULL == gc->xnp_circ_buf), err, ENOMEM,
				fail1, "xnp_circ_buf malloc failed");

		for (i = 0; i < gc->xnp_circ_buf_len; ++i) {
			CDS_INIT_LIST_HEAD(&gc->xnp_circ_buf[i].l);
			os_spinlock_init(&gc->xnp_circ_buf[i].sl);
		}
		MSG("GC_NBIN_XNP_PATENT GC initialized");
		MSG("XNP CIRCULAR BUFFER LEN = %d TOT_SEGS = %"PRIu64,
			gc->xnp_circ_buf_len, seg_nr);
		MSG("XNP MAX CAPACITY = %"PRIu64 " TOT_GC_SEGS = %"PRIu64,
			gc->xnp_max_capacity, gc_avail_segs);
		break;
	}
	case GC_NBIN_HEAT_SEG:
		assert(is_power_of_two(stream_nr));
		*((u32 *)&gc->stream_nr) = stream_nr;
		*((u32 *)&gc->bin_nr) = bin_nr + 1;
		*((u32 *)&gc->bin_div) = gc_salsa_bin_div(seg_size, bin_nr);
		break;
	default:
		*((u32 *)&gc->bin_nr) = bin_nr + 1;
		*((u32 *)&gc->bin_div) = gc_salsa_bin_div(seg_size, bin_nr);
		break;
	}

	assert(0 != gc->bin_div);

	assert(0 == err);
	*gc_out = (struct gc *) gc;
	return 0;
fail1:
	os_free(gc);
fail0:
	return err;
}

static int gc_salsa_init(struct gc *const gc_in)
{
	int err = 0;
	struct gc_salsa *gc = (struct gc_salsa *) gc_in;
	const u32 bin_nr = gc->bin_nr;
	int i;
	for (i = 0; i < bin_nr - 1; ++i) {
		gc_queue_init(&gc->fifo[i]);
		os_atomic64_set(&gc->fifo[i].threshold,
				((i + 1) * gc->bin_div) - 1);
	}
	gc_queue_init(&gc->fifo[bin_nr - 1]);
	os_atomic64_set(&gc->fifo[bin_nr - 1].threshold, gc->base.seg_size);
	if (os_atomic64_read(&gc->fifo[bin_nr - 1].threshold) <=
		os_atomic64_read(&gc->fifo[bin_nr - 2].threshold)) {
		os_atomic64_set(&gc->fifo[bin_nr - 2].threshold, gc->base.seg_size - 1U);
	}
	return err;
}

static void gc_salsa_init_stats(struct gc *const gc_in)
{
	struct gc_salsa *gc = (struct gc_salsa *) gc_in;
	int i;
	for (i = 0; i < gc->bin_nr; ++i)
		gc_queue_init_stats(&gc->fifo[i]);
}

static void gc_salsa_print_list_size(const struct gc *const gc_in)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	int i;
	struct segment *p;
	for (i = 0; i < gc->bin_nr; ++i) {
		int n = 0;
		os_spinlock_lock(&gc->fifo[i].sl);
		cds_list_for_each_entry(p, &gc->fifo[i].lst, list) {
			++n;
		}
		os_spinlock_unlock(&gc->fifo[i].sl);
		MSG("GC FIFO-%d-SIZE=%d", i, n);
	}
	for (i = 0; i < gc->bin_nr; ++i) {
		MSG("GC FIFO-%d-PAGE-RELOCS=%ld",
			i, os_atomic64_read(&gc->fifo[i].page_relocs));
		MSG("GC FIFO-%d-SEG-RELOCS=%ld",
			i, os_atomic64_read(&gc->fifo[i].seg_relocs));
		MSG("GC FIFO-%d-LEN=%ld",
			i, os_atomic64_read(&gc->fifo[i].lst_len));
		MSG("GC FIFO-%d-THRESHOLD=%ld",
			i, os_atomic64_read(&gc->fifo[i].threshold));
	}
	MSG("GC freed with zero valid=%d", os_atomic32_read(&gc->zero_valid_nr));
}

static void gc_salsa_n_xnp_print_list_size(const struct gc *const gc_in)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	int i;
	struct segment *p;
	gc_salsa_print_list_size(gc_in);
	for (i = 0; i < gc->xnp_levels; ++i) {
		int n = 0;
		os_spinlock_lock(&gc->xnp_wait[i].sl);
		cds_list_for_each_entry(p, &gc->xnp_wait[i].lst, list) {
			++n;
		}
		os_spinlock_unlock(&gc->xnp_wait[i].sl);
		MSG("XNP FIFO-%d-SIZE=%d", i, n);
	}
	MSG("GC XNP cherry picked=%d", os_atomic32_read(&gc->cherry_picked_nr));
}

static void gc_salsa_n_xnp_pat_print_list_size(const struct gc *const gc_in)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	int i;
	struct segment *p;
	int tot = 0;
	gc_salsa_print_list_size(gc_in);
	for (i = 0; i < gc->xnp_circ_buf_len; ++i) {
		int n = 0;
		os_spinlock_lock(&gc->xnp_circ_buf[i].sl);
		cds_list_for_each_entry(p, &gc->xnp_circ_buf[i].l, list) {
			++n;
		}
		os_spinlock_unlock(&gc->xnp_circ_buf[i].sl);
		tot += n;
		MSG("XNP FIFO-%d-SIZE=%d", i, tot);
	}
}

static void gc_nbin_print_info(const struct gc *const gc_in)
{
	const u32 bin_nr __attribute__((unused)) = gc_get_bin_nr(gc_in);
	MSG("GC_TYPE                    =%02u_%s",
	    gc_in->gc_type, gc_string[gc_in->gc_type]);
	MSG("LOW-WATERMARK              =%"PRIu64,
		gc_in->LOW_WATERMARK);
	MSG("HIGH-WATERMARK             =%"PRIu64,
		gc_in->HIGH_WATERMARK);
	MSG("BIN-NR                     =%u", bin_nr);
	MSG("FREE-SEG-NR                =%"PRIu64,
		scm_seg_queues_free_total(&gc_in->scm->seg_qs));
	MSG("FREE-REL-SEG-NR            =%"PRIu64,
		scm_seg_queues_reloc_free_total(&gc_in->scm->seg_qs));
}

static void gc_nbin_hs_print_info(const struct gc *const gc_in)
{
	gc_nbin_print_info(gc_in);
}

static void gc_nbin_xnp_print_info(const struct gc *const gc_in)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	int i;
	gc_nbin_hs_print_info(gc_in);
	for (i = 0; i < gc->xnp_levels; ++i)
		MSG("XNP-PERCENT[%d]             =%u%%", i, gc->xnp_pct[i]);
}

static int gc_salsa_dtr (struct gc *gc_in)
{
	int err =0;
	unsigned i;
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	ERR_CHK_SET_GOTO(NULL == gc, err, EINVAL, out0);

	for (i=0; i<gc->bin_nr; i++)
		DBG("gc bin %u: %ld entries",
			i, os_atomic64_read(&gc->fifo[i].lst_len));
	if (NULL != gc->xnp_circ_buf)
		os_free(gc->xnp_circ_buf);
	if (NULL != gc->seg_bmap)
		os_free(gc->seg_bmap);
	os_free(gc);
out0:
	return err;
}

static u32 gc_get_stream_nr_dummy(const struct gc *const gc_in)
{
	return 1;
}

static u32 gc_hs_get_stream_nr(const struct gc *const gc_in)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	return gc->stream_nr;
}

static u32 gc_salsa_get_bin_nr(const struct gc *const gc_in)
{
	return 3;
}

static u32 gc_salsa_n_get_bin_nr(const struct gc *const gc_in)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	return gc->bin_nr - 1;
}

static inline int gc_salsa_n_segment_lookup(
	       struct gc_salsa  *const gc,
	       struct segment *const seg)
{
	const int rc = SEG_GC == segment_get_state(seg) ? 0 : ENODATA;
	return rc;
}

static inline int gc_salsa_n_segment_lock_and_lookup(
	struct gc_salsa  *const gc,
	struct segment *const seg)
{
	os_spinlock_lock(&seg->sl);
	return gc_salsa_n_segment_lookup(gc, seg);
}

static inline void gc_salsa_n_segment_unlock(
	struct gc_salsa *const gc,
	struct segment    *const seg)
{
	os_spinlock_unlock(&seg->sl);
}

static inline void gc_salsa_n_segment_locks_deactivate(
	struct gc_salsa *const gc,
	struct segment    *const seg)
{
	assert(SEG_GC == segment_get_state(seg));
	segment_set_state(seg, SEG_INVALID);
	gc_salsa_n_segment_unlock(gc, seg);
}

static inline void gc_salsa_n_segment_locks_activate(
	struct gc_salsa *const gc,
	struct segment    *const seg)
{
	os_spinlock_lock(&seg->sl);
	assert(SEG_GC != segment_get_state(seg));
	segment_set_state(seg, SEG_GC);
	os_spinlock_unlock(&seg->sl);
}

static inline void gc_salsa_n_segment_rm_from_bin(
	struct gc_salsa  *const gc,
	struct segment *const seg,
	const u32        bin)
{
	assert(bin < gc->bin_nr);
	gc_queue_dequeue(&gc->fifo[bin], &seg->list);
}

static inline void gc_salsa_n_segment_add_to_bin(
	struct gc_salsa *const gc,
	struct segment    *const seg,
	const u32           bin)
{
	assert(bin < gc->bin_nr);
	gc_queue_enqueue(&gc->fifo[bin], &seg->list);
	segment_set_private(seg, bin);
	DBG("inserted seg%p into queue %u.", seg, bin);
}

static inline void gc_salsa_n_segment_add_to_bin_head(
	struct gc_salsa  *const gc,
	struct segment *const seg,
	const u32        bin)
{
	assert(bin < gc->bin_nr);
	gc_queue_enqueue_head(&gc->fifo[bin], &seg->list);
	segment_set_private(seg, bin);
	DBG("inserted seg%p into queue %u.", seg, bin);
}

static void gc_recycle_seg_on_zero_valid_pages(
	struct gc_salsa  *const gc,
	struct segment *const seg,
	const u32             bin)
{
	scm_put_free_seg(gc->base.scm, seg, 0);
	gc_seg_released(&gc->base, &gc->fifo[bin], 0);
	os_atomic32_inc(&gc->zero_valid_nr);
}

/* Called with segment lock held */
static void gc_free_seg_on_zero_valid_pages(
	struct gc_salsa  *const gc,
	struct segment *const seg)
{
	u32 bin;
	assert(SEG_GC == segment_get_state(seg));
	segment_set_state(seg, SEG_INVALID);
	bin = segment_get_private(seg);
	gc_salsa_n_segment_unlock(gc, seg);
	gc_salsa_n_segment_rm_from_bin(gc, seg, bin);
	BUG_ON(0 != segment_valid_nr(seg));
	gc_recycle_seg_on_zero_valid_pages(gc, seg, bin);
}

static inline struct segment *gc_xnp_popone(
		struct gc_salsa *const gc,
		const u32       xnp_lvl)
{
	struct segment *seg_pop = NULL;
	u32 xnp_lvl_len;
 	os_spinlock_lock(&gc->xnp_wait[xnp_lvl].sl);
	xnp_lvl_len = os_atomic32_read(&gc->xnp_wait_nr[xnp_lvl]);
	if (xnp_lvl_len >= gc->xnp_thr[xnp_lvl]) {
		assert(!cds_list_empty(&gc->xnp_wait[xnp_lvl].lst));
		seg_pop = cds_list_first_entry(&gc->xnp_wait[xnp_lvl].lst,
					       struct segment, list);
		cds_list_del(&seg_pop->list);
		assert(0 < os_atomic32_read(&gc->xnp_wait_nr[xnp_lvl]));
		assert(XNP_BIN_ID == segment_get_private(seg_pop));
		assert(SEG_GC_XNP == segment_get_state(seg_pop));
		os_atomic32_dec(&gc->xnp_wait_nr[xnp_lvl]);
	}
	os_spinlock_unlock(&gc->xnp_wait[xnp_lvl].sl);
	return seg_pop;
}

static inline struct segment *gc_xnp_popone_force(
		struct gc_salsa *const gc,
		const u32       xnp_lvl)
{
	struct segment *seg_pop = NULL;
 	os_spinlock_lock(&gc->xnp_wait[xnp_lvl].sl);
	if (!cds_list_empty(&gc->xnp_wait[xnp_lvl].lst)) {
		seg_pop = cds_list_first_entry(&gc->xnp_wait[xnp_lvl].lst,
					       struct segment, list);
		cds_list_del(&seg_pop->list);
		assert(0 < os_atomic32_read(&gc->xnp_wait_nr[xnp_lvl]));
		assert(XNP_BIN_ID == segment_get_private(seg_pop));
		assert(SEG_GC_XNP == segment_get_state(seg_pop));
		os_atomic32_dec(&gc->xnp_wait_nr[xnp_lvl]);
	}
	os_spinlock_unlock(&gc->xnp_wait[xnp_lvl].sl);
	return seg_pop;
}

static inline int gc_xnp_try_popone(
	struct gc_salsa  *const gc,
	struct segment *const seg)
{
	const u32 heat_tmp = segment_get_stream(seg);
	const u32 xnp_lvl = gc->xnp_levels > heat_tmp ?
		heat_tmp : gc->xnp_levels - 1;
	struct segment *p, *q;
	int err = -ENOENT;
	os_spinlock_lock(&gc->xnp_wait[xnp_lvl].sl);
	cds_list_for_each_entry_safe(p, q, &gc->xnp_wait[xnp_lvl].lst, list) {
		if (seg == p) {
			err = 0;
			assert(XNP_BIN_ID == segment_get_private(seg));
			assert(SEG_GC_XNP == segment_get_state(seg));
			os_atomic32_dec(&gc->xnp_wait_nr[xnp_lvl]);
			cds_list_del(&seg->list);
			break;
		}
	}
	os_spinlock_unlock(&gc->xnp_wait[xnp_lvl].sl);
	return err;
}

static inline u32 gc_salsa_n_seg2bin(
	struct gc_salsa  *const gc,
	const u32        valid_nr)
{
	if (unlikely(gc->base.seg_size == valid_nr))
		return gc->bin_nr - 1;
	else
		return valid_nr / gc->bin_div;
}

#define XNP_CHERRY_PICK_THRESHOLD	10ULL /* % */
#if	0
/* XXX: not being used */
static inline void gc_xnp_cherry_pick_best(
	struct gc_salsa  *const gc,
	const u32        xnp_lvl)
{
	const u32 cherry_pick_thr =
		gc->base.seg_size * XNP_CHERRY_PICK_THRESHOLD / 100ULL;
	struct segment *p, *q, *best = NULL;
	u32 min = gc->base.seg_size;
	os_spinlock_lock(&gc->xnp_wait[xnp_lvl].sl);
	cds_list_for_each_entry_safe_reverse(p, q, &gc->xnp_wait[xnp_lvl].lst, list) {
		u32 valid_nr = segment_valid_nr(p);
		if (valid_nr < cherry_pick_thr && valid_nr < min) {
			min = valid_nr;
			best = p;
		}
	}
	if (NULL != best) {
		os_atomic32_dec(&gc->xnp_wait_nr[xnp_lvl]);
		cds_list_del(&best->list);
	}
	os_spinlock_unlock(&gc->xnp_wait[xnp_lvl].sl);
	if (NULL != best) {
		const u32 newbin = gc_salsa_n_seg2bin(gc, segment_valid_nr(best));
		gc_salsa_n_segment_add_to_bin(gc, best, newbin);
		gc_salsa_n_segment_locks_activate(gc, best);
		os_atomic32_inc(&gc->cherry_picked_nr);
	}
}
#endif

static inline struct segment * gc_xnp_pushone_popone(
		struct gc_salsa  *const gc,
		struct segment *const seg,
		const u32        xnp_lvl)
{
	struct segment *seg_pop = NULL;
	u32 xnp_lvl_len;
	os_spinlock_lock(&gc->xnp_wait[xnp_lvl].sl);
	xnp_lvl_len = os_atomic32_inc_return(&gc->xnp_wait_nr[xnp_lvl]);
	cds_list_add_tail(&seg->list, &gc->xnp_wait[xnp_lvl].lst);
	if (xnp_lvl_len >= gc->xnp_thr[xnp_lvl]) {
		assert(!cds_list_empty(&gc->xnp_wait[xnp_lvl].lst));
		seg_pop = cds_list_first_entry(&gc->xnp_wait[xnp_lvl].lst,
					       struct segment, list);
		cds_list_del(&seg_pop->list);
		assert(0 < os_atomic32_read(&gc->xnp_wait_nr[xnp_lvl]));
		os_atomic32_dec(&gc->xnp_wait_nr[xnp_lvl]);
		/* assert(seg != seg_pop); */
		if (unlikely(seg == seg_pop)) {
			ERR("seg == seg_pop cnt = %u n = %u thr = %u heat = %u",
				os_atomic32_read(&gc->xnp_wait_nr[xnp_lvl]),
			    xnp_lvl_len, gc->xnp_thr[xnp_lvl], xnp_lvl);
		}
	}
	assert(SEG_GC_XNP != segment_get_state(seg));
	segment_set_state(seg, SEG_GC_XNP);
	segment_set_private(seg, XNP_BIN_ID);
	os_spinlock_unlock(&gc->xnp_wait[xnp_lvl].sl);

	return seg_pop;
}

/*
 * SALSA thresholds
 */
#define	MED_BIN_THRESHOLD(seg_size)	(((seg_size) * 120) / 400)
#define	HIGH_BIN_THRESHOLD(seg_size)	(((seg_size) * 180) / 400)

static void gc_salsa_seg_destaged(
	struct gc  *const gc_in,
	struct segment *const seg)
{
	u32 bin;
	const u32 seg_size = gc_in->seg_size;
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	const u32 valid_nr = segment_valid_nr(seg);
	const u32 nfree = seg_size - valid_nr;
	if (nfree < MED_BIN_THRESHOLD(seg_size)) {
		bin = GC_SALSA_LOW_QUEUE;
	} else if (nfree < HIGH_BIN_THRESHOLD(seg_size)) {
		bin = GC_SALSA_MED_QUEUE;
	} else {
		bin = GC_SALSA_HIGH_QUEUE;
	}
	gc_salsa_n_segment_add_to_bin(gc, seg, bin);
	gc_salsa_n_segment_locks_activate(gc, seg);
	DBG("inserted seg%p into %u bin", seg, bin);
}

static void gc_salsa_page_invalidate(
	struct gc      *const gc_in,
	struct segment *const seg,
	const u32        grain_nr)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	const u32 seg_size = gc_in->seg_size;
	const u32 valid_nr = segment_putn(seg, grain_nr);
	const u32 nfree = seg_size - valid_nr;
	u32 newbin = -1;
	u32 oldbin = -1;
	int rc;
	assert(NULL != seg);
	assert(seg_size > valid_nr);

	rc = gc_salsa_n_segment_lock_and_lookup(gc, seg);
	if (rc) /* segment not in the GC queues */
		goto exit_segment_unlock;

	if (unlikely(0 == segment_valid_nr(seg))) {
		gc_free_seg_on_zero_valid_pages(gc, seg);
		return;
	}
	oldbin = segment_get_private(seg);
	newbin = oldbin;
	if (MED_BIN_THRESHOLD(seg_size) == nfree) {
		newbin = GC_SALSA_MED_QUEUE;
	} else if (HIGH_BIN_THRESHOLD(seg_size) == nfree) {
		newbin = GC_SALSA_HIGH_QUEUE;
	}
	if (newbin != oldbin) {
		gc_salsa_n_segment_rm_from_bin(gc, seg, oldbin);
		gc_salsa_n_segment_add_to_bin(gc, seg, newbin);
	}
	DBG("inserted seg%p into %u bin nfree=%u", seg, newbin, nfree);

exit_segment_unlock:
	gc_salsa_n_segment_unlock(gc, seg);
}

static void gc_salsa_n_seg_destaged(
	struct gc  *const gc_in,
	struct segment *const seg)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	const u32 valid_nr = segment_valid_nr(seg);
	const u32 newbin = valid_nr / gc->bin_div;
	if (unlikely(0 == valid_nr)) {
		gc_recycle_seg_on_zero_valid_pages(gc, seg, 0);
		return;
	}
	gc_salsa_n_segment_add_to_bin(gc, seg, newbin);
	gc_salsa_n_segment_locks_activate(gc, seg);
}

static void gc_salsa_n_page_invalidate_common(
	struct gc_salsa  *const gc,
	struct segment *const seg)
{
	const int rc = gc_salsa_n_segment_lock_and_lookup(gc, seg);
	if (rc) /* segment not in the GC queues */
		goto exit_segment_unlock;

	if (unlikely(0 == segment_valid_nr(seg))) {
		gc_free_seg_on_zero_valid_pages(gc, seg);
		return;
	}

	/* the segment is ours */
	{
		const u32 oldbin = segment_get_private(seg);
		const u32 newbin = segment_valid_nr(seg) / gc->bin_div;
		if (newbin != oldbin) {
			gc_salsa_n_segment_rm_from_bin(gc, seg, oldbin);
			gc_salsa_n_segment_add_to_bin(gc, seg, newbin);
		}
	}

exit_segment_unlock:
	gc_salsa_n_segment_unlock(gc, seg);
}

static void gc_salsa_n_page_invalidate(
	struct gc      *const gc_in,
	struct segment *const seg,
	const u32        grain_nr)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	const u32 valid_nr __attribute__((unused)) = segment_putn(seg, grain_nr);
	assert(valid_nr <= gc_in->seg_size);

	gc_salsa_n_page_invalidate_common(gc, seg);

}

static void gc_salsa_n_xnp_intrpt_safe_page_invalidate(
	struct gc      *const gc_in,
	struct segment *const seg,
	const u32             grain_nr)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	const u32 valid_nr __attribute__((unused)) = segment_putn(seg, grain_nr);
	const u32 bit_idx = scm_get_seg_idx(gc_in->scm, seg);
	BUG_ON(gc_in->seg_size < valid_nr);
	if (0U == os_test_bit(gc->seg_bmap, bit_idx))
		os_test_and_set_bit(gc->seg_bmap, bit_idx);
}

static void gc_salsa_n_xnp_seg_destaged(
	struct gc  *const gc_in,
	struct segment *const seg)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	u32 heat_tmp = segment_get_stream(seg);
	/* only hot or cold for now */
	const u32 xnp_lvl = gc->xnp_levels > heat_tmp ?
		heat_tmp : gc->xnp_levels - 1;
	if (unlikely(0 == segment_valid_nr(seg))) {
		gc_recycle_seg_on_zero_valid_pages(gc, seg, 0);
		return;
	}
	if (!segment_is_reloc(seg)) {
		/* only delay it if not a relocated segment, we expect
		 * no recurring pattern there */
		struct segment *seg_popped =
			gc_xnp_pushone_popone(gc, seg, xnp_lvl);
		if (NULL != seg_popped) {
			const u32 newbin =
				segment_valid_nr(seg_popped) / gc->bin_div;
			gc_salsa_n_segment_add_to_bin(gc, seg_popped, newbin);
			gc_salsa_n_segment_locks_activate(gc, seg_popped);
		}
	} else {
		const u32 newbin = segment_valid_nr(seg) / gc->bin_div;
		gc_salsa_n_segment_add_to_bin(gc, seg, newbin);
		gc_salsa_n_segment_locks_activate(gc, seg);
	}
}

static void gc_salsa_n_xnp_ovrw_seg_destaged(
	struct gc  *const gc_in,
	struct segment *const seg)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	const u32 valid_nr = segment_valid_nr(seg);
	if (unlikely(0 == valid_nr)) {
		gc_recycle_seg_on_zero_valid_pages(gc, seg, 0);
		return;
	}
	DBG("Received seg idx=%"PRIu64" valid_nr=%u bin_div=%u",
		scm_get_seg_idx(gc_in->scm, seg), valid_nr, gc->bin_div);

	/* only hot or cold for now */
	if (gc_in->seg_size == valid_nr) {
		/* fully valid */
		const u32 newbin = gc_salsa_n_seg2bin(gc, valid_nr);
		gc_salsa_n_segment_add_to_bin(gc, seg, newbin);
		gc_salsa_n_segment_locks_activate(gc, seg);
	} else {
		/* already some overwrites at time of destage */
		u32 heat_tmp = segment_get_stream(seg);
		struct segment *seg_popped;
		/* only hot or cold for now */
		const u32 xnp_lvl = gc->xnp_levels > heat_tmp ?
			heat_tmp : gc->xnp_levels - 1;
		seg_popped = gc_xnp_pushone_popone(gc, seg, xnp_lvl);
		if (NULL != seg_popped) {
			const u32 newbin =
				gc_salsa_n_seg2bin(gc, segment_valid_nr(seg_popped));
			gc_salsa_n_segment_add_to_bin(gc, seg_popped, newbin);
			gc_salsa_n_segment_locks_activate(gc, seg_popped);
		}
	}
}

static inline int gc_salsa_n_xnp_try_cherry_pick(
	struct gc_salsa  *const gc,
	struct segment *const seg,
	u32                   valid_nr)
{
	const u32 seg_bin = segment_get_private(seg);
	const u32 cherry_pick_thr =
		gc->base.seg_size * XNP_CHERRY_PICK_THRESHOLD / 100ULL;
	u32 newbin;
	int err = 0;

	/* cherry_pick */
	if (!(valid_nr < cherry_pick_thr && XNP_BIN_ID == seg_bin)) {
		err = -ENOENT;
		goto fail0;
	}

	/* check again with lock held */
	os_spinlock_lock(&seg->sl);

	valid_nr = segment_valid_nr(seg);

	if (!(valid_nr < cherry_pick_thr && XNP_BIN_ID == seg_bin)) {
		err = -ENOENT;
		goto unlock;
	}

	if (0 != gc_xnp_try_popone(gc, seg)) {
		err = -ENOENT;
		goto unlock;
	}

	if (0 == segment_valid_nr(seg)) {
		gc_recycle_seg_on_zero_valid_pages(gc, seg, 0);
		goto unlock_success;
	}

	newbin = gc_salsa_n_seg2bin(gc, segment_valid_nr(seg));
	gc_salsa_n_segment_add_to_bin(gc,seg, newbin);
	assert(SEG_GC != segment_get_state(seg));
	segment_set_state(seg, SEG_GC);

unlock_success:
	os_atomic32_inc(&gc->cherry_picked_nr);
unlock:
	os_spinlock_unlock(&seg->sl);
fail0:
	return err;
}

static void gc_salsa_n_xnp_ovrw_page_invalidate(
	struct gc      *const gc_in,
	struct segment *const seg,
	const u32        grain_nr)
{
	int rc;
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	const u32 valid_nr = segment_putn(seg, grain_nr);

	#if defined(GC_CHECK_ACTIVE)
	if (valid_nr < GC_ACTIVE_LOW_WATERMARK) {
		os_wakeup_all(&gc->base.low_watermark_waitq);
	}
	#endif


	if (0 == gc_salsa_n_xnp_try_cherry_pick(gc, seg, valid_nr))
		return;

	rc = gc_salsa_n_segment_lock_and_lookup(gc, seg);
	if (0 != rc) { /* not in the GC queues */
		gc_salsa_n_segment_unlock(gc, seg);
		return;
	}

	if (unlikely(0 == segment_valid_nr(seg))) {
		gc_free_seg_on_zero_valid_pages(gc, seg);
		return;
	}

	if (gc_in->seg_size - 1 == valid_nr /* && 0 == seg->is_reloc */) {
		/* 1st invalidate detected */
		const u32 oldbin = segment_get_private(seg);
		const u32 heat_tmp = segment_get_stream(seg);
		struct segment *seg_popped;
		/* only hot or cold for now */
		const u32 xnp_lvl = heat_tmp < gc->xnp_levels ?
			heat_tmp : gc->xnp_levels - 1;
		gc_salsa_n_segment_rm_from_bin(gc, seg, oldbin);
		gc_salsa_n_segment_locks_deactivate(gc, seg);
		seg_popped = gc_xnp_pushone_popone(gc, seg, xnp_lvl);
		if (NULL != seg_popped) {
			const u32 newbin =
				gc_salsa_n_seg2bin(gc, segment_valid_nr(seg_popped));
			gc_salsa_n_segment_add_to_bin(gc, seg_popped, newbin);
			gc_salsa_n_segment_locks_activate(gc, seg_popped);
		}
	} else {
		const u32 oldbin = segment_get_private(seg);
		const u32 newbin =
			gc_salsa_n_seg2bin(gc, segment_valid_nr(seg));
		if (oldbin != newbin) {
			gc_salsa_n_segment_rm_from_bin(gc, seg, oldbin);
			gc_salsa_n_segment_add_to_bin(gc,seg, newbin);
		}
		gc_salsa_n_segment_unlock(gc, seg);
	}
}

static void gc_salsa_n_xnp_ovrw_pat_drain_queue(
	struct gc_salsa    *const gc,
	const u32              xnp_idx,
	struct cds_list_head *const head)
{
	struct segment *seg, *q;
	struct xnp_queue *xnp_queue = &gc->xnp_circ_buf[xnp_idx];
	u32 tot_removed = 0;
	os_spinlock_lock(&xnp_queue->sl);
	cds_list_for_each_entry_safe(seg, q, &xnp_queue->l, list) {
		cds_list_del(&seg->list);
		cds_list_add_tail(&seg->list, head);
		tot_removed++;
	}
	os_spinlock_unlock(&xnp_queue->sl);
	assert(os_atomic64_read(&gc->xnp_tot_capacity) >= tot_removed);
	os_atomic64_sub(&gc->xnp_tot_capacity, tot_removed);
}

static void gc_salsa_n_xnp_ovrw_pat_drain_next_queue_and_inc(
	struct gc_salsa  *const gc)
{
	u32 new_xnp_idx;
	struct segment *seg, *q;
	CDS_LIST_HEAD(drained);
	os_rwlock_down_write(&gc->xnp_current_rwlock);
	new_xnp_idx = os_atomic32_inc_return(&gc->xnp_current) %
		gc->xnp_circ_buf_len;
	gc_salsa_n_xnp_ovrw_pat_drain_queue(gc, new_xnp_idx, &drained);
	os_rwlock_up_write(&gc->xnp_current_rwlock);
	cds_list_for_each_entry_safe(seg, q, &drained, list) {
		const u32 newbin = segment_valid_nr(seg) / gc->bin_div;
		cds_list_del(&seg->list);
		gc_salsa_n_segment_add_to_bin(gc, seg, newbin);
		gc_salsa_n_segment_locks_activate(gc, seg);
	}
}

static void gc_salsa_n_xnp_ovrw_pat_enqueue(
	struct gc_salsa  *const gc,
	struct segment     *const seg)
{
	u32 cur_xnp_idx;
	struct xnp_queue *xnp_queue;
	os_rwlock_down_read(&gc->xnp_current_rwlock);
	cur_xnp_idx = os_atomic32_read(&gc->xnp_current) %
		gc->xnp_circ_buf_len;
	xnp_queue = &gc->xnp_circ_buf[cur_xnp_idx];
	os_spinlock_lock(&xnp_queue->sl);
	cds_list_add_tail(&seg->list, &xnp_queue->l);
	os_spinlock_unlock(&xnp_queue->sl);
	os_atomic64_inc(&gc->xnp_tot_capacity);
	os_rwlock_up_read(&gc->xnp_current_rwlock);
}

static void gc_salsa_n_xnp_ovrw_pat_seg_destaged(
	struct gc  *const gc_in,
	struct segment *const seg)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	const u32 valid_nr = segment_valid_nr(seg);
	const bool xnp_full = os_atomic64_read(&gc->xnp_tot_capacity) >=
		gc->xnp_max_capacity;
	if (unlikely(0 == valid_nr)) {
		gc_recycle_seg_on_zero_valid_pages(gc, seg, 0);
		return;
	}
	/* increment xnp queue index and drain queue*/
	gc_salsa_n_xnp_ovrw_pat_drain_next_queue_and_inc(gc);

	/* only hot or cold for now */
	if (gc_in->seg_size == valid_nr || xnp_full) {
		/* fully valid */
		const u32 newbin = valid_nr / gc->bin_div;
		gc_salsa_n_segment_add_to_bin(gc, seg, newbin);
		gc_salsa_n_segment_locks_activate(gc, seg);
	} else {
		gc_salsa_n_xnp_ovrw_pat_enqueue(gc, seg);
	}
}

static void gc_salsa_n_xnp_ovrw_pat_seg_relocate(
	struct gc  *const gc_in,
	struct segment *const seg)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	const u32 valid_nr = segment_valid_nr(seg);
	const u32 newbin = valid_nr / gc->bin_div;
	gc_salsa_n_segment_add_to_bin(gc, seg, newbin);
	gc_salsa_n_segment_locks_activate(gc, seg);
}

static void gc_salsa_n_xnp_ovrw_pat_page_invalidate(
	struct gc      *const gc_in,
	struct segment *const seg,
	const u32        grain_nr)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	const u32 valid_nr = segment_putn(seg, grain_nr);
	const bool xnp_full = os_atomic64_read(&gc->xnp_tot_capacity) >=
		gc->xnp_max_capacity;

	int rc = gc_salsa_n_segment_lock_and_lookup(gc, seg);
	if (0 != rc) { /* not in the GC queues */
		gc_salsa_n_segment_unlock(gc, seg);
		return;
	}

	if (unlikely(0 == segment_valid_nr(seg))) {
		gc_free_seg_on_zero_valid_pages(gc, seg);
		return;
	}

	if (gc_in->seg_size - 1 == valid_nr && !segment_is_reloc(seg) && !xnp_full) {
		/* 1st invalidate detected */
		const u32 oldbin = segment_get_private(seg);
		gc_salsa_n_segment_rm_from_bin(gc, seg, oldbin);
		gc_salsa_n_segment_locks_deactivate(gc, seg);
		gc_salsa_n_xnp_ovrw_pat_enqueue(gc, seg);
	} else {
		/* common case */
		const u32 oldbin = segment_get_private(seg);
		const u32 newbin = segment_valid_nr(seg) / gc->bin_div;
		if (oldbin != newbin) {
			gc_salsa_n_segment_rm_from_bin(gc, seg, oldbin);
			gc_salsa_n_segment_add_to_bin(gc,seg, newbin);
		}
		gc_salsa_n_segment_unlock(gc, seg);
	}
}

static inline u32 find_right_bin(
	struct gc_salsa *const gc,
	struct segment    *const seg)
{
	const u32 n_valid = segment_valid_nr(seg);

	u32 bin;
	for (bin = 0; bin < gc->bin_nr - 2; ++bin) {
		const u64 thresh =
			os_atomic64_read(&gc->fifo[bin].threshold);
		if (n_valid < thresh)
			break;
	}
	return bin;
}

static inline void bin_threshold_dec(struct gc_salsa *const gc,
				     struct segment *const seg,
				     const u32 bin)
{
	const u64 bin_thresh_min = gc->base.seg_size / 20;

	const u64 bin_thresh = os_atomic64_read(&gc->fifo[bin].threshold);
	const u64 bin_thresh_prev = (bin > 0)?
			os_atomic64_read(&gc->fifo[bin-1].threshold) + 10:
			bin_thresh_min;

	if (bin_thresh > bin_thresh_min && bin_thresh > bin_thresh_prev)
		os_atomic64_dec(&gc->fifo[bin].threshold);
}

static inline void bin_threshold_inc(struct gc_salsa *const gc,
				     struct segment *const seg,
				     const u32 bin)
{
	const u64 bin_thresh_max = gc->base.seg_size;

	const u64 bin_thresh = os_atomic64_read(&gc->fifo[bin].threshold);
	const u64 bin_thresh_next = (bin < gc->bin_nr-1)?
			os_atomic64_read(&gc->fifo[bin+1].threshold) - 10:
			bin_thresh_max;

	if (bin_thresh < bin_thresh_max && bin_thresh < bin_thresh_next)
		os_atomic64_inc(&gc->fifo[bin].threshold);
}

static inline u32 seg2bin(
	struct gc_salsa *const gc,
	struct segment    *const seg)
{
	const u32 bin = find_right_bin(gc, seg);
	return bin;
}

static void gc_salsa_n_dyn_thr_seg_destaged(
	struct gc  *const gc_in,
	struct segment *const seg)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	/* only hot or cold for now */

	const u32 newbin = seg2bin(gc, seg);
	if (unlikely(0 == segment_valid_nr(seg))) {
		gc_recycle_seg_on_zero_valid_pages(gc, seg, 0);
		return;
	}
	gc_salsa_n_segment_add_to_bin(gc, seg, newbin);
	gc_salsa_n_segment_locks_activate(gc, seg);
}

void gc_salsa_n_dyn_thr_page_invalidate(
	struct gc      *const gc_in,
	struct segment *const seg,
	const u32        grain_nr)
{
	int rc;
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	segment_putn(seg, grain_nr);

	rc = gc_salsa_n_segment_lock_and_lookup(gc, seg);
	if (0 != rc) { /* not in the GC queues */
		gc_salsa_n_segment_unlock(gc, seg);
		return;
	}

	{
		const u32 oldbin = segment_get_private(seg);
		const u32 newbin = seg2bin(gc, seg);
		assert(oldbin != -1);
		if (oldbin != newbin) {
			gc_salsa_n_segment_rm_from_bin(gc, seg, oldbin);
			gc_salsa_n_segment_add_to_bin(gc, seg, newbin);
		}
	}
	gc_salsa_n_segment_unlock(gc, seg);

}

static struct segment *gc_salsa_get_first_seg(
	struct gc_salsa *const gc,
	const u32       bin_in)
{
	u32 bin  = bin_in;
	struct segment *seg;
	int rc;
	assert(bin < gc->bin_nr);
	do {
		seg = NULL;
		os_spinlock_lock(&gc->fifo[bin].sl);
		if (likely(!cds_list_empty(&gc->fifo[bin].lst))) {
			seg = cds_list_first_entry(
				&gc->fifo[bin].lst, struct segment, list);
		}
		os_spinlock_unlock(&gc->fifo[bin].sl);
		if (unlikely(NULL == seg)) {
			DBG("queue%u empty", bin);
			goto out;
		}
		os_spinlock_lock(&seg->sl);
		rc = SEG_GC == segment_get_state(seg) ? 0 : ENODATA;
		if (0 == rc)
			segment_set_state(seg, SEG_INVALID);
		os_spinlock_unlock(&seg->sl);
	} while (ENODATA == rc);
	bin = segment_get_private(seg); /* this might differ to bin_in */
	gc_salsa_n_segment_rm_from_bin(gc, seg, bin);
out:
	return seg;
}

static void gc_salsa_return_failed_reloc_seg(
	struct gc_salsa *const gc,
	struct segment    *const seg)
{
	const u32 valid_nr = segment_valid_nr(seg);
	const u32 nfree = gc->base.seg_size - valid_nr;
	u32 bin;
	if (nfree < MED_BIN_THRESHOLD(gc->base.seg_size)) {
		bin = GC_SALSA_LOW_QUEUE;
	} else if (nfree < HIGH_BIN_THRESHOLD(gc->base.seg_size)) {
		bin = GC_SALSA_MED_QUEUE;
	} else {
		bin = GC_SALSA_HIGH_QUEUE;
	}
	gc_salsa_n_segment_add_to_bin_head(gc, seg, bin);
	gc_salsa_n_segment_locks_activate(gc, seg);
}

static void gc_salsa_n_return_failed_reloc_seg(
	struct gc_salsa *const gc,
	struct segment    *const seg)
{
	const u32 valid_nr = segment_valid_nr(seg);
	const u32 bin = valid_nr / gc->bin_div;
	gc_salsa_n_segment_add_to_bin(gc, seg, bin);
	gc_salsa_n_segment_locks_activate(gc, seg);
}

static int gc_salsa_sto_ctlr_pre_dtr(
	struct gc *const gc_in,
	const u8    ctlr_id)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	int i;
	struct segment *p, *q;
	for (i = 0; i < gc->bin_nr; ++i) {
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
			os_spinlock_lock(&p->sl);
			assert(SEG_GC == segment_get_state(p));
			segment_set_state(p, SEG_INVALID);
			os_spinlock_unlock(&p->sl);
			cds_list_del(&p->list);
			os_atomic32_zero(&p->valid_nr);
			scm_put_free_seg(gc_in->scm, p, 0);
		}
	}
	return 0;
}

static int gc_salsa_n_xnp_sto_ctlr_pre_dtr(
	struct gc *const gc_in,
	const u8    ctlr_id)
{
	struct gc_salsa *const gc = (struct gc_salsa *) gc_in;
	int i;
	struct segment *p, *q;
	gc_salsa_sto_ctlr_pre_dtr(gc_in, ctlr_id);
	for (i = 0; i < gc->xnp_levels; ++i) {
		CDS_LIST_HEAD(l);
		os_spinlock_lock(&gc->xnp_wait[i].sl);
		cds_list_for_each_entry_safe(p, q, &gc->xnp_wait[i].lst, list) {
			if (ctlr_id != p->ctlr_id)
				continue;
			cds_list_del(&p->list);
			assert(0 < os_atomic32_read(&gc->xnp_wait_nr[i]));
			os_atomic32_dec(&gc->xnp_wait_nr[i]);
			cds_list_add_tail(&p->list, &l);
		}
		os_spinlock_unlock(&gc->xnp_wait[i].sl);
		cds_list_for_each_entry_safe(p, q, &l, list) {
			os_spinlock_lock(&p->sl);
			assert(SEG_GC_XNP == segment_get_state(p));
			segment_set_state(p, SEG_INVALID);
			os_spinlock_unlock(&p->sl);
			cds_list_del(&p->list);
			os_atomic32_zero(&p->valid_nr);
			scm_put_free_seg(gc_in->scm, p, 0);
		}
	}
	return 0;
}

static inline  int gc_salsa_relocate_seg(
	struct gc_salsa            *const gc,
	struct gc_io_work_parent *const gciowp,
	struct segment           *const seg,
	const u32                       bin,
	os_atomic32_t            *const exit)
{
	int rc, retry_nr = 0;
	struct sto_ctlr *const sc = gc->base.ctl_data[seg->ctlr_id].sc;
	const gc_reloc_fn_t fn = gc->base.ctl_data[seg->ctlr_id].sc_reloc_fn;
	assert(NULL != sc && NULL != fn);
	do {
		DBG("Reloc seg=%p bin=%u idx=%"PRIu64" ctlr-id=%u valid-nr=%u",
			seg, bin, scm_get_seg_idx(gc->base.scm, seg), seg->ctlr_id, segment_valid_nr(seg));
		gc_io_work_parent_init(gciowp, seg);
		rc = (fn) (sc, gciowp);
		gc_queue_add_page_relocs(
			&gc->fifo[bin], os_atomic32_read(&gciowp->cnt));
		if (unlikely(1000 <= ++retry_nr)) {
			ERR("stuck in reloc loop seg=%p sc-id=%u valid-nr=%u",
				seg, seg->ctlr_id, segment_valid_nr(seg));
			os_msleep(1000);
			break;
		}
	} while (EAGAIN == rc && 0 == os_atomic32_read(exit));
	return rc;
}

static void gc_salsa_n_xnp_intrpt_safe_update_segs(struct gc_salsa *const gc)
{
	int bit;
	const u64 seg_nr = scm_get_seg_nr(gc->base.scm);
	/* FIXME: to support more than 1 gc threads, this has to be serialized */
	os_for_each_set_bit(bit, gc->seg_bmap, seg_nr) {
		struct segment *const seg = scm_get_seg(gc->base.scm, bit);
		/* clear the bit */
		os_test_and_clear_bit(gc->seg_bmap, bit);
		/* update data structures */
		if (0 == gc_salsa_n_xnp_try_cherry_pick(
				gc, seg, segment_valid_nr(seg)))
			continue;
		gc_salsa_n_page_invalidate_common(gc, seg);
	}
}

static void gc_salsa_n_xnp_try_pop_one(struct gc_salsa *const gc)
{
	int i;
	struct segment *seg_popped = NULL;
	for (i = 0; i < gc->xnp_levels && NULL == seg_popped; ++i)
		seg_popped = gc_xnp_popone_force(gc, i);

	if (NULL != seg_popped) {
		const u32 newbin = segment_valid_nr(seg_popped) /
			gc->bin_div;
		gc_salsa_n_segment_add_to_bin(gc, seg_popped, newbin);
		gc_salsa_n_segment_locks_activate(gc, seg_popped);
	}
}

static os_thread_fun_return gc_salsa_run(void *arg)
{
	struct os_thread_arg *gcarg = arg;
	struct gc_salsa *const gc = (struct gc_salsa *) gcarg->private;
	const u32 id = gcarg->id;
	struct sto_capacity_mgr *const scm = gc->base.scm;
	os_atomic64_t *const nfree_segs = scm_seg_queues_get_free_ref(&scm->seg_qs);
	const u32 bin_nr __attribute__((unused)) = gc_get_bin_nr(&gc->base);
	os_atomic32_t *const exit = &gc->base.exit;
	const u64 LOW_WATERMARK = gc->base.LOW_WATERMARK;
	const u64 HIGH_WATERMARK = gc->base.HIGH_WATERMARK;
	struct gc_io_work_parent *const gciowp = &gc->base.gciowp[id];
	os_free(gcarg);
	gcarg = NULL;

	MSG("%s with LOW_WATERMARK=%"PRIu64" HIGH_WATERMARK=%"PRIu64" bins=%u" \
	    " id=%u", gc_string[gc->base.gc_type], LOW_WATERMARK,
	    HIGH_WATERMARK, bin_nr, id);

	for (; 0 == os_atomic32_read(exit); ) {
		os_wait_event_timeout(gc->base.low_watermark_waitq,
				os_atomic64_read(nfree_segs) <= LOW_WATERMARK ||
				1 == os_atomic32_read(exit),
				GC_WAIT_WATERMARK_TIMEOUT);
		DBG("Starting garbage collection.");
		for (; 0 == os_atomic32_read(exit) &&
			     os_atomic64_read(nfree_segs) < HIGH_WATERMARK; ) {
			struct segment *seg = NULL;
			int bin = GC_SALSA_HIGH_QUEUE, rc;
			for (; GC_SALSA_LOW_QUEUE <= bin && NULL == seg; --bin)
				seg = gc_salsa_get_first_seg(gc, bin);
			if (unlikely(NULL == seg)) {
				os_msleep_interruptible(1);
				continue;
			}
			bin = segment_get_private(seg);
			DBG("Chose candidate segment for reloc: idx=%"PRIu64,
				scm_get_seg_idx(scm, seg));
			gc_seg_selected(&gc->base, seg);
			rc = gc_salsa_relocate_seg(gc, gciowp, seg, bin, exit);
			if (unlikely(0 != rc)) {
				gc_salsa_return_failed_reloc_seg(gc, seg);
				ERR("lsa_ctlr_relocate_segment "	\
					" failed with %d.", rc);
				continue;
			}
			/* push it to the free list */
			scm_put_free_seg(scm, seg, 0);
			gc_seg_released(&gc->base, &gc->fifo[bin], os_atomic32_read(&gciowp->cnt));
			/* avoid tight loop kernel soft-lockups */
			os_schedule();
		}
	}
	os_atomic32_set(&gc->base.gc_done[id], 1);
	return 0;
}

static inline void gc_salsa_n_run_choose_seg_and_reloc(
	struct gc_salsa            *const gc,
	const u32                       bin_nr,
	struct gc_io_work_parent *const gciowp)
{
	os_atomic32_t *const exit = &gc->base.exit;
	struct sto_capacity_mgr *const scm = gc->base.scm;
	struct segment *seg = NULL;
	int bin, rc;

	for (bin = 0; bin < bin_nr && NULL == seg; ++bin)
		seg = gc_salsa_get_first_seg(gc, bin);

	if (unlikely(NULL == seg)) {
		os_schedule();
		gc_salsa_n_xnp_try_pop_one(gc);
		return;
	}
	bin = segment_get_private(seg);
	DBG("Chose candidate segment for reloc: idx=%"PRIu64 " valid_nr=%u",
		scm_get_seg_idx(scm, seg), segment_valid_nr(seg));

	gc_seg_selected(&gc->base, seg);
	rc = gc_salsa_relocate_seg(gc, gciowp, seg, bin, exit);
	if (unlikely(0 != rc)) {
		gc_salsa_n_return_failed_reloc_seg(gc, seg);
		DBG("lsa_ctlr_relocate_segment rc=%d.", rc);
		return;
	}
	/* push it to the free list */
	scm_put_free_seg(scm, seg, 0);
	gc_seg_released(&gc->base, &gc->fifo[bin],
			os_atomic32_read(&gciowp->cnt));
	DBG("Finished relocation of segment idx=%"PRIu64,
		scm_get_seg_idx(scm, seg));
}

// check if GC queue has segments that have active pages less than @threshold
u32
gc_queue_check_active_threshold(struct gc_queue *q, u32 threshold)
{
	bool ret;
	struct segment *s;

	ret = false;
	os_spinlock_lock(&q->sl);
	cds_list_for_each_entry(s, &q->lst, list) {
		u32 valid_nr = os_atomic32_read(&s->valid_nr);
		if (valid_nr <= threshold) {
			ret = true;
			break;
		}
	}
	os_spinlock_unlock(&q->sl);

	return ret;
}


bool
gc_salsa_check_start(struct gc_salsa *gc)
{
	if (gc_check_start(&gc->base))
		return true;

	#if defined(GC_CHECK_ACTIVE)
	return gc_queue_check_active_threshold(&gc->fifo[0], GC_ACTIVE_LOW_WATERMARK);
	#else
	return false;
	#endif
}

bool
gc_salsa_check_continue(struct gc_salsa *gc)
{
	if (gc_check_continue(&gc->base))
		return true;

	#if defined(GC_CHECK_ACTIVE)
	return gc_queue_check_active_threshold(&gc->fifo[0], GC_ACTIVE_HIGH_WATERMARK);
	#else
	return false;
	#endif
}

static os_thread_fun_return
gc_salsa_do_run(void *arg, const bool intrpt_safe)
{
	struct os_thread_arg *gcarg = arg;
	struct gc_salsa *const gc = (struct gc_salsa *) gcarg->private;
	const u32 id = gcarg->id;
	//os_atomic64_t *const nfree_segs = scm_get_free_seg_ref(gc->base.scm);
	const u32 bin_nr = gc_get_bin_nr(&gc->base);
	os_atomic32_t *const exit = &gc->base.exit;
	const u64 LOW_WATERMARK __attribute__((unused)) = gc->base.LOW_WATERMARK;
	const u64 HIGH_WATERMARK __attribute__((unused)) = gc->base.HIGH_WATERMARK;
	struct gc_io_work_parent *const gciowp = &gc->base.gciowp[id];

	assert(bin_nr < gc->bin_nr);
	os_free(gcarg);
	gcarg = NULL;

	MSG("%s with LOW_WATERMARK=%"PRIu64" HIGH_WATERMARK=%"PRIu64" bins=%u" \
	    " id=%u", gc_string[gc->base.gc_type], LOW_WATERMARK,
	    HIGH_WATERMARK, bin_nr, id);

	// NB: ->gc_active_threads is initialized to -1, to detect GC
	// implementations that do not use it.  The thread with id == 0, is
	// responsible for adding one in its initalization
	if (id == 0)
		os_atomic32_inc(&gc->base.gc_active_threads);

	for (; 0 == os_atomic32_read(exit); ) {
		os_wait_event_interruptible(gc->base.low_watermark_waitq,
				gc_salsa_check_start(gc) || 1 == os_atomic32_read(exit));

		DBG("GC %u starts", id);
		if (1 == os_atomic32_inc_return(&gc->base.gc_active_threads))
			scm_throttle_gc_starts(gc->base.scm);

		for (; 0 == os_atomic32_read(exit) && gc_salsa_check_continue(gc); ) {

			/* update data structures since last time */
			if (intrpt_safe && 0 == id) /* sequential */
				gc_salsa_n_xnp_intrpt_safe_update_segs(gc);

			gc_salsa_n_run_choose_seg_and_reloc(gc, bin_nr, gciowp);
		}

		DBG("GC %u stops", id);
		if (0 == os_atomic32_dec_return(&gc->base.gc_active_threads))
			scm_throttle_gc_stopped(gc->base.scm);
	}
	os_atomic32_set(&gc->base.gc_done[id], 1);
	return 0;
}

static os_thread_fun_return
gc_salsa_n_run(void *arg)
{
	return gc_salsa_do_run(arg, false);
}

static os_thread_fun_return
gc_salsa_n_intrpt_safe_run(void *arg)
{
	return gc_salsa_do_run(arg, true);
}


/*
 * We can use this to "inherit" the initialization of structs,
 * overriding only a subset of functions, making the code clearer,
 * less error-prone, and more aligned with the DRY principle.
 */

#define INIT_FNS_SALSA \
	.ctr              = gc_salsa_ctr, \
	.init             = gc_salsa_init, \
	.init_stats       = gc_salsa_init_stats, \
	.dtr              = gc_salsa_dtr, \
	.run              = gc_salsa_run, \
	.seg_write        = gc_salsa_seg_destaged, \
	.seg_relocate     = gc_salsa_seg_destaged, \
	.page_invalidate  = gc_salsa_page_invalidate, \
	.get_stream_nr    = gc_get_stream_nr_dummy, \
	.get_bin_nr       = gc_salsa_get_bin_nr, \
	.print_list_size  = gc_salsa_print_list_size, \
	.print_info       = gc_nbin_print_info,	\
	.sto_ctlr_pre_dtr = gc_salsa_sto_ctlr_pre_dtr,


#define INIT_FNS_SALSA_NBIN \
	INIT_FNS_SALSA \
	.run             = gc_salsa_n_run, \
	.seg_write       = gc_salsa_n_seg_destaged, \
	.seg_relocate    = gc_salsa_n_seg_destaged, \
	.page_invalidate = gc_salsa_n_page_invalidate, \
	.get_bin_nr       = gc_salsa_n_get_bin_nr,

#define INIT_FNS_SALSA_NBIN_HEATSEG \
	INIT_FNS_SALSA_NBIN \
	.get_stream_nr     = gc_hs_get_stream_nr, \
	.print_info      = gc_nbin_hs_print_info,

#define INIT_FNS_SALSA_NBIN_XNP \
	INIT_FNS_SALSA_NBIN_HEATSEG \
	.get_stream_nr   = gc_hs_get_stream_nr, \
	.print_list_size = gc_salsa_n_xnp_print_list_size,\
	.print_info      = gc_nbin_xnp_print_info,	\
	.sto_ctlr_pre_dtr= gc_salsa_n_xnp_sto_ctlr_pre_dtr,


static const struct gc_fns salsa_fns = {
	INIT_FNS_SALSA
};

static const struct gc_fns salsa_n_fns = {
	INIT_FNS_SALSA_NBIN
};

static const struct gc_fns salsa_n_xnp_hs_fns = {
	INIT_FNS_SALSA_NBIN_XNP
	.seg_write       = gc_salsa_n_xnp_seg_destaged,
	.seg_relocate    = gc_salsa_n_xnp_seg_destaged,
	.page_invalidate = gc_salsa_n_page_invalidate,
};

static const struct gc_fns salsa_n_xnp_ovrw_hs_fns = {
	INIT_FNS_SALSA_NBIN_XNP
	.seg_write       = gc_salsa_n_xnp_ovrw_seg_destaged,
	.seg_relocate    = gc_salsa_n_xnp_ovrw_seg_destaged,
	.page_invalidate = gc_salsa_n_xnp_ovrw_page_invalidate,
};

static const struct gc_fns salsa_n_xnp_intrpt_safe_fns = {
	INIT_FNS_SALSA_NBIN_XNP
	.seg_write       = gc_salsa_n_xnp_seg_destaged,
	.seg_relocate    = gc_salsa_n_xnp_seg_destaged,
	.run             = gc_salsa_n_intrpt_safe_run,
	.page_invalidate = gc_salsa_n_xnp_intrpt_safe_page_invalidate,
};

static const struct gc_fns salsa_n_xnp_ovrw_hs_patent_fns = {
	INIT_FNS_SALSA_NBIN_XNP
	.seg_write       = gc_salsa_n_xnp_ovrw_pat_seg_destaged,
	.seg_relocate    = gc_salsa_n_xnp_ovrw_pat_seg_relocate,
	.page_invalidate = gc_salsa_n_xnp_ovrw_pat_page_invalidate,
	.print_list_size = gc_salsa_n_xnp_pat_print_list_size,
};

static const struct gc_fns salsa_n_hs_fns = {
	INIT_FNS_SALSA_NBIN
	.get_stream_nr   = gc_hs_get_stream_nr,
	.print_info      = gc_nbin_hs_print_info,
};

static const struct gc_fns salsa_n_dyn_thr_fns = {
	INIT_FNS_SALSA_NBIN
	.get_stream_nr   = gc_hs_get_stream_nr,
	.print_info      = gc_nbin_hs_print_info,
	.seg_write       = gc_salsa_n_dyn_thr_seg_destaged,
	.seg_relocate    = gc_salsa_n_dyn_thr_seg_destaged,
	.page_invalidate = gc_salsa_n_dyn_thr_page_invalidate,
};
