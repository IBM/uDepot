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
#include "libos/os-atomic.h"
#include "libos/os-debug.h"
#include "libos/os-lock.h"
#include "libos/os-malloc.h"
#include "libos/os-string.h"
#include "libos/os-thread.h"
#include "libos/os-time.h"
#include "sto-ctlr/sto-capacity-mgr-parameters.h"
#include "sto-ctlr/sto-capacity-mgr.h"
#include "sto-ctlr/sto-ctlr.h"

const char *const gc_string[] = {
	"GC_GREEDY_WINDOW",
	"GC_CIRCULAR_BUFFER",
	"GC_CONTAINER_MARKER",
	"",
	"GC_SALSA_3BIN",
	"GC_NBIN",
	"",
	"",
	"GC_NBIN_INFLIGHT",
	"",
	"GC_NBIN_XNP_INTRPT_SAFE",
	"GC_NBIN_XNP_HEAT_SEG",
	"GC_LOG2_BIN",
	"GC_NBIN_HEAT_SEGREGATION",
	"GC_NBIN_XNP_OVRW_HEAT_SEG",
	"GC_NBIN_DYN_THRESHOLD",
	"GC_NBIN_XNP_PATENT"
};

int gc_ctr(
	const struct gc_parameters *const params,
	struct sto_capacity_mgr    *const scm,
	struct gc                 **const gc_out)
{
	int err = 0;
	struct gc *gc = NULL;
	int i, thread;
	const struct gc_fns *fns = NULL;
	u32 max_workers = SALSA_GC_MAX_IO_WORKERS;
	switch (params->gc) {
	case GC_GREEDY_WINDOW:
	case GC_CIRCULAR_BUFFER:
		gc_base_get_fns(params->gc, &fns);
		break;
	case GC_SALSA_3BIN:
	case GC_SALSA_NBIN:
	case GC_NBIN_XNP_HEAT_SEG:
	case GC_NBIN_XNP_OVRW_HEAT_SEG:
	case GC_NBIN_DYN_THRESHOLD:
	case GC_NBIN_HEAT_SEG:
	case GC_NBIN_XNP_PATENT:
	case GC_NBIN_XNP_INTRPT_SAFE:
		gc_salsa_get_fns(params->gc, &fns);
		break;
	default:
		assert(0);
		ERR_SET_GOTO(err, EINVAL, fail0);
		break;
	}

	assert(NULL != fns);
	err = fns->ctr(params, &gc);
	ERR_CHK_PRNT_GOTO(0 != err, fail0,
			"gc_ctr failed with %d.", err);
	assert(NULL != gc);

	/* Init common fields */
	os_atomic32_zero(&gc->exit);
	os_atomic64_zero(&gc->valid_seg_nr);
	os_atomic64_zero(&gc->inval_page_nr);
	*((struct sto_capacity_mgr **) &gc->scm) = scm;
	*((u64 *) &gc->seg_size) = scm_get_seg_size(scm);
	*((enum gc_type *) &gc->gc_type) = params->gc;
	memcpy((void *) &gc->fns, &fns, sizeof(fns));
	os_waitqueue_head_init(&gc->low_watermark_waitq);

	if (params->low_watermark < 2 ||
		params->high_watermark <= params->low_watermark) {
		*((u64 *)&gc->LOW_WATERMARK) = 2 * SALSA_MIN_REL_SEGMENTS;
		*((u64 *)&gc->HIGH_WATERMARK) = gc->LOW_WATERMARK + 2;
	} else {
		*((u64 *)&gc->LOW_WATERMARK)  = params->low_watermark;
		*((u64 *)&gc->HIGH_WATERMARK) = params->high_watermark;
	}

	*(u32 *)&gc->thread_nr = params->thread_nr <= GC_MAX_THREADS ?
		params->thread_nr : 1;
	os_atomic32_set(&gc->gc_active_threads, 0);

	assert(gc->LOW_WATERMARK < gc->HIGH_WATERMARK);

	max_workers = gc->seg_size <= SALSA_GC_MAX_IO_WORKERS ?
		gc->seg_size : SALSA_GC_MAX_IO_WORKERS;
	for (thread = 0; thread < gc->thread_nr; ++thread) {
		struct gc_io_work_parent *const gciowp = &gc->gciowp[thread];
		os_waitqueue_head_init(&gciowp->waitq);
		for (i = 0; i < max_workers; ++i) {
			struct gc_io_work *const gciow = &gciowp->workers[i];
			gciow->parent = gciowp;
		}
	}

	/* Class specific initialization */
	err = fns->init(gc);
	ERR_CHK_PRNT_GOTO(0 != err, fail1,
			"gc_init failed with %d.", err);

	/* GC ENDS */
	assert(0 == err);
	*gc_out = gc;
	return 0;
fail1:

	fns->dtr(gc);
fail0:
	return err;
}

int gc_init_threads(struct gc *const gc)
{
	int err = 0;
	int i;

	// NB: We set it to -1, to detect GC implementations that do not set it.
	// The thread with id == 0, is responsible for adding one in its
	// initalization
	os_atomic32_set(&gc->gc_active_threads, -1);

	os_atomic32_zero(&gc->exit);
	for (i = 0; i < gc->thread_nr; ++i)
		os_atomic32_set(&gc->gc_done[i], 1);
	for (i = 0; i < gc->thread_nr; ++i) {
		err = os_init_thread(gc, gc->fns->run,
			&gc->gc[i], &gc->gc_done[i], i, "gc");
		ERR_CHK_PRNT_GOTO(0 != err, fail0,
				"os_init_thread failed with %d", err);
	}
	assert(0 == err);
	return 0;
fail0:
	gc_exit_threads(gc);
	return err;
}

int gc_exit_threads(struct gc *const gc)
{
	int err = 0;
	int done = 1;
	ERR_CHK_SET_GOTO(NULL == gc, err, EINVAL, out0);

	os_atomic32_set(&gc->exit, 1);
	os_compiler_fence();
	/* wait for gc threads to join */
	os_wakeup_all(&gc->low_watermark_waitq);
	do {
		int i;
		done = 1;
		for (i = 0; i < gc->thread_nr; ++i) {
			if (0 == os_atomic32_read(&gc->gc_done[i])) {
				done = 0;
				break;
			}
		}
		if (!done)
			os_msleep(1);
	} while(!done);
out0:
	return err;
}

int gc_dtr(struct gc *gc)
{
	int err = 0;
	ERR_CHK_SET_GOTO(NULL == gc, err, EINVAL, out0);

	gc->fns->dtr(gc);
out0:
	return err;
}

void gc_reset_stats(struct gc *const gc)
{
	gc->fns->init_stats(gc);
}

u32 gc_get_bin_nr(const struct gc *const gc)
{
	return gc->fns->get_bin_nr(gc);
}

void gc_seg_destaged(
	struct gc      *const gc,
	struct segment *const seg)
{
	os_atomic64_inc(&gc->valid_seg_nr);
	if (!segment_is_reloc(seg))
		gc->fns->seg_write(gc, seg);
	else
		gc->fns->seg_relocate(gc, seg);
	DBG("GC:Seg %p %"PRIu64" val=%u destaged",
		seg, scm_get_seg_idx(gc->scm, seg), segment_valid_nr(seg));
}

void gc_seg_relocated(
	struct gc      *const gc,
	struct segment *const seg)
{
	os_atomic64_inc(&gc->valid_seg_nr);
	segment_set_reloc(seg);
	gc->fns->seg_relocate(gc, seg);
	DBG("GC:Seg %p %"PRIu64" val=%u relocated",
		seg, scm_get_seg_idx(gc->scm, seg), segment_valid_nr(seg));
}

void gc_page_invalidate(
	struct gc      *const gc,
	struct segment *const seg,
	const u32             grain_nr)
{
	/* os_atomic64_add(&gc->inval_page_nr, grain_nr); */
	DBG("GC:Seg %p %"PRIu64" val=%u page invalidate",
		seg, scm_get_seg_idx(gc->scm, seg), segment_valid_nr(seg));
	gc->fns->page_invalidate(gc, seg, grain_nr);
}

void gc_page_invalidate_reloc(
	struct gc      *const gc,
	struct segment *const seg,
	const u32             grain_nr)
{
	/* os_atomic64_add(&gc->inval_page_nr, grain_nr); */
	segment_putn(seg, grain_nr);
 	DBG("GC:Seg %p %"PRIu64" val=%u reloc page inval",
		seg, scm_get_seg_idx(gc->scm, seg), segment_valid_nr(seg));
}

void gc_seg_selected(
	struct gc *const gc,
	struct segment *seg)
{
	const u64 seg_size = scm_get_seg_size(gc->scm);
	const u64 seg_invalid = seg_size - segment_valid_nr(seg);
	scm_throttle_update(gc->scm, seg_invalid);
}

void gc_seg_released(
	struct gc       *const gc,
	struct gc_queue *const fifo,
	const u32              relocs)
{
	/* const u32 seg_size = gc->seg_size; */
	assert(0 < os_atomic64_read(&gc->valid_seg_nr));
	os_atomic64_dec(&gc->valid_seg_nr);
	/* assert(seg_size <= os_atomic64_read(&gc->inval_page_nr)); */
	/* os_atomic64_sub(&gc->inval_page_nr, seg_size); */
	gc_queue_add_seg_reloc(fifo);
}

void gc_print_list_info(struct gc *const gc)
{
	MSG("N DIRTY SEGS=%lu", os_atomic64_read(&gc->valid_seg_nr));
	gc->fns->print_list_size(gc);
}

void gc_print_info(const struct gc *const gc)
{
	return gc->fns->print_info(gc);
}

void gc_check_free_segment_low_watermark(
	struct gc *const gc,
	const u64        free_seg_nr)
{
	if (free_seg_nr <= gc->LOW_WATERMARK)
		os_wakeup_all(&gc->low_watermark_waitq);
}

int gc_register_ctlr(
	struct gc *const    gc,
	const u8       ctlr_id,
	const gc_reloc_fn_t reloc_fn,
	struct scm_seg_queue_set qset)
{
	const struct gc_ctl_data *ctl_data;
	int err = 0;

	const struct sto_ctlr *const sc = scm_get_ctlr(gc->scm, ctlr_id);
	/* check whether the ctlr_id is valid */
	if (SALSA_MAX_CTLRS < ctlr_id || NULL == sc) {
		err = EINVAL;
		goto out0;
	}

	/* register controller to controller table */
	ctl_data = &gc->ctl_data[ctlr_id];
	assert(NULL == ctl_data->sc_reloc_fn);
	((struct gc_ctl_data *)ctl_data)->sc_reloc_fn = reloc_fn;
	((struct gc_ctl_data *)ctl_data)->sc = (struct sto_ctlr *)sc;
	((struct gc_ctl_data *)ctl_data)->sc_qset = qset;

	// nio suggested restarting the GC here (scm_suspend_gc/scm_resume_gc),
	// but because these functions can fail, here's a more optimistic
	// version where we just wakeup the gc threads so that they recheck
	// their conditions. --KOU
	os_wakeup_all(&gc->low_watermark_waitq);
out0:
	return err;
}

int gc_unregister_ctlr(
	struct gc *const    gc,
	const u8       ctlr_id)
{
	int err = 0;

	// There are controllers that are not sto_ctlr for which the assertion
	// below will break
	#if 0
	const struct sto_ctlr *const sc = scm_get_ctlr(gc->scm, ctlr_id);
	/* check whether the ctlr_id is valid */
	if (SALSA_MAX_CTLRS < ctlr_id || NULL == sc) {
		err = EINVAL;
		goto out0;
	}
	#endif

	/* unregister fn from fn table */
	*(gc_reloc_fn_t *)&gc->ctl_data[ctlr_id].sc_reloc_fn = NULL;
	*(struct sto_ctlr **)&gc->ctl_data[ctlr_id].sc = NULL;
//out0:
	return err;
}

int gc_sto_ctlr_pre_dtr(
	struct gc *const gc,
	const u8    ctlr_id)
{
	return gc->fns->sto_ctlr_pre_dtr(gc, ctlr_id);
}

__attribute__((pure))
u32 gc_high_wm_get(const struct gc *const gc)
{
	return gc->HIGH_WATERMARK;
}

__attribute__((pure))
bool gc_is_active(const struct gc *const gc)
{
	s32 gc_active_threads = os_atomic32_read(&gc->gc_active_threads);

	if (gc_active_threads == -1) {
		MSG("This GC does not set ->gc_active_threads. Dying ungracefully");
		BUG();
	}

	return (gc_active_threads != 0);

}

__attribute__((pure))
u32 gc_low_wm_get(const struct gc *const gc)
{
	return gc->LOW_WATERMARK;
}

__attribute__((pure))
u32 gc_get_type(const struct gc *const gc)
{
	return gc->gc_type;
}

// returns true if GC should start
bool
gc_check_start(const struct gc *gc)
{
	unsigned i, ctlr_nr;
	struct scm_seg_queues *seg_qs = &gc->scm->seg_qs;
	const u64 LOW_WATERMARK = gc->LOW_WATERMARK;

	ctlr_nr = os_atomic32_read(&gc->scm->ctlr_nr);
	for (i = 0; i < SALSA_MAX_CTLRS && 0 < ctlr_nr; ++i) {
		const struct gc_ctl_data *ctl_data = gc->ctl_data + i;
		u64 nsegs;
		if (ctl_data->sc == NULL)
			continue;
		ctlr_nr--;
		nsegs = scm_seg_queues_free_qset(seg_qs, ctl_data->sc_qset);
		if (nsegs <= LOW_WATERMARK) {
			DBG("GC should start for controller: %u (nsegs=%"PRIu64") (qbitset=%u)", i, nsegs, ctl_data->sc_qset.qm__);
			return true;
		}
	}

	return false;
}

bool
gc_check_critical(const struct gc *gc)
{
	unsigned i, ctlr_nr;
	struct scm_seg_queues *seg_qs = &gc->scm->seg_qs;
	const u64 LOW_WATERMARK = 1;

	ctlr_nr = os_atomic32_read(&gc->scm->ctlr_nr);
	for (i = 0; i < SALSA_MAX_CTLRS && 0 < ctlr_nr; ++i) {
		const struct gc_ctl_data *ctl_data = gc->ctl_data + i;
		u64 nsegs;
		if (ctl_data->sc == NULL)
			continue;
		ctlr_nr--;
		nsegs = scm_seg_queues_free_qset(seg_qs, ctl_data->sc_qset);
		if (nsegs <= LOW_WATERMARK) {
			DBG("GC is critical for controller: %u (nsegs=%"PRIu64") (qbitset=%u)", i, nsegs, ctl_data->sc_qset.qm__);
			return true;
		}
	}

	return false;
}

// returns true if GC should continue running (assuming it has started)
bool
gc_check_continue(const struct gc *gc)
{
	unsigned i, ctlr_nr;
	struct scm_seg_queues *seg_qs = &gc->scm->seg_qs;
	const u64 HIGH_WATERMARK = gc->HIGH_WATERMARK;

	ctlr_nr = os_atomic32_read(&gc->scm->ctlr_nr);
	for (i=0; i < SALSA_MAX_CTLRS && 0 < ctlr_nr; ++i) {
		const struct gc_ctl_data *ctl_data = gc->ctl_data + i;
		u64 nsegs;
		if (ctl_data->sc == NULL)
			continue;
		ctlr_nr--;
		nsegs = scm_seg_queues_free_qset(seg_qs, ctl_data->sc_qset);
		if (nsegs < HIGH_WATERMARK) {
			DBG("GC should continue for controller: %u (nsegs=%"PRIu64") (qbitset=%u)", i, nsegs, ctl_data->sc_qset.qm__);
			return true;
		}
	}

	return false;
}

void gc_precondition_start(struct gc *const gc, const u32 streams)
{
	*((u64 *)&gc->HIGH_WATERMARK) = gc->HIGH_WATERMARK + streams;
}

void gc_precondition_finish(struct gc *const gc, const u32 streams)
{
	BUG_ON(gc->HIGH_WATERMARK < streams || gc->HIGH_WATERMARK - streams < gc->LOW_WATERMARK);
	*((u64 *)&gc->HIGH_WATERMARK) = gc->HIGH_WATERMARK - streams;
}
