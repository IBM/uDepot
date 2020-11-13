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

#include <asm/errno.h>

#ifndef	_SALSA_STO_CAPACITY_MGR_COMMON_H_
#define	_SALSA_STO_CAPACITY_MGR_COMMON_H_
#include <stdint.h>
#include <stdbool.h>

#include "data-structures/cds-queue.h"
#include "gc/gc.h"
#include "libos/os-atomic.h"
#include "libos/os-debug.h"
#include "libos/os-lock.h"
#include "libos/os-time.h"
#include "sto-ctlr/private/sto-segment.h"
#include "sto-ctlr/sto-capacity-mgr-parameters.h"
#include "sto-ctlr/scm-seg-queues.h"

struct sto_ctlr;

#define	SALSA_MAX_CTLRS	((1U << SALSA_SEGMENT_CTLR_BITS) - 1U)
#define	SALSA_MAX_STREAMS	((1U << SALSA_SEGMENT_STREAM_BITS))
#define	SALSA_MAX_GENERATIONS	(SALSA_MAX_STREAMS / 2)

#define	SALSA_SEG_FREE		0
#define	SALSA_SEG_FREE_REL	1

#define	SALSA_MIN_REL_SEGMENTS	1U

#define LBA_PBA_BITS	(64 - 1)
#define PBA_INVAL_ENTRY	((u64)(-1))
#define	PAGE_SIZE	4096UL

struct scm_throttler {
	os_atomic64_t       ua_nr;      /* user allowed number */
	os_atomic64_t       u;          /* number of user writes before having to throttle */
	os_atomic64_t       ga_nr;      /* gc allowed number */
	os_atomic64_t       g;          /* number of gc writes before allowing more user writes */
	os_waitqueue_head_t throttle_waitq;
};


struct sto_capacity_mgr {
	os_atomic32_t                ctlr_nr; /* # of ctrls */
	os_atomic32_t                exit;	  /* for shutdown */
	os_atomic64_t                thin_prov_segs; /* # available segments for
						    thin provisioned devices */
	struct gc             *const gc;
	struct scm_seg_queues        seg_qs;
	const u64                    total_physical_capacity_raw; /* bytes */
	const u64                    seg_nr; /* # segments */
	const u64                    seg_size; /* # grains */
	const u64                    grain_nr; /* # grains */
	const u64                    rmap_nr; /* # grains */
	const u32                    rmap_size; /* # grains */
	const u32                    grain_size; /* bytes */
	const u32                    queue_max_hw_grains;
	const u16                    dev_nr; /* #devs */
	const enum scm_raid_mode     raid;
	const bool                   simulation;
	os_atomic32_t                waiting_nr;	  /* for throttling */
	os_mutex_t                   mtx; /* protect updates to the ctlrs table */
	struct scm_throttler         scmt;
	const void           *const ctlrs[SALSA_MAX_CTLRS];
	volatile u32                *rmap_priv; /* 32bits for each reverse map entry */

	struct scm_dev_properties    scm_dev_props;

	#define SCM_NSTATS 8
	os_atomic64_t prop_stats[SCM_DEV_RANGES_MAX][SCM_NSTATS];

	struct segment               segs[]; /* Segment table */
} __attribute__((aligned));


static inline unsigned long
scm_seg_id(struct sto_capacity_mgr *scm, struct segment *seg)
{
	return (seg - scm->segs);
}

/**
 * handling of free segments
 */

// It would probably make sense to add a gfs_flags argument here for more
// fine-grained reporting
static inline u64
scm_get_free_seg_nr(struct sto_capacity_mgr *scm)
{
	return scm_seg_queues_free_total(&scm->seg_qs);
}

/**
 * register, unregister storage cotrollers
 *
 * @reserved_segs should be computed with function provided below
 */
int scm_register_sto_ctlr(
	struct sto_capacity_mgr *const scm,
	void                    *const sc,
	u8                 *const ctlr_id_out);
void scm_unregister_sto_ctlr(
	struct sto_capacity_mgr *const scm,
	void                    *const sc);

int scm_reserve_segments(
	struct sto_capacity_mgr *const scm,
	struct sto_ctlr             *const sc,
	const u64                 seg_nr);
int scm_suspend_gc(struct sto_capacity_mgr *const scm);
int scm_resume_gc(struct sto_capacity_mgr *const scm);
int scm_sto_ctlr_pre_dtr(
	struct sto_capacity_mgr *const scm,
	struct sto_ctlr             *const sc,
	const u8                  ctlr_id);
int scm_release_segments(
	struct sto_capacity_mgr *const scm,
	struct sto_ctlr             *const sc,
	const u64                 seg_nr);

/**
 * Auxiliary functions
 */
u64 scm_compute_reserved_segs(
	const u64 logical_segs,
	const u64 overpro,
	const bool     thin_provisioned);

__attribute__((pure))
static inline u64 page_to_grain(
	const u64 page_id,
	const u64 grains_per_page)
{
	return page_id * grains_per_page;
}

__attribute__((pure))
static inline u64 grain_to_page(
	const u64 grain_id,
	const u64 grains_per_page)
{
	assert(0 < grains_per_page);
	return grain_id / grains_per_page;
}

__attribute__((pure))
static inline struct segment *scm_get_seg(
	struct sto_capacity_mgr *const scm,
	const u64                      seg_idx)
{
	assert(seg_idx < scm->seg_nr);
	return &scm->segs[seg_idx];
}

__attribute__((pure))
static inline u64 scm_get_seg_idx(
	const struct sto_capacity_mgr *const scm,
	const struct segment          *const seg)
{
	assert(&scm->segs[0] <= seg && seg < &scm->segs[scm->seg_nr]);
	return (seg - &scm->segs[0]);
}

__attribute__((pure))
static inline u64 scm_seg_to_grain(
	const struct sto_capacity_mgr *const scm,
	const struct segment          *const seg)
{
	const u64 seg_idx = scm_get_seg_idx(scm, seg);
	return seg_idx * scm->seg_size;
}

__attribute__((pure))
static inline u64 scm_get_seg_nr(
	const struct sto_capacity_mgr *const scm)
{
	return scm->seg_nr;
}

__attribute__((pure))
static inline struct segment *scm_grain_to_segment(
	struct sto_capacity_mgr *const scm,
	const u64                 grain_idx)
{
	DBG_ASSERT(grain_idx / scm->seg_size < scm->seg_nr);
	return &scm->segs[grain_idx / scm->seg_size];
}

__attribute__((pure))
static inline u64 scm_grain_to_seg_idx(
	struct sto_capacity_mgr *const scm,
	const u64                     grain_idx)
{
	DBG_ASSERT(grain_idx / scm->seg_size < scm->seg_nr);
	return grain_idx / scm->seg_size;
}

__attribute__((pure))
static inline u32 scm_get_grain_size(const struct sto_capacity_mgr *const scm)
{
	return scm->grain_size;
}

__attribute__((pure))
static inline u32 scm_get_rmap_size(const struct sto_capacity_mgr *const scm)
{
	return scm->rmap_size;
}

__attribute__((pure))
static inline u64 scm_get_seg_size(const struct sto_capacity_mgr *const scm)
{
	return scm->seg_size;
}

__attribute__((pure))
static inline u64 scm_get_dev_size_raw(const struct sto_capacity_mgr *const scm)
{
	return scm->total_physical_capacity_raw;
}

__attribute__((pure))
static inline u64 scm_get_dev_size(const struct sto_capacity_mgr *const scm)
{
	return scm_dev_props_size(&scm->scm_dev_props);
}

__attribute__((pure)) static inline u64
scm_get_dev_size_prop_set(const struct sto_capacity_mgr *const scm,
                          struct scm_prop_set set)
{
	return scm_dev_props_size_set(&scm->scm_dev_props, set);
}

__attribute__((pure))
static inline struct gc *scm_get_gc(const struct sto_capacity_mgr *const scm)
{
	assert(NULL != scm->gc);
	return scm->gc;
}

__attribute__((pure))
static inline bool scm_get_simulation(const struct sto_capacity_mgr *const scm)
{
	return scm->simulation;
}

__attribute__((pure))
static inline const void *scm_get_ctlr(
	const struct sto_capacity_mgr *const scm,
	const u8                             ctlr_id)
{
	assert(ctlr_id < SALSA_MAX_CTLRS);
	return scm->ctlrs[ctlr_id];
}

__attribute__((pure))
static inline u32 scm_rmap_priv_get(
	struct sto_capacity_mgr *const scm,
	const u64                       grain_idx)
{
	struct segment *const seg = scm_grain_to_segment(scm, grain_idx);
	assert(grain_idx < scm->grain_nr);
	if (NULL == seg->rmap)
		BUG();		/* should not come to this point */
	{
		const u64 grain_offset = grain_idx % scm->seg_size;
		const u64 rmap_idx = grain_offset / scm->rmap_size;
		return *(volatile u32 *)&seg->rmap[rmap_idx];
	}
}

static inline void scm_rmap_priv_set(
	struct sto_capacity_mgr *const scm,
	const u64                      grain_idx,
	const u32                      priv)
{
	struct segment *const seg = scm_grain_to_segment(scm, grain_idx);
	assert(grain_idx < scm->grain_nr);
	if (NULL == seg->rmap)
		return;
	{
		const u64 grain_offset = grain_idx % scm->seg_size;
		const u64 rmap_idx = grain_offset / scm->rmap_size;
		BUG_ON(scm->grain_nr <= grain_idx);
		*(volatile u32 *)&seg->rmap[rmap_idx] = priv;
	}
}

static inline void scm_rmap_priv_prefetch(
	struct sto_capacity_mgr *const scm,
	const u64                       grain_idx)
{
	struct segment *const seg = scm_grain_to_segment(scm, grain_idx);
	assert(grain_idx < scm->grain_nr);
	if (NULL == seg->rmap)
		return;
	{
		const u64 grain_offset = grain_idx % scm->seg_size;
		const u64 rmap_idx = grain_offset / scm->rmap_size;
	__builtin_prefetch((void *) &seg->rmap[rmap_idx],
			1 /* write */, 1 /* low locality*/);
	}
}

__attribute__((pure))
static inline enum scm_raid_mode scm_get_raid_mode(
	const struct sto_capacity_mgr *const scm)
{
	return scm->raid;
}

__attribute__((pure))
static inline u16 scm_get_dev_nr(const struct sto_capacity_mgr *const scm)
{
	return scm->dev_nr;
}

__attribute__((pure))
static inline u32 scm_get_queue_max_hw_grains(const struct sto_capacity_mgr *const scm)
{
	return scm->queue_max_hw_grains;
}

#if	0
static inline void scm_throttle(struct sto_capacity_mgr *const scm)
{
	/* have to force a limit when down to 1 segment, otherwise gc
	 * could starve */
#if	0
	while (scm_get_free_seg_nr(scm) <= 2)
		os_msleep(1);
#else
	u64 free;
	const u32 wait_nr = os_atomic32_inc_return(&scm->waiting_nr);
	const u64 gc_low_wm = gc_low_wm_get(scm->gc);
	while ((free = scm_get_free_seg_nr(scm)) <= 2)
		os_msleep(1);
	if (free <= gc_low_wm) {
		const u64 diff = gc_low_wm - free + 2;
		os_usleep(wait_nr * diff * diff * 3);
	}
	os_atomic32_dec(&scm->waiting_nr);
#endif
}
#endif

void scm_detach_free_seg(
	struct sto_capacity_mgr *scm,
	struct segment          *seg);

/**
 * Throttling public interface
 */

// initialize throttling variables (coutners, wait queue, etc.)
void scm_throttle_init(struct scm_throttler *scmt);

// shut down throttling (will wake up tasks blocked in the wait queue)
void scm_throttle_exit(struct scm_throttler *scmt);

// user throttling:
// Should be called by the controller before every user write. This function may
// block, and will return if the user is allowed to write wrt to the throttling
// policy
void scm_throttle_user_write(struct sto_capacity_mgr *scm);

// Should be called by the GC after rellocating @gc_nr pages. The GC will
// typically operate on page batches when relocating a segment. This is meant to
// be called for every batch.
void scm_throttle_gc_write_nr(
	struct sto_capacity_mgr *scm,
	u32                      gc_nr);

// Called by the GC every time a relocation was completed, with @relocs being
// the number of relocations for this particular segment.
void scm_throttle_update(struct sto_capacity_mgr *, u32 relocs);

// notify the throttler when the GC starts/stops
void scm_throttle_gc_stopped(struct sto_capacity_mgr *const scm);
void scm_throttle_gc_starts(struct sto_capacity_mgr *scm);

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
int scm_cfg_set_scm_props(struct scm_parameters *cfg, u64 target_len, char **err_str);

void scm_dev_props_stats_print(struct sto_capacity_mgr *scm);
void scm_dev_props_stats_init(struct sto_capacity_mgr *scm);


#endif	/* _SALSA_STO_CAPACITY_MGR_COMMON_H_ */
