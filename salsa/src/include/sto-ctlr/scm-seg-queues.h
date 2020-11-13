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

#ifndef	_SALSA_SEG_QUEUES_H_
#define	_SALSA_SEG_QUEUES_H_

#include "data-structures/cds-queue.h"
#include "sto-ctlr/private/sto-segment.h"
#include "sto-ctlr/scm-dev-properties.h"
#include "sto-ctlr/scm-gfs-flags.h"

struct sto_capacity_mgr;

#include <limits.h> // ULONG_MAX

/**
 * Segment queue
 * Segments are allocated from queues. Queues segregate segments based on
 * properties as described by @p_prop_mask:
 *   bit x in q_prop_mask is set => queue has segments with property x
 */
struct scm_seg_queue {
	// list of free segments (normal, relocation)
	struct cds_queue_base q_segs, q_rel_segs;
	// We record the maximum number of segments in the queue. This is used
	// in garbage collection, to distinguish between queues that are empty
	// and queues that had never had segments.
	u64                   q_nsegs_max, q_rel_nsegs_max;
        // set of segment properties for this queue
	struct scm_prop_set   q_prop_set;
};

/**
 * struct segment has a ->queue_id bit field of size SALSA_SEGMENT_QUEUEID_BITS
 * (currently 7) that we can use to track the free queue for each segment.
 */
#define SCM_MAX_QUEUES 16
STATIC_ASSERT(SCM_MAX_QUEUES <= (1<<SALSA_SEGMENT_QUEUEID_BITS), too_many_scm_max_queues);

/**
 * Segment queues
 * We use a single waitqueue (one for relocation and one for normal requests)
 * for all queues. The use will sleep into one of the queue and check the
 * availability of queues as they wakeup.
 */
struct scm_seg_queues {
	struct scm_seg_queue         qs[SCM_MAX_QUEUES];
	u32                          qs_nr;
	os_waitqueue_head_t          wq_segs;
	os_waitqueue_head_t          wq_rel_segs;
};

/**
 * queue sets
 */
struct scm_seg_queue_set {
	uint16_t qm__; // queue mask
} __attribute__((packed));
STATIC_ASSERT(SCM_MAX_QUEUES <= 16, scm_seg_queue_set_mask_too_small);

#define SCM_SEG_QUEUE_SET_EMPTY ((struct scm_seg_queue_set){.qm__ = 0x0})

static inline bool
scm_seg_queue_set_is_empty(struct scm_seg_queue_set qs) {
	return qs.qm__ == SCM_SEG_QUEUE_SET_EMPTY.qm__;
}

// initialize segments on scm (put segments into queues)
int scm_seg_queues_init(struct sto_capacity_mgr *scm,
                        const u16 *qs_prop_masks, size_t qs_nr,
                        struct scm_dev_properties *d_props);

void
scm_seg_queues_init_mirror(struct scm_seg_queues *dst,
                           const struct scm_seg_queues *src);

// initialize queues configuration
enum scm_seg_queue_conf {
	SCM_SEG_QUEUES_ONE   = 0x0, // one queue for all segments
	SCM_SEG_QUEUES_ALL   = 0x1, // one queue per property
};

// initialize queues based on configuration
int scm_seg_queues_init_conf(struct sto_capacity_mgr *scm, unsigned conf);

// helper: initialize single queue (i.e., one for reloc and one for free)
int
scm_seg_queues_init_singleq(struct sto_capacity_mgr *scm,
                            struct scm_dev_properties *d_props);

// wake up all waiters
// XXX: maybe we can avoid this?
static inline void
scm_seg_queues_wakup_all(struct scm_seg_queues *seg_qs)
{
	os_wakeup_all(&seg_qs->wq_segs);
	os_wakeup_all(&seg_qs->wq_rel_segs);
}


// returns the first queue that matches a property (or NULL)
static inline struct scm_seg_queue *
scm_prop_find_queue(struct scm_seg_queues *seg_qs, enum scm_property p)
{
	struct scm_seg_queue *q;
	u32 i;

	for (i=0; i < seg_qs->qs_nr; i++) {
		q = seg_qs->qs + i;
		if (scm_prop_set_contains(q->q_prop_set, p))
			return q;
	}

	return NULL;
}

__attribute__((pure)) static inline u64
scm_seg_queues_free_total(struct scm_seg_queues *seg_qs)
{
	u32 i;
	u64 ret = 0;

	for (i=0; i < seg_qs->qs_nr; i++)
		ret += cds_queue_base_size(&seg_qs->qs[i].q_segs);

	return ret;
}

__attribute__((pure)) static inline struct scm_seg_queue_set
scm_seg_qset_from_pset(struct scm_seg_queues *seg_qs, struct scm_prop_set pset)
{
	u32 i;
	struct scm_seg_queue_set ret = (struct scm_seg_queue_set){.qm__ = 0};

	for (i=0; i <seg_qs->qs_nr; i++) {
		struct scm_seg_queue *queue = &seg_qs->qs[i];
		if (scm_prop_set_is_subset_of(queue->q_prop_set, pset)) {
			ret.qm__ |= (1<<i);
		}
	}

	return ret;
}


// return number of free segments based on allocation (gfs) flags
__attribute__((pure)) static inline u64
scm_seg_queues_free(struct scm_seg_queues *seg_qs, struct scm_gfs_flags flags)
{
	struct cds_queue_base *q;
	u32 i;
	u64 ret = 0;

	for (i=0; i <seg_qs->qs_nr; i++) {
		struct scm_seg_queue *queue = &seg_qs->qs[i];
		if (!scm_prop_set_is_subset_of(queue->q_prop_set, flags.prop_set_default))
			continue;
		q = flags.reloc ? &queue->q_rel_segs : &queue->q_segs;
		ret += cds_queue_base_size(q);
	}

	return ret;
}

#if 0
// NB: This was used to determine if GC was supposed to run, but it does not
// work for cases where a controller uses multiple queues.
// See gc_check_start() gc_check_continue() for how we deal with this now.
__attribute__((pure)) static inline u64
scm_seg_queues_free_min(struct scm_seg_queues *seg_qs)
{
	u32 i;
	u64 min = ULONG_MAX;

	for (i=0; i <seg_qs->qs_nr; i++) {
		struct scm_seg_queue *q;
		q = seg_qs->qs + i;

		// ignore queues that never had any segments in them
		if (q->q_nsegs_max == 0)
			continue;

		min = min2(cds_queue_base_size(&q->q_segs), min);
	}

	return min;
}
#endif

// return number of free segments in queues that match the given queue set
__attribute__((pure)) static inline u64
scm_seg_queues_free_qset(struct scm_seg_queues *seg_qs, struct scm_seg_queue_set qset)
{
	u32 i;
	u64 ret = 0;

	for (i=0; i < seg_qs->qs_nr; i++)
		if (qset.qm__ & (1<<i))
			ret += cds_queue_base_size(&seg_qs->qs[i].q_segs);

	return ret;
}

__attribute__((pure)) static inline u64
scm_seg_queues_reloc_free_total(struct scm_seg_queues *seg_qs)
{
	u32 i;
	u64 ret = 0;

	for (i=0; i < seg_qs->qs_nr; i++)
		ret += cds_queue_base_size(&seg_qs->qs[i].q_rel_segs);

	return ret;
}


// This was used when there was only a single queue. For now, keep backwards
// compatiblity when we can and issue a BUG() otherwise.
__attribute__((pure)) static inline os_atomic64_t *
scm_seg_queues_get_free_ref(struct scm_seg_queues *seg_qs)
{
	if (seg_qs->qs_nr == 1)
		return cds_queue_base_size_ref(&seg_qs->qs[0].q_segs);

	BUG();
	return NULL;
}

/**
 * Find the proper queue, given an allocation mask (@gfs_flags)
 *
 * returns 0       and sets @q_ret if a proper queue was find
 * returns -EAGAIN if one of the matching queues was empty
 * returns -EINVAL if no matching queue was found. This is most likely a bug.
 * if return is not 0, *q_ret is set to NULL
 */
int
scm_seg_queues_get_queue(struct scm_seg_queues *seq_qs,
                         struct scm_gfs_flags flags,
                         struct cds_queue_base **q_ret);

/**
 * Returns the waitqueue a process might sleep based on its allocation flags
 * (@gfs)
 */
static inline os_waitqueue_head_t *
scm_seg_queues_get_wq(struct scm_seg_queues *seg_qs, struct scm_gfs_flags gfs)
{
	os_waitqueue_head_t *waitq;
	waitq = gfs.reloc ? &seg_qs->wq_rel_segs : &seg_qs->wq_segs;
	return waitq;
}

// Return the property sets of queues (@qs_prop_sets) that match the given
// property set @prop_set.
void
scm_seg_queues_avail_props(const struct scm_seg_queues *seg_qs,
                           struct scm_prop_set prop_set,
                           #if defined(__cplusplus)
                           struct scm_prop_set *qs_prop_sets,
                           #else
                           struct scm_prop_set qs_prop_sets[static SCM_MAX_QUEUES],
                           #endif
                           u32 *qs_prop_sets_nr,
                           struct scm_seg_queue_set *qset);

/**
 * We have a set of streams and we want to map them to different queue property
 * sets, so that we can create allocation flags for these queues. This function
 * maps a stream into queues and returns the property set of the queue.
 *
 * Queue masks can be obtained via scm_seg_queues_avail()
 */
void
scm_seg_queues_map_to_streams(const struct scm_prop_set *qs_props,
                              const u64                 *qs_size,
                              u32 qs_nr, u16 streams_nr,
                              struct scm_prop_set *stream_props,
                              u64                 *stream_size);

void scm_seg_queues_dump_stats(const struct scm_seg_queues *qs);

#endif // _SALSA_SEG_QUEUES_H_
