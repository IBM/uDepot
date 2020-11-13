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

#include "sto-ctlr/scm-dev-properties.h"
#include "sto-ctlr/scm-seg-queues.h"
#include "sto-ctlr/private/sto-capacity-mgr-common.h"


// check the validity of qs_prop_masks: we want qs_prop_masks to form a
// partition of the set of all properties every property is included in *one*
// and *only one* of the masks. The *only one* part might be too strong, but
// potentially simplifies some things. We could also deal with the *one* part by
// defining another queue, but that's probably being too smart.
static int
verify_masks(const u16 *qs_prop_masks, size_t qs_nr)
{
	u16 m;
	u32 i;

	if (qs_nr > SCM_MAX_QUEUES)
		return -EINVAL;
	m = 0x0;
	for (i=0; i<qs_nr; i++) {
		if (m & qs_prop_masks[i]) // a property was set before, "only one" part failed
			return -EINVAL;
		m |= qs_prop_masks[i];
	}
	if (m != (1<<SCM_PROP_LAST) - 1)   // a property is missing
		return -EINVAL;

	return 0;
}

static void
init_queues(struct scm_seg_queues *seg_qs, size_t qs_nr, const u16 *prop_masks)
{
	struct scm_seg_queue *q;
	u32 i;
	seg_qs->qs_nr = qs_nr;
	for (i=0; i<qs_nr; i++) {
		q = seg_qs->qs + i;
		cds_queue_base_init(&q->q_segs);
		cds_queue_base_init(&q->q_rel_segs);
		q->q_prop_set = (struct scm_prop_set){.mask__ = prop_masks[i]};
	}
	os_waitqueue_head_init(&seg_qs->wq_segs);
	os_waitqueue_head_init(&seg_qs->wq_rel_segs);
}

// mirrors the structure of @src queues to @dst
void
scm_seg_queues_init_mirror(struct scm_seg_queues *dst,
                           const struct scm_seg_queues *src)
{
	const struct scm_seg_queue *q_src;
	struct scm_seg_queue *q_dst;
	u32 i;

	dst->qs_nr = src->qs_nr;
	for (i=0; i<src->qs_nr; i++) {
		q_src = src->qs + i;
		q_dst = dst->qs + i;

		q_dst->q_nsegs_max     = q_src->q_nsegs_max;
		q_dst->q_rel_nsegs_max = q_src->q_rel_nsegs_max;

		cds_queue_base_init(&q_dst->q_segs);
		cds_queue_base_init(&q_dst->q_rel_segs);

		q_dst->q_prop_set = q_src->q_prop_set;
	}

	os_waitqueue_head_init(&dst->wq_segs);
	os_waitqueue_head_init(&dst->wq_rel_segs);
}

static void
fill_queue(struct scm_seg_queue *q,
           struct segment *segs, u64 nsegs, u64 nsegs_real,
           u32 seg_size, enum scm_property prop, u8 q_id)
{
	u64 rel_size, rel_rem, rsi;
	struct segment *seg;
	u64 d = nsegs_real / nsegs, inserted = 0;

	assert(nsegs_real >= nsegs);

	rel_size = cds_queue_base_size(&q->q_rel_segs);
	assert(rel_size <= SALSA_MIN_REL_SEGMENTS);
	rel_rem = SALSA_MIN_REL_SEGMENTS - rel_size;
	for (rsi=0; rsi < nsegs_real; rsi++) {
		seg = segs + rsi;
		assert(0 == ((uintptr_t) &seg->valid_nr & 3UL));
		segment_init(seg, seg_size, prop, q_id);

		// do not insert this segment into the queue
		if (rsi % d != 0 || inserted++ >= nsegs)
			continue;

		// fill rellocation queues first
		if (rsi < rel_rem) {
			segment_set_state(seg, SEG_FREE_REL);
			//cds_queue_base_enqueue_head(&q->q_rel_segs, &seg->list);
			cds_queue_base_enqueue_tail(&q->q_rel_segs, &seg->list);
		} else {
			segment_set_state(seg, SEG_FREE);
			//cds_queue_base_enqueue_head(&q->q_segs, &seg->list);
			cds_queue_base_enqueue_tail(&q->q_segs, &seg->list);
		}
	}
}

// Each segment has a property (e.g., SCM_PROP_FLASH for flash). These
// are described in @scm_paramenters for each scm device.
//
// For allocating segments, we want to create multiple segment queues based on
// these properties.
//
// A set of properties is described as a u16 bitmask (see scm_get_free_seg). A
// set bit means that the corresponding property is excluded from the set.
//
// The function below allows the user to configure how many queues SCM will
// create and how the segments will be segregated on these queues.
//
// Mask ordering *does* matter. It is the default order the allocator looks into
// the queues. That means that if no property constrains are specified the
// allocator will try to allocate from the first queue first. See
// scm_get_free_seg() for more details.
//
// NOTE: scm->seg_nr, scm->seg_size, scm->grain_size should be set by the
// caller before this function is called.
int
scm_seg_queues_init(struct sto_capacity_mgr *scm,
                    const u16 *qs_prop_masks, size_t qs_nr,
                    struct scm_dev_properties *d_props)
{
	struct scm_seg_queue *q;
	u32 i;
	u64 __attribute__((unused)) total_segments, s_idx;
	struct scm_seg_queues *seg_qs = &scm->seg_qs;
	const u64 bytes_per_segment = scm->grain_size*scm->seg_size;
	int err;

	err = verify_masks(qs_prop_masks, qs_nr);
	if (err) {
		MSG("verify_masks() failed");
		return err;
	}

	init_queues(seg_qs, qs_nr, qs_prop_masks);

	s_idx=0;
	for (i=0; i < d_props->ranges_nr; i++) {
		enum scm_property prop = d_props->ranges[i].r_prop;
		u8 q_id;
		u64 r_nsegs, wasted, r_nsegs_real;

		q = scm_prop_find_queue(seg_qs, prop);
		assert(q != NULL); // see checks above
		q_id =  q - seg_qs->qs;

		// number of segments in the range
		r_nsegs = d_props->ranges[i].r_len / bytes_per_segment;
		wasted  = d_props->ranges[i].r_len % bytes_per_segment;
		if (wasted)
			MSG("range: %u: wasted space due to rounding to segments: %"PRIu64" bytes", i, wasted);

		if (r_nsegs == 0) {
			ERR("No available segments for range %u", i);
			continue;
		}

		r_nsegs_real = d_props->ranges[i].r_real_len_ / bytes_per_segment;
		if (r_nsegs_real <= r_nsegs)
			r_nsegs_real = r_nsegs;
		if (r_nsegs_real > r_nsegs)
			MSG("range: %u: REAL (r_real_len) number of segments:  %"PRIu64, i, r_nsegs_real);

		// if scm_cfg_get_segs_nr() was used, the assertion should be OK
		assert(s_idx + r_nsegs <= scm->seg_nr);
		MSG("range: %u nsegs:%"PRIu64" queue_id:%u", i, r_nsegs, q_id);

		fill_queue(q, scm->segs + s_idx,
		           r_nsegs, r_nsegs_real,
		           scm->seg_size, prop, q_id);

		s_idx += r_nsegs;
	}

	total_segments = 0;
	MSG("Configuring queues bytes_per_segment:%"PRIu64, bytes_per_segment);
	for (i=0; i<qs_nr; i++) {
		q = seg_qs->qs + i;
		q->q_nsegs_max     = cds_queue_base_size(&q->q_segs);
		q->q_rel_nsegs_max = cds_queue_base_size(&q->q_rel_segs);

		MSG("Configured queue %2u with mask:0x%02x size:%"PRIu64" rel_size:%"PRIu64,
		     i, q->q_prop_set.mask__, q->q_nsegs_max, q->q_rel_nsegs_max);

		total_segments += (q->q_nsegs_max + q->q_rel_nsegs_max);
	}
	MSG("TOTAL: #segments: %"PRIu64" bytes:%"PRIu64, total_segments, total_segments*bytes_per_segment);

	return 0;
}

int
scm_seg_queues_init_conf(struct sto_capacity_mgr *scm, unsigned conf)
{
	struct scm_dev_properties *props = &scm->scm_dev_props;
	const static u16 masks_one[1] = {
		(1<<SCM_PROP_LAST) - 1
	};
	const static u16 masks_all[5] = {
		(1<<SCM_PROP_NVRAM),
		(1<<SCM_PROP_FLASH),
		(1<<SCM_PROP_SMR_CONV),
		(1<<SCM_PROP_SMR_SEQ1),
		(1<<SCM_PROP_SMR_SEQ2),
	};
	const size_t masks_all_size = sizeof(masks_all)/sizeof(masks_all[0]);

	switch (conf) {
		case SCM_SEG_QUEUES_ONE:
		return scm_seg_queues_init(scm, masks_one, 1, props);

		case SCM_SEG_QUEUES_ALL:
		return scm_seg_queues_init(scm, masks_all, masks_all_size, props);

		default:
		return -EINVAL;
	}

}

// helper: single queue (i.e., one for reloc and one for free)
int scm_seg_queues_init_singleq(struct sto_capacity_mgr *scm,
                                struct scm_dev_properties *d_props)
{
	static u16 masks[1] = { (1<<SCM_PROP_LAST) - 1 };
	return scm_seg_queues_init(scm, masks, 1, d_props);
}


// returns 0       and sets q_ret if a proper queue was found
// returns -EAGAIN if there was at least one matching queue which was empty
// returns -EINVAL if no matching queue was found. This is most likely a bug.
// if return is not 0, *q_ret is set to NULL
int
scm_seg_queues_get_queue(struct scm_seg_queues *seg_qs, struct scm_gfs_flags flags,
                         struct cds_queue_base **q_ret)
{
	u32 i;
	bool q_empty;
	int err;
	struct cds_queue_base *freeq;
	struct scm_prop_set props;
	bool last_iter;


	props = flags.prop_set_preferred;
	last_iter = false;
	q_empty = false;
	while (true) {
		// XXX: We check all the queues every time, which we don't need
		// to.
		for (i=0; i <seg_qs->qs_nr; i++) {
			struct scm_seg_queue *queue = &seg_qs->qs[i];
			// check if the flag set is compatible with the queue set
			if (!scm_prop_set_is_subset_of(queue->q_prop_set, props))
				continue;
			// check if the queue is empty
			freeq = flags.reloc ? &queue->q_rel_segs : &queue->q_segs;
			if (cds_queue_base_is_empty(freeq)) {
				q_empty = true;
				continue;
			}
			// queue looks good, return it
			*q_ret = freeq;
			return 0;
		}

		// do this two times, one for the preferred property set, and
		// one for the default
		if (!last_iter) {
			props = flags.prop_set_default;
			last_iter = true;
		} else break;
	}

	// we didn't find a queue. If none of the queues were empty, then
	// weirdly the mask did not match anything. This should not happen.
	// Issue a warning and return NULL.
	if (!q_empty) {
		ERR("%s: could not find matching queue for masks: preferred:%u default:%u",
		    __FUNCTION__, flags.prop_set_preferred.mask__, flags.prop_set_default.mask__);
		//abort();
		err = -EINVAL;
	} else {
		// MSG("%s: could not find matching queue for mask:%u\n", __FUNCTION__, flags.prop_mask);
		err = -EAGAIN;
	}
	*q_ret = NULL;
	return err;
}

// Return the property sets of queues (@qs_prop_sets) that match the given
// property set @prop_set, as well as the queue set.
void
scm_seg_queues_avail_props(const struct scm_seg_queues *seg_qs,
                           struct scm_prop_set prop_set,
                           /* out parameters */
                           struct scm_prop_set qs_prop_sets[static SCM_MAX_QUEUES],
                           u32 *qs_prop_sets_nr,
                           struct scm_seg_queue_set *qset)
{
	u32 qs_nr = 0, i;

	*qset = (struct scm_seg_queue_set){.qm__ = 0x0};

	for (i=0; i < seg_qs->qs_nr; i++) {
		const struct scm_seg_queue *q = &seg_qs->qs[i];
		// ignore queues that are not a subset of prop_mask
		if (!scm_prop_set_is_subset_of(q->q_prop_set, prop_set))
			continue;
		// ignore queue if it never had any segments
		if (q->q_nsegs_max + q->q_rel_nsegs_max == 0)
			continue;

		// set queue bit in bitset, and add property set
		qset->qm__ |= (1<<i);
		MSG("avail_qs_mask[%u] = 0x%x", qs_nr, q->q_prop_set.mask__);
		qs_prop_sets[qs_nr++] = q->q_prop_set;
	}
	MSG("avail_qs_nr = %u", qs_nr);
	MSG("qset=%x", qset->qm__);
	*qs_prop_sets_nr = qs_nr;
}


/**
 * We have a set of streams and we want to map them to different queue property
 * sets, so that we can create allocation flags for these queues. This function
 * maps a stream into queues and returns the property set of the queue.
 *
 * Also, if qs_size has the size of the queues, it returns the size of the
 * streams.
 *
 * Queue masks can be obtained via scm_seg_queues_avail()
 */
void
scm_seg_queues_map_to_streams(const struct scm_prop_set *qs_props,
                              const u64                 *qs_size,
                              u32 qs_nr, u16 streams_nr,
                              struct scm_prop_set *stream_props,
                              u64                 *stream_size)
{
	/*
	 * We have @qs_nr available queues and @streams_nr streams.
	 *
	 * Assumption:
	 *   hot streams -> high stream id
	 *   high quality storage -> low queue id
	 */
	DBG("qs_nr=%lu streams_nr=%u", (unsigned long)qs_nr, streams_nr);

	/*
	 * If we got lucky and @streams_nr == qs_props_nr, then it's easy
	 *
	 * For example, if we have 3 streams and 3 queues:
	 *  stream 0 -> queue 2
	 *  stream 1 -> queue 1
	 *  stream 2 -> queue 0
	 */
	if (qs_nr == streams_nr) {
		unsigned sid;
		for (sid=0; sid < streams_nr; sid++) {
			unsigned qid = qs_nr -sid -1;
			stream_props[sid] = qs_props[qid];
			stream_size[sid] = qs_size[qid];
			MSG("stream_id: %u prop_mask: 0x%x size:%"PRIu64, sid, stream_props[sid].mask__, stream_size[sid]);
		}
		return;
	}

	/*
	 * If we have more streams than queues, then we map the first
	 * streams at the last queue.
	 *
	 * For example, if we have 3 streams and 2 queues:
	 *  stream 0 -> mask of queue 1
	 *  stream 1 -> mask of queue 1
	 *  stream 2 -> mask of queue 0
	 */
	if (streams_nr > qs_nr) {
		const unsigned r = streams_nr - qs_nr;
		const unsigned last_qid = qs_nr - 1;
		const u64 last_size_part = qs_size[last_qid] / (r+1);
		const u64 last_size_rem = qs_size[last_qid] - last_size_part*r;
		unsigned sid;

		for (sid=0; sid < streams_nr; sid++) {
			if (sid <= r) {
				stream_props[sid] = qs_props[last_qid];
				stream_size[sid] = (sid == r) ? last_size_rem : last_size_part;
			} else {
				unsigned qid = qs_nr - (sid - r) - 1;
				stream_props[sid] = qs_props[qid];
				stream_size[sid] = qs_size[qid];
			}
			DBG("stream_id: %u prop_mask: 0x%x size:%" PRIu64, sid, stream_props[sid].mask__, stream_size[sid]);
		}
		return;
	}

	#if 1
	/*
	 * If we have more queues that streams, then the first stream will
	 * get a mask for multiple queues.
	 *
	 * For example, if we have 5 queues and 3 streams
	 *  stream 0 -> mask of queue 4 | mask of queue 3 | mask of queue 2
	 *  stream 1 -> mask of queue 1
	 *  stream 2 -> mask of queue 0
	 */
	if (qs_nr > streams_nr) {
		u32 d = qs_nr - streams_nr;
		unsigned sid;
		for (sid=0; sid < streams_nr; sid++) {
			struct scm_prop_set set;
			u64 size;
			if (sid == 0) {
				unsigned x;
				set = SCM_PROP_SET_EMPTY;
				size = 0;
				// NB: if streams_nr == 1, x >= 0 is not enough
				// as a check
				for (x=qs_nr - 1; x >= qs_nr -d -1 && x < qs_nr; x--) {
					set = scm_prop_set_union(set, qs_props[x]);
					size += qs_size[x];
				}
			} else {
				unsigned qid = qs_nr -sid -d -1;
				set = qs_props[qid];
				size = qs_size[qid];
			}

			stream_props[sid] = set;
			stream_size[sid] = size;
			DBG("stream_id: %u prop_mask: 0x%x size:%" PRIu64, sid, stream_props[sid].mask__, stream_size[sid]);
		}
		return;
	}
	#else
	/*
	 * If we have more queues that streams, then the last stream will
	 * get a mask for multiple queues.
	 *
	 * For example, if we have 5 queues and 3 streams
	 *  stream 0 -> mask of queue 4
	 *  stream 1 -> mask of queue 3
	 *  stream 2 -> mask of queue 2 | mask of queue 1 | mask of queue 0
	 */
	if (qs_nr > streams_nr) {
		unsigned sid, q;
		struct scm_prop_set last_stream_set;
		u64 last_stream_size;

		assert(streams_nr >= 1);
		for (sid=0; sid < streams_nr - 1; sid++) {
			unsigned qid = qs_nr -sid -1;
			stream_props[sid] = qs_props[qid];
			stream_size[sid] = qs_size[qid];
			MSG("stream_id: %u prop_mask: 0x%x size:%"PRIu64, sid, stream_props[sid].mask__, stream_size[sid]);
		}

		assert(sid == streams_nr - 1);
		last_stream_set = SCM_PROP_SET_EMPTY;
		last_stream_size = 0;
		for (q=0; q <= qs_nr -streams_nr; q++) {
			last_stream_set   = scm_prop_set_union(last_stream_set, qs_props[q]);
			last_stream_size += qs_size[q];
		}
		stream_props[sid] = last_stream_set;
		stream_size[sid] = last_stream_size;
		DBG("stream_id: %u prop_mask: 0x%x size:%" PRIu64, sid, stream_props[sid].mask__, stream_size[sid]);

		return;
	}
	#endif

	BUG_ON(true);
}

void
scm_seg_queues_dump_stats(const struct scm_seg_queues *qs)
{
	unsigned i;
	MSG("------------ SCM SEG QUEUES DUMP ----------------");
	for (i=0; i<qs->qs_nr; i++) {
		u64 usr_nsegs __attribute__((unused)), rel_nsegs __attribute__((unused));
		const struct scm_seg_queue *q = qs->qs + i;
		if (q->q_nsegs_max == 0)
			continue;
		usr_nsegs = os_atomic64_read(&q->q_segs.size);
		rel_nsegs = os_atomic64_read(&q->q_rel_segs.size);
		MSG("q=%2u usr_segs:%5"PRIu64"/%5"PRIu64" rel_segs:%5"PRIu64"/%5"PRIu64" prop_set_mask:0x%x",
			i,
			usr_nsegs, q->q_nsegs_max,
			rel_nsegs, q->q_rel_nsegs_max,
			q->q_prop_set.mask__);
	}
	MSG("-------------------------------------------------");
}

/* DebugFs functions */


#if defined(SCM_SEG_QUEUES_TEST)
static int
scm_seg_queues_map_to_streams_test()
{
	unsigned i, errors=0;

	const uint8_t m0 = 1<<SCM_PROP_NVRAM;
	const uint8_t m1 = 1<<SCM_PROP_FLASH;
	const uint8_t m2 = 1<<SCM_PROP_SMR_CONV;
	const uint8_t m3 = 1<<SCM_PROP_SMR_SEQ1;
	const uint8_t m4 = 1<<SCM_PROP_SMR_SEQ2;

	struct {
		uint8_t   qmasks[SCM_PROP_LAST];
		u64       qsizes[SCM_PROP_LAST];
		unsigned  qsets_nr;
		unsigned  streams_nr;
		uint8_t   res_masks[SALSA_MAX_STREAMS];
		u64       res_sizes[SALSA_MAX_STREAMS];
	} ts[] = {{
		.qsets_nr   = 3,
		.qmasks     = {m0, m1, m2},
		.qsizes     = {100, 200, 300},
		.streams_nr = 3,
		.res_masks  = {m2, m1, m0},
		.res_sizes  = {300, 200, 100},
	},{
		.qsets_nr   = 2,
		.qmasks     = {m0, m1},
		.qsizes     = {100, 200},
		.streams_nr = 4,
		.res_masks  = {m1, m1, m1, m0},
		.res_sizes  = {66, 66, 68, 100},
	},{
		.qsets_nr   = 5,
		.qmasks     = {m0, m1, m2, m3, m4},
		.qsizes     = {100, 200, 300, 400, 500},
		.streams_nr = 2,
		.res_masks  = {m4 | m3 | m2 | m1, m0},
		.res_sizes  = {1400,100},
	},{
		.qsets_nr   = 2,
		.qmasks     = {m0, m1},
		.qsizes     = {100, 200},
		.streams_nr = 1,
		.res_masks  = {m0 | m1},
		.res_sizes  = {300},
	},{
		.qsets_nr   = 0
	}};

	for (i=0; ; i++) {
		struct scm_prop_set q_pset[SCM_PROP_LAST];
		struct scm_prop_set s_pset[SALSA_MAX_STREAMS];
		u64                 s_sizes[SALSA_MAX_STREAMS];
		unsigned j;

		if (ts[i].qsets_nr == 0)
			break;

		for (j=0; j<ts[i].qsets_nr; j++) {
			q_pset[j] = (struct scm_prop_set){.mask__ = ts[i].qmasks[j]};
		}

		printf("%s: test %u\n", __FUNCTION__, i);
		scm_seg_queues_map_to_streams(q_pset, ts[i].qsizes, ts[i].qsets_nr,
		                              ts[i].streams_nr, s_pset, s_sizes);

		for (j=0; j<ts[i].streams_nr; j++) {
			uint8_t m_e = ts[i].res_masks[j];
			uint8_t m_r = s_pset[j].mask__;
			u64     s_e = ts[i].res_sizes[j];
			u64     s_r = s_sizes[j];

			if (m_e != m_r) {
				fprintf(stderr, "FAIL ==> stream idx=%u expected mask (0x%x) different form result (0x%x)\n", j, m_e, m_r);
				errors++;
			}

			if (s_e != s_r) {
				fprintf(stderr, "FAIL ==> stream idx=%u expected size (%llu) different form result (%llu)\n", j, (unsigned long long)s_e, (unsigned long long)s_r);
				errors++;
			}
		}
	}

	return errors;
}
#endif

#if defined(SCM_SEG_QUEUES_TEST)
int main(int argc, char *argv[])
{
	scm_seg_queues_map_to_streams_test();
	return 0;
}
#endif
