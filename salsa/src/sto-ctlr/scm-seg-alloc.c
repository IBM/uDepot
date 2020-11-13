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

#include "sto-ctlr/scm-seg-alloc.h"
#include "sto-ctlr/private/sto-capacity-mgr-common.h"

// helper function to print allocation flags
void scm_do_print_gfs_flags(struct scm_gfs_flags flags, const char *callfn)
{
	unsigned i;
	MSG("scm_print_gfs_flags: caller:%s ", callfn);
	MSG("          gfs_flags: ");
	if (flags.reloc)
		MSG("reloc ");
	if (flags.thin_prov)
		MSG("thin ");
	if (flags.prop_hard)
		MSG("hard ");
	if (flags.no_blocking)
		MSG("no_blocking ");
	MSG(" ");

	MSG("included properties (preferred):");
	for (i=0; i<SCM_PROP_LAST; i++) {
		if (flags.prop_set_preferred.mask__ & (1<<i))
			MSG("%s ", scm_dev_prop_getname_short(i));
	}

	MSG("included properties (default):");
	for (i=0; i<SCM_PROP_LAST; i++) {
		if (flags.prop_set_default.mask__ & (1<<i))
			MSG("%s ", scm_dev_prop_getname_short(i));
	}
	MSG("\n");
}

int scm_get_segs_thin_prov(
	struct sto_capacity_mgr *const scm,
	const u64                 seg_nr)
{
	int err = 0;
	u64 free;
	u64 new_free;
	do {
		free = os_atomic64_read(&scm->thin_prov_segs);
		if (free < seg_nr) {
			err = ENOSPC;
			break;
		}
		new_free = free - seg_nr;
	} while (free != os_atomic64_cmpxchg(&scm->thin_prov_segs, free, new_free));
	return err;
}

void scm_put_segs_thin_prov(
	struct sto_capacity_mgr *const scm,
	const u64                 seg_nr)
{
	u64 old;
	u64 new;
	do {
		old = os_atomic64_read(&scm->thin_prov_segs);
		new = old + seg_nr;
	} while (old != os_atomic64_cmpxchg(&scm->thin_prov_segs, old, new));
}

/**
 * Allocate a new segment.
 *
 * @seg_out:  set to NULL if error is returned, otherwise set to the segment
 *
 * Returns:
 * 0:      success, and free segment in @seg_out
 * EAGAIN: try again
 * ENOSPC: ran out of physical space for thin provisioned devs
 *
 * flags:
 *  SCM_GFS_RELLOC     0: normal, 1: reallocation
 *  SCM_GFS_THIN_PROV  thin provisioning if set
 */
int
scm_get_free_seg(struct sto_capacity_mgr *const scm, struct scm_gfs_flags flags,
                 struct segment **seg_out)
{
	int err;
	struct cds_queue_base *freeq = NULL;
	os_waitqueue_head_t   *waitq;
	struct segment *seg;
	// decode flags
	u64 free_nr = 0;
	struct scm_seg_queues *seg_qs;

	//scm_print_gfs_flags(flags);

	seg = NULL;
	if (flags.reloc && flags.thin_prov) {
		ERR("Thin provisioning not allowed for rellocation\n");
		err = EINVAL;
		goto fail;
	}

	// thin provisioning: ensure remaining free segments are available
	if (flags.thin_prov && (err = scm_get_segs_thin_prov(scm, 1)) != 0)
			return err;


	// retry only if we gain EAGAIN from get_seg_queue() or an exit
	seg_qs = &scm->seg_qs;
	waitq = scm_seg_queues_get_wq(seg_qs, flags);

	os_wait_event_interruptible(*waitq,
		-EAGAIN != (err = scm_seg_queues_get_queue(seg_qs, flags, &freeq))
		|| flags.no_blocking
		|| 1 == os_atomic32_read(&scm->exit));

	if (err)
		goto fail_reclaim_thin;

	assert(freeq != NULL);
	seg = cds_queue_base_pop_head_ret_cnt(freeq, struct segment, list, &free_nr);
	if (seg == NULL) {
		err = EAGAIN;
		goto fail_reclaim_thin;
	}

	// segment allocation OK: trigger gc if needed
	if (!flags.reloc)
		gc_check_free_segment_low_watermark(scm->gc, free_nr);
	*seg_out = seg;
	DBG("segment allocated: segid: %lu queueid:%u reloc:%u flags_free_seg_nr:%"PRIu64" total_free_seg_nr:%"PRIu64" total_free_rel_seg_nr:%"PRIu64"",
		seg - scm->segs, seg->queue_id, flags.reloc,
		scm_seg_queues_free(seg_qs, flags),
		scm_seg_queues_free_total(seg_qs),
		scm_seg_queues_reloc_free_total(seg_qs));
	#if defined(DEBUG)
	scm_print_gfs_flags(flags);
	#endif

	if (1 == scm->dev_nr || SALSA_LINEAR == scm->raid) {
		struct scm_dev_range_prop *rp;
		const u64 seg_idx = seg - scm->segs;
		const u64 seg_len = scm->seg_size*scm->grain_size; /* bytes */
		const u64 seg_off = seg_idx*seg_len; /* bytes */

		BUG_ON(seg < scm->segs || scm->seg_nr <= seg_idx);
		rp = scm_dev_props_find(&scm->scm_dev_props, seg_off);
		BUG_ON(NULL == rp);
		if (rp->r_seg_alloc_cb)
			err = rp->r_seg_alloc_cb(seg_off, seg_len, rp);
		if (err)
			goto fail_put;
	} else {
		/* TODO: support for RAID setups
		 * we need to use vpba_to_pba_and_dev or similar here */
	}

	return 0;

fail_put:
	scm_put_free_seg(scm, seg, flags.thin_prov);
fail_reclaim_thin:
	if (flags.thin_prov) // reclaim segments for thin provisioning
		scm_put_segs_thin_prov(scm, 1);
fail:
	*seg_out = NULL;
	return err;
}

void scm_put_free_seg(
	struct sto_capacity_mgr *const scm,
	struct segment          *const seg,
	bool thin_prov)
{
	u32 qid;
	struct cds_queue_base *freeq, *freeq_rel;
	bool is_reloc __attribute__((unused));

	assert(0 == segment_valid_nr(seg));
	qid = seg->queue_id;
	freeq      = &scm->seg_qs.qs[qid].q_segs;
	freeq_rel  = &scm->seg_qs.qs[qid].q_rel_segs;

	segment_reset(seg, scm->seg_size);
	cds_queue_base_lock(freeq_rel);
	if (cds_queue_base_size(freeq_rel) < SALSA_MIN_REL_SEGMENTS) {
		segment_set_state(seg, SEG_FREE_REL);
		cds_queue_base_enqueue_tail_bare(freeq_rel, &seg->list);
		cds_queue_base_unlock(freeq_rel);
		os_wakeup_all(&scm->seg_qs.wq_rel_segs);
		is_reloc = true;
	} else {
		u64 free __attribute__((unused));
		segment_set_state(seg, SEG_FREE);
		cds_queue_base_unlock(freeq_rel);
		free = cds_queue_base_enqueue_tail_ret_cnt(freeq, &seg->list);
		os_wakeup_all(&scm->seg_qs.wq_segs);
		is_reloc = false;
	}

	DBG("segment freed: segid: %lu queueid:%u reloc:%u free_seg_nr:%"PRIu64" free_rel_seg_nr:%"PRIu64,
		seg - scm->segs, seg->queue_id, is_reloc,
		scm_seg_queues_free_total(&scm->seg_qs),
		scm_seg_queues_reloc_free_total(&scm->seg_qs));

	if (thin_prov)
		scm_put_segs_thin_prov(scm, 1);
}
