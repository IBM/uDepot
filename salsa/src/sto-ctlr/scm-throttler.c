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

#include "gc/gc.h"
#include "libos/os-atomic.h"
#include "libos/os-debug.h"
#include "sto-ctlr/sto-ctlr.h"
#include "sto-ctlr/private/sto-ctlr-common.h"

/* TODO: Make this a variable (e.g., seg_size/10) */
//#define SCM_USER_AVAIL 10ULL
#define SCM_USER_AVAIL 1000ULL

/**
 * Implmentation notes:
 * There are four values controlling throttling in the current implementation:
 * (listed based on frequency of change)
 *
 * ->u: This is equal to SCM_USER_AVAIL and never changes
 * ->g: This is equal to (WA-1)*SCM_USER_AVAIL, and is recomputed on every
 *      segment the GC relocates (scm_throttle_update()) based on the number of
 *      segments relocated and the size of the segment.

 * ->ga_nr: This is current GC budget. It is updated by the GC calling
 *  scm_throttle_gc_write_nr(). The value is decreased by the pages the GC
 *  writes. If it reaches zero, both ->ga_nr and ->ua_nr are reset to ->g and
 *  ->u respectively.
 *
 * ->ua_nr: This is the current user budget. If it reaches zero, the user will
 *  block until GC completes its updates and resets the user budget.
 *
 */

void scm_throttle_init(struct scm_throttler *const scmt)
{
	memset(scmt, 0, sizeof(*scmt));
	os_waitqueue_head_init(&scmt->throttle_waitq);
	/* init to unbounded */
	os_atomic64_set(&scmt->g, SCM_USER_AVAIL);
	os_atomic64_set(&scmt->u, SCM_USER_AVAIL);
	os_atomic64_set(&scmt->ga_nr, SCM_USER_AVAIL);
	os_atomic64_set(&scmt->ua_nr, SCM_USER_AVAIL);
}

static inline bool scm_throttle_user_slot(struct scm_throttler *const scmt)
{
	union {
		u64 u;
		s64 s;
	} old, new;
	do {
		old.s = os_atomic64_read(&scmt->ua_nr);
		if (0U == old.u)
			return true; /* throttle */
		new.u = old.u - 1U;
	} while (old.s != os_atomic64_cmpxchg(&scmt->ua_nr, old.s, new.s));

	return false;		/* don't throttle */
}

static inline void scm_throttle_gc_slot(
	struct scm_throttler *const scmt,
	u32                         gc_nr)
{
	while (gc_nr) {
		// update ->ga_nr to MIN(0, ->ga_nr - gc_nr) using CAS
		union {
			u64 u;
			s64 s;
		} old, new = { 0 };
		do {
			old.s = os_atomic64_read(&scmt->ga_nr);
			if (0U == old.u) {
				new.u = 0; /* don't wake up */
				break;
			}
			new.u = gc_nr <= old.u ? old.u - gc_nr : 0U;
		} while (old.s != os_atomic64_cmpxchg(&scmt->ga_nr, old.s, new.s));

		gc_nr -= old.u - new.u;

		// if there are no more GC slots refill both user/gc slots.
		if (0U == new.u) {
			/* allow u more user writes */
			os_atomic64_set(&scmt->ua_nr, os_atomic64_read(&scmt->u));
			/* allow g more gc writes */
			os_atomic64_set(&scmt->ga_nr, os_atomic64_read(&scmt->g));
			/* wake up users */
			os_wakeup_all(&scmt->throttle_waitq);
		}
	}
}

static inline bool
scm_throttle_user(struct sto_capacity_mgr *const scm)
{
	struct scm_throttler *const scmt = &scm->scmt;
	struct gc *const gc = scm->gc;
	/**
	 * throttle if (indendation indicates associativity):
	 * - We are not shutting down (exit == 0) AND
	 *      - Number of free segments is lower or equal to 1 OR
	 *              - Number of free segments is lower or equal to the gc_low watermanrk AND
	 *              - The throttler has run out of user slots
	 */
	if (1 == os_atomic32_read(&scm->exit))
		return false;

	if (gc_check_critical(gc))
		return true;

	/* if GC is not running, don't stall, nobody will wake us up */
	if (!gc_is_active(scm->gc))
		return false;

	DBG("about to call scm_throttle_user_slot.");
	return scm_throttle_user_slot(scmt);
}

void scm_throttle_user_write(struct sto_capacity_mgr *const scm)
{
	struct scm_throttler *const scmt = &scm->scmt;
	// Timeout can lead to high CPU utilization, so we try to avoid it
	// os_wait_event_interruptible_timeout(scmt->throttle_waitq,
	//                                     scm_throttle_user_wakeup(scm),
	//                                     15000);
	os_wait_event_interruptible(scmt->throttle_waitq, !scm_throttle_user(scm));
}

void scm_throttle_gc_write_nr(
	struct sto_capacity_mgr *const scm,
	const u32                      gc_nr)
{
	struct scm_throttler *const scmt = &scm->scmt;
	scm_throttle_gc_slot(scmt, gc_nr);
}

void
scm_throttle_gc_stopped(struct sto_capacity_mgr *const scm)
{
	struct scm_throttler *const scmt = &scm->scmt;
	os_wakeup_all(&scmt->throttle_waitq);
}

void
scm_throttle_gc_starts(struct sto_capacity_mgr *const scm)
{
	struct scm_throttler *const scmt = &scm->scmt;
	/* reset user writes */
	os_atomic64_set(&scmt->ua_nr, os_atomic64_read(&scmt->u));
}

/* called every time a segment has been relocated */
void scm_throttle_update(struct sto_capacity_mgr *const scm, const u32 relocs)
{
	struct scm_throttler *const scmt = &scm->scmt;
	const u64 seg_size = scm_get_seg_size(scm);
	u64 write_amp, gc_nr, write_amp_total;

	if (0U == relocs)
		return;

	/* care about write amp since last time */
	gc_nr   = relocs;
	assert(gc_nr <= seg_size);

	if (seg_size <= gc_nr)
		return;

	// write amplification for this segment
	write_amp = seg_size * SCM_USER_AVAIL / (seg_size - gc_nr);
	/* write_amp -= SCM_USER_AVAIL / 5ULL; /\* Give GC a bit more priority *\/ */

	if (write_amp <= SCM_USER_AVAIL)
		return;
	assert(SCM_USER_AVAIL < write_amp);

	write_amp_total = write_amp;

	os_atomic64_set(&scmt->g, write_amp_total - SCM_USER_AVAIL);

	if (SCM_USER_AVAIL < os_atomic64_read(&scmt->ua_nr))
		os_atomic64_set(&scmt->ua_nr, SCM_USER_AVAIL);

	DBG("seg relocations=%5" PRId64
		" USR budget:%7"PRId64" GC budget:%7"PRId64
		" wa=%7"PRIu64" wa_seg=%7"PRIu64" (scaled on %7llu)",
		gc_nr,
		os_atomic64_read(&scmt->ua_nr), os_atomic64_read(&scmt->ga_nr),
		write_amp_total, write_amp, SCM_USER_AVAIL);
}

void scm_throttle_exit(struct scm_throttler *scmt)
{
	os_wakeup_all(&scmt->throttle_waitq);
}
