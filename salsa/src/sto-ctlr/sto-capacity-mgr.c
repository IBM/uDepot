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

#include "sto-ctlr/private/sto-capacity-mgr-common.h"

#include <asm/errno.h>

#include "gc/gc.h"
#include "libos/os-atomic.h"
#include "libos/os-debug.h"
#include "libos/os-lock.h"
#include "libos/os-malloc.h"
#include "libos/os-string.h"
#include "sto-ctlr/sto-ctlr.h"
#include "sto-ctlr/private/sto-ctlr-common.h"
#include "sto-ctlr/sto-capacity-mgr.h"
#include "sto-ctlr/scm-seg-alloc.h"
#include "sto-ctlr/sto-ctlr-parameters.h"
#include "sto-ctlr/sto-capacity-mgr-parameters.h"
#include "util/parse-storage-args.h"
/**
 * Static function definitions needed by external interfaces *
 *************************************************************/
static inline void **scm_find_ctlr(
	const struct sto_capacity_mgr *const scm,
	const struct sto_ctlr             *const sc);
static void scm_print_ctr_info(struct sto_capacity_mgr *const scm);
/**
 *          Public interfaces             *
 ******************************************/

int sto_capacity_mgr_ctr(
	struct scm_parameters    *const params,
	struct sto_capacity_mgr **const scm_out)
{
	int err = 0;
	u64 size;
	u64 grain_nr, rmap_nr;
	u64 seg_nr, real_seg_nr;
	u64 dev_size;
	struct sto_capacity_mgr *scm = NULL;
	/* check parameter validity */
	err = check_sto_capacity_mgr_parameters(params);
	ERR_CHK_PRNT_GOTO(0 != err, fail0,
			"check_sto_capacity_mgr_params failed with %d", err);

	dev_size = scm_dev_props_size(&params->cfg_scm_props);

	/* compute allocation size */
	seg_nr = scm_cfg_get_segs_nr(params);
	real_seg_nr = scm_cfg_get_real_segs_nr(params);
	dev_size = seg_nr * params->segment_size * params->grain_size;
	grain_nr = dev_size / params->grain_size;
	MSG("real_seg_nr: %"PRIu64, real_seg_nr);
	size = sizeof(*scm) + (sizeof(struct segment) * real_seg_nr);

	/* allocate scm structure */
	scm = os_vmalloc(size);
	ERR_CHK_SET_PRNT_GOTO(NULL == scm, err, ENOMEM, fail1,
			"failed to allocate scm.");
	memset(scm, 0, size);


	if (params->use_rmap) {
		rmap_nr = grain_nr / params->rmap_grain_size;
		scm->rmap_priv = os_vmalloc(sizeof(u32) * (rmap_nr + 1));
		ERR_CHK_SET_PRNT_GOTO(NULL == scm->rmap_priv, err, ENOMEM, fail2,
				"failed to allocate rmap_priv.");
		memset((void *)scm->rmap_priv, 0, sizeof(u32) * (rmap_nr + 1));
		assert(0 == ((uintptr_t) scm->rmap_priv & (sizeof(u64) - 1U)));
	} else {
		params->rmap_grain_size = 1;
		rmap_nr = grain_nr;
	}
	/* initialize */
	*(u64 *)&scm->seg_nr = seg_nr;
	*(u64 *)&scm->rmap_nr = rmap_nr;
	*(u64 *)&scm->seg_size = params->segment_size;
	*(u32 *)&scm->grain_size = params->grain_size;
	*(u32 *)&scm->rmap_size = params->rmap_grain_size;
	*(u32 *)&scm->queue_max_hw_grains = params->queue_max_hw_grains;
	*(u64 *)&scm->grain_nr = grain_nr;
	*(u64 *)&scm->total_physical_capacity_raw = params->dev_size_raw;
	*(bool *)&scm->simulation = params->simulation;
	*(enum scm_raid_mode *)&scm->raid = params->raid;
	*(u16 *)&scm->dev_nr = params->dev_nr;

	os_atomic32_zero(&scm->ctlr_nr);
	os_atomic32_zero(&scm->waiting_nr);
	os_atomic64_set(&scm->thin_prov_segs, seg_nr);
	os_mutex_init(&scm->mtx);
	DBG("segs=%lu", (uintptr_t)scm->segs);
	DBG("segment-size=%lu", sizeof(struct segment));
	DBG("seg-list=%lu", offsetof(struct segment, list));
	DBG("seg-hhead=%lu", offsetof(struct segment, hhead));
	DBG("seg-nr1=%lu", offsetof(struct segment, nr1));
	DBG("seg-nr2=%lu", offsetof(struct segment, nr2));
	DBG("seg-hlist=%lu", offsetof(struct segment, hlist));
	DBG("seg-valid-nr=%lu", offsetof(struct segment, valid_nr));
	DBG("seg-outs_read_nr=%lu", offsetof(struct segment, outs_read_nr));
	DBG("seg-ctlr_id=%lu", offsetof(struct segment, ctlr_id));
	// DBG("seg-is_reloc=%lu", offsetof(struct segment, is_reloc));
	DBG("seg-stream=%lu", offsetof(struct segment, stream));
	DBG("seg-private=%lu", offsetof(struct segment, priv));
	/* our overprovisioning is at least SALSA_MIN_REL_SEGMENTS */
	assert(SALSA_MIN_REL_SEGMENTS < os_atomic64_read(&scm->thin_prov_segs));

	scm_dev_props_copy_full(&scm->scm_dev_props, &params->cfg_scm_props);
	scm_dev_props_stats_init(scm);
	err = scm_seg_queues_init_conf(scm, params->seg_qs_conf);

	ERR_CHK_PRNT_GOTO(0 != err, fail2, "scm_seg_queues_init failed with %d", err);

	/* create the gc */
	params->gc_params.scm = scm;
	err = gc_ctr(&params->gc_params, scm, (struct gc **) &scm->gc);
	ERR_CHK_PRNT_GOTO(0 != err, fail3, "gc_ctr failed with %d", err);

	scm_throttle_init(&scm->scmt);

	scm_print_ctr_info(scm);

	if (params->use_rmap) {
		u64 i;
		for (i = 0; i < scm->seg_nr; ++i) {
			struct segment *const seg = &scm->segs[i];
			const u64 rmap_idx = i * scm->seg_size / scm->rmap_size;
			BUG_ON(scm->rmap_nr < rmap_idx);
			seg->rmap = &scm->rmap_priv[rmap_idx];
		}
	}

	assert(0 == err);
	*scm_out = scm;
	return 0;
fail3:
fail2:
	os_vfree(scm);
fail1:
fail0:
	return err;
}

int sto_capacity_mgr_dtr(struct sto_capacity_mgr *const scm)
{
	int err = 0;
	ERR_CHK_SET_GOTO(NULL == scm, err, EINVAL, fail0);
	if (0 != os_atomic32_read(&scm->ctlr_nr) ||
		0 == os_atomic32_read(&scm->exit)) {
		err = EPERM;
		ERR("Trying to destroy capacity manager before destroying" \
			" the storage ctlrs and/or calling the exit fn.");
		goto fail0;
	}
	err = gc_dtr(scm->gc);
	assert(0 == err);
	if (NULL != scm->rmap_priv)
		os_vfree((void *)scm->rmap_priv);
	os_vfree(scm);
fail0:
	return err;
}

int sto_capacity_mgr_init_threads(struct sto_capacity_mgr *const scm)
{
	int err = 0;
	os_atomic32_zero(&scm->exit);
	err = gc_init_threads(scm->gc);
	ERR_CHK_PRNT(0 != err, "gc_init_threads failed with %d", err);
	return err;
}

int sto_capacity_mgr_exit_threads(struct sto_capacity_mgr *const scm)
{
	int err;
	os_atomic32_set(&scm->exit, 1);
	scm_seg_queues_wakup_all(&scm->seg_qs);
	scm_throttle_exit(&scm->scmt);
	err = gc_exit_threads(scm->gc);
	assert(0 == err);
	return err;
}

int scm_register_sto_ctlr(
	struct sto_capacity_mgr *const scm,
	void                    *const sc,
	u8                      *const ctlr_id_out)
{
	int err = ENOSPC;
	const void **scp;
	const void **const scstart = (const void **) &scm->ctlrs[0];
	const void *const*const scend   = &scm->ctlrs[SALSA_MAX_CTLRS];
	/* lock and search the table and check that sc does not
	 * exist */
	os_mutex_lock(&scm->mtx);
	if (NULL != scm_find_ctlr(scm, sc)) {
			err = EEXIST;
			goto out0;
	}
	/* find first empty entry */
	for (scp = scstart; scp != scend; ++scp) {
		if (NULL != *scp)
			continue;
		/* insert it in the first empty location */
		*scp = sc;
		/* success, return unique id of table location */
		err = 0;
		*ctlr_id_out = scp - scstart;
		os_atomic32_inc(&scm->ctlr_nr);
		assert(os_atomic32_read(&scm->ctlr_nr) <= SALSA_MAX_CTLRS);
		break;
	}
out0:
	os_mutex_unlock(&scm->mtx);
	return err;
}

void scm_unregister_sto_ctlr(
	struct sto_capacity_mgr *const scm,
	void                    *const sc)
{
	void **scp;
	/* lock and search if it exists */
	os_mutex_lock(&scm->mtx);
	scp = scm_find_ctlr(scm, sc);
	assert(NULL != sc);
	if (NULL != scp) {
		// XXX: because we now have controllers that are not sto_ctlr,
		// the check below might fail. Disable it.
		#if 0
		/* retrieve ctlr_id from ctlr */
		const u8 ctlr_id __attribute__((unused)) = ctlr_get_id(sc);
		/* sanity check */
		assert(sc == scm->ctlrs[ctlr_id]);
		#endif
		*scp = NULL;	/* reset entry */
		assert(0 < os_atomic32_read(&scm->ctlr_nr));
		os_atomic32_dec(&scm->ctlr_nr);
	}
	os_mutex_unlock(&scm->mtx);
}

int scm_suspend_gc(struct sto_capacity_mgr *const scm)
{
	os_atomic32_set(&scm->exit, 1);
	scm_throttle_exit(&scm->scmt);
	scm_seg_queues_wakup_all(&scm->seg_qs);
	return gc_exit_threads(scm->gc);
}

int scm_resume_gc(struct sto_capacity_mgr *const scm)
{
	os_atomic32_set(&scm->exit, 0);
	return gc_init_threads(scm->gc);
}

/**
 * Has to be called before ctlr_dtr and after ctlr exit threads
 */
int scm_sto_ctlr_pre_dtr(
	struct sto_capacity_mgr *const scm,
	struct sto_ctlr         *const sc,
	const u8                       ctlr_id)
{
	int err;
	assert(sc == scm->ctlrs[ctlr_id]);
	/* return all segs of ctlr_id to the free queue */
	err = gc_sto_ctlr_pre_dtr(scm->gc, ctlr_id);
	ERR_CHK_PRNT_GOTO(0 != err, fail0, "gc_sto_ctlr_pre_dtr failed");
fail0:
	return err;
}

int scm_reserve_segments(
	struct sto_capacity_mgr *const scm,
	struct sto_ctlr         *const sc,
	const u64                      seg_nr)
{
	int err = 0;
	const struct sto_ctlr **const scp =
		(const struct sto_ctlr **)scm_find_ctlr(scm, sc);
	/* verify that the ctlr exists in our table */
	if (NULL == scp) {
		err = ENOENT;
		goto out0;
	}
	/* try to reserve requested number of segments */
	err = scm_get_segs_thin_prov(scm, seg_nr);
	ERR_CHK_PRNT(0 != err, "Unable to reserve %"PRIu64" segments.", seg_nr);
	DBG("reserve %"PRIu64" rc=%d", seg_nr, err);
out0:
	return err;
}

int scm_release_segments(
	struct sto_capacity_mgr *const scm,
	struct sto_ctlr             *const sc,
	const u64                 seg_nr)
{
	int err = 0;
	const struct sto_ctlr **const scp =
		(const struct sto_ctlr **) scm_find_ctlr(scm,sc);
	/* verify that the ctlr exists in our table */
	if (NULL == scp) {
		err = ENOENT;
		goto out0;
	}
	scm_put_segs_thin_prov(scm, seg_nr);
out0:
	return err;
}

void scm_detach_free_seg(
	struct sto_capacity_mgr *const scm,
	struct segment          *const seg)
{
	struct scm_seg_queue *q;
	u32 qid = seg->queue_id;
	q = scm->seg_qs.qs + qid;

	if (SEG_FREE == segment_get_state(seg))
		cds_queue_base_del(&q->q_segs, &seg->list);
	else {
		assert(SEG_FREE_REL == segment_get_state(seg));
		cds_queue_base_del(&q->q_rel_segs, &seg->list);
	}
}

u64 scm_compute_reserved_segs(
	const u64  logical_segs,
	const u64  overpro,
	const bool thin_provisioned);


/**
 * Procedures used inside this file alone *
 ******************************************/

static void **scm_find_ctlr(
	const struct sto_capacity_mgr *const scm,
	const struct sto_ctlr             *const sc)
{
	void **scp;
	void **scstart = (void **)&scm->ctlrs[0];
	void **scend   = (void **)&scm->ctlrs[SALSA_MAX_CTLRS];
	for (scp = scstart; scp != scend; ++scp)
		if (*scp == sc)
			return scp;
	return NULL;
}

static void scm_print_ctr_info(struct sto_capacity_mgr *const scm)
{
	u64 total_capacity __attribute__((unused)) = scm_get_dev_size(scm);

	MSG("-------------------------------------");
	MSG("-------------SCM INIT----------------");
	MSG("GRAIN SIZE        =%u", scm->grain_size);
	MSG("SEGMENT SIZE      =%"PRIu64, scm->seg_size);
	MSG("GRAIN NR          =%"PRIu64, scm->grain_nr);
	MSG("SEGMENT NR        =%"PRIu64, scm->seg_nr);
	MSG("RMAP SIZE         =%u grains", scm->rmap_size);
	MSG("RMAP NR           =%"PRIu64, scm->rmap_nr);
	MSG("RMAP TABLE        =%"PRIu64"MiB",
		NULL == scm->rmap_priv ?
		0 : (scm->rmap_nr * sizeof(*scm->rmap_priv)) >> 20ULL);
	MSG("TOTAL CAPACITY    =%"PRIu64"B", total_capacity);
	MSG("SIMULATION MODE   =%s", 0 == scm->simulation ? "NO" : "YES");
	MSG("-------------------------------------");
}

u64 sto_capacity_mgr_get_capacity(struct sto_capacity_mgr *const scm)
{
	return scm_get_dev_size(scm);
}

u64 sto_capacity_mgr_get_capacity_set(
	struct sto_capacity_mgr *const scm, struct scm_prop_set set)
{
	return scm_get_dev_size_prop_set(scm, set);
}

u64 sto_capacity_mgr_get_grain_size(struct sto_capacity_mgr *const scm)
{
	return scm->grain_size;
}


/**
 * @cfg contains:
 *  ->cfg_dev_props: properties of all underlying devices for the SCM
 *  ->cfg_scm_props: properties of the resulitng SCM device
 *
 * This function sets ->cfg_scm_props based on ->cfg_dev_props and other @cfg
 * values.
 *
 * @target_len is a user specified device length (if 0, it will be ignored)
 * @err_str    should be fiiled in case of an error
 *
 * Returns 0 if no error, -errval otherwise
 *
 * sets cfg->cfg_scm_props
 *      cfg->dev_size_raw;
 */
int
scm_cfg_set_scm_props(struct scm_parameters *cfg, u64 target_len, char **err_str)
{
	u64 raw_dev_sectors = 0, dev_sectors, dev_segments;
	const u64 sector_size  = SEC2B(1);
	const u64 segment_size = cfg->segment_size*cfg->grain_size;
	const u64 seg_sectors  = B2SEC(segment_size);
	unsigned dev_nr = cfg->dev_nr;
	const struct scm_dev_properties *dev_props = cfg->cfg_dev_props;
	struct scm_dev_properties *scm_props = &cfg->cfg_scm_props;
	if (0 == seg_sectors) {
		ERR("trying to set prop with a seg size of 0.");
		return -EINVAL;
	}
	if (dev_nr == 1) { // NB: we can probably merge this with linear
		u64 dev_bytes;
		raw_dev_sectors = scm_dev_props_size_grain(dev_props + 0, sector_size);
		if (target_len)
			raw_dev_sectors = min2(raw_dev_sectors, target_len);
		dev_sectors = (raw_dev_sectors / seg_sectors) * seg_sectors;
		dev_bytes = SEC2B(dev_sectors);
		scm_dev_props_copy(scm_props, dev_props + 0, dev_bytes);
	} else if (cfg->raid == SALSA_LINEAR) {
		unsigned i;
		// append all individual dev properties to build a linear one
		for (i=0; i<dev_nr; i++)
			scm_dev_props_append(scm_props, dev_props + i);
		// determine the raw size of the device
		raw_dev_sectors = scm_dev_props_size_grain(scm_props, sector_size);
		// truncate the device to the specified target_len
		if (target_len)
			scm_dev_props_truncate_grain(scm_props, sector_size, target_len);
		// determine the number of usable sectors in the device
		dev_segments = scm_dev_props_size_grain(scm_props, segment_size);
		dev_sectors = dev_segments*seg_sectors;
	} else if (cfg->raid == RAID0 || cfg->raid == RAID5) {
		unsigned raid_n, i;
		u64 min_capacity, min_raw_capacity;
		// raid_n: number of effective devices
		raid_n = RAID5 == cfg->raid ? dev_nr-1 : dev_nr;
		if (0 != cfg->segment_size % raid_n) {
			ERR("segment size does not align to number of raid devs.");
			*err_str = "Invalid segment size parameter.";
			return -EINVAL;
		}
		min_raw_capacity = scm_dev_props_size_grain(dev_props + 0, sector_size);
		for (i=1; i<dev_nr; i++) {
			u64 c = scm_dev_props_size_grain(dev_props + i, sector_size);
			if (c < min_raw_capacity)
				min_capacity = c;
		}
		min_capacity     = (min_raw_capacity / seg_sectors)*seg_sectors;
		raw_dev_sectors  = raid_n*min_raw_capacity;
		dev_sectors      = raid_n*min_capacity;
		// for now, use a single property range for multi-device setups
		scm_dev_props_set_default(scm_props, SEC2B(dev_sectors));
	} else {
		BUG_ON(1 && "Unknown cfg->raid");
	}
	MSG("SCM Device properties:");
	scm_dev_props_print(scm_props, "  ");
	/* raw capacity should always be the underlying device's
	 * capacity */
	cfg->dev_size_raw = SEC2B(raw_dev_sectors);
	if (scm_dev_props_size(scm_props) != SEC2B(dev_sectors)) {
		MSG("Different Capacities:");
		MSG("props_size:    %"PRIu64, scm_dev_props_size(scm_props));
		MSG("dev_size:      %lu", SEC2B(dev_sectors));
		MSG("raw_dev_size:  %lu", SEC2B(raw_dev_sectors));
	}
	return 0;
}

/* stats */

STATIC_ASSERT(SC_OP_NR__ <= SCM_NSTATS, scm_dev_range_nstats_too_small);

void
scm_dev_props_stats_init(struct sto_capacity_mgr *scm)
{
	unsigned i, j;

	for (i=0; i< scm->scm_dev_props.ranges_nr; i++) {
		for (j=0; j<SCM_NSTATS; j++) {
			os_atomic64_zero(&scm->prop_stats[i][j]);
		}
	}
}

void
scm_dev_props_stats_print(struct sto_capacity_mgr *scm)
{
	struct scm_dev_properties *ps = &scm->scm_dev_props;
	unsigned i, j;

	static const char *sc_op_name[] __attribute__((unused)) = {
		[SC_RD]                   = "RD",
		[SC_WR_USR]               = "WR_USR",
		[SC_WR_USR_UIP]           = "WR_USR_UIP",
		[SC_WR_USR_UNALIGNED]     = "WR_USR_UNALIGNED",
		[SC_WR_USR_UNALIGNED_UIP] = "WR_USR_UNALIGNED_UIP",
		[SC_WR_RELOC]             = "WR_RELOC",
	};

	//u64 total_i[ps->ranges_nr] = {0};
	u64 total_j[SC_OP_NR__]    = {0};
	u64 total_write = 0;

	//u64 total = 0;
	for (i=0; i < ps->ranges_nr; i++) {
		for (j=0; j<SC_OP_NR__; j++) {
			u64 x = os_atomic64_read(&scm->prop_stats[i][j]);
			//total_i[i] += x;
			total_j[j] += x;
			if (j != SC_RD)
				total_write += x;
		}
	}

	MSG("= SCM stats");
	for (i=0; i < ps->ranges_nr; i++) {
		struct scm_dev_range_prop *rp __attribute__((unused)) = ps->ranges + i;

		MSG("Range %u (%s)", i, scm_dev_prop_getname_short(rp->r_prop));
		for (j=0; j<SC_OP_NR__; j++) {
			const u64 c = os_atomic64_read(&scm->prop_stats[i][j]);
			const u64 p_j __attribute((unused)) = total_j[j] ? (c*100)/total_j[j] : 0;
			if (j == SC_RD)
				MSG("# %-25s=%"PRIu64 " (%"PRIu64 "%% of total %s accesses)",
					sc_op_name[j], c, p_j, sc_op_name[j]);
			else {
				const u64 p_w __attribute__((unused)) = total_j[j] ? (c*100)/total_write : 0;
				MSG("# %-25s=%"PRIu64 " (%"PRIu64 "%% of total %s accesses and %"PRIu64"%% of total write accesses)",
					sc_op_name[j], c, p_j, sc_op_name[j], p_w);
			}
		}
	}
}

void scm_dump_stats(const struct sto_capacity_mgr *scm)
{
	scm_seg_queues_dump_stats(&scm->seg_qs);
	scm_dev_props_stats_print((struct sto_capacity_mgr *)scm);
}

void scm_reset_stats(struct sto_capacity_mgr *scm)
{
	scm_dev_props_stats_init(scm);
}
