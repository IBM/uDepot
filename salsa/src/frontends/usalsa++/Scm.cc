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

#include "frontends/usalsa++/Scm.hh"

extern "C" {
	#include "gc/gc-io-work.h"
	#include "gc/gc.h"
	#include "sto-ctlr/private/sto-capacity-mgr-common.h"
	#include "sto-ctlr/sto-capacity-mgr.h"
	#include "util/parse-storage-args.h"
	#include "util/scm_usr_helpers.h"
}
#include <mutex>
namespace salsa {

Scm::Scm() : scm_(nullptr), used_streams_(0U), used_capacity_(0UL), max_capacity_(0UL), max_capacity_reloc_(0UL) {}

int
Scm::init(int argc, char **argv, u32 overprovision)
{
	scm_parameters scm_cfg;
	int err = scm_usr_init_cfg(argv, argc, &scm_cfg);
	const u64 reloc_spare = scm_cfg.segment_size * (2); // 2 segments
	const u64 seg_aligned_dev_size = align_down(scm_cfg.dev_size_raw, scm_cfg.grain_size * scm_cfg.segment_size);
	if (err) {
		ERR("scm_usr_init_cfg return err=%d", err);
		goto fail0;
	}

	max_capacity_ = scm_cfg.dev_size_raw / scm_cfg.grain_size * (1000ULL - overprovision) / 1000ULL;
	max_capacity_reloc_ = max_capacity_ + reloc_spare;

	if (!has_enough_spare_segments(seg_aligned_dev_size, scm_cfg.grain_size,
					scm_cfg.segment_size, 3)) {
		err = EINVAL;
		goto fail0;
	}

	err = sto_capacity_mgr_ctr(&scm_cfg, &scm_);
	if (err)
		ERR("sto_capacity_mgr_ctr returned err=%d", err);

	return err;
fail0:
	scm_ = nullptr;
	return err;
}

int
Scm::init_threads()
{
	int err = sto_capacity_mgr_init_threads(scm_);
	if (err) {
		ERR("sto_capacity_mgr_init_threads return err=%d.", err);
	}
	return err;
}

Scm::~Scm()
{
	if (scm_ == nullptr)
		return;
	gc_print_info(scm_->gc);
	gc_print_list_info(scm_->gc);
	const int err = sto_capacity_mgr_dtr(scm_);
	if (err)
		ERR("sto_capacity_mgr_dtr failed with err=%d", err);
	DBG("SCM used capacity=%lu grains", used_capacity_.load());
	scm_ = nullptr;
}

int Scm::exit_threads() {
	int err;
	err = sto_capacity_mgr_exit_threads(scm_);
	if (err)
		ERR("sto_capacity_mgr_exit_threads failed with err=%d", err);
	return err;
}


bool
Scm::has_enough_spare_segments(
	const u64 dev_size,
	const u64 grain_size,
	const u64 segment_size,
	const u32 stream_nr) const
{
	const u32 new_stream_nr = used_streams_ + stream_nr;
	u64 spare_seg_nr, min_seg_nr;
	const u64 seg_sizeb = grain_size * segment_size;
	const u64 tot_segs = align_down(dev_size, seg_sizeb) / seg_sizeb;
	const u64 used_segs = align_down(max_capacity_reloc_ * grain_size, seg_sizeb) / seg_sizeb;
	min_seg_nr = SALSA_MIN_REL_SEGMENTS + new_stream_nr;
	spare_seg_nr = used_segs < tot_segs ? tot_segs - used_segs : 0;

	if (spare_seg_nr < min_seg_nr || dev_size <= max_capacity_reloc_ * grain_size) {
		ERR("Not enough spare segments=%lu min=%lu.", spare_seg_nr, min_seg_nr);
		return false;
	}

	return true;
}
std::mutex register_mutex_g;

Scm::RegistrationInfo
Scm::register_ctlr(void *ctlr,
                   gc_reloc_fn_t gc_cb,
                   scm_prop_set prop_set,
		   const u32 stream_nr)
{
	Scm::RegistrationInfo ri;
	scm_prop_set qps[SCM_MAX_QUEUES] __attribute__((unused));
	u32 nqs __attribute__((unused));

	// compute the queue set of this controler based on the given property set
	scm_seg_queues_avail_props(&scm_->seg_qs, prop_set, qps, &nqs, &ri.ri_qset);
	if (scm_seg_queue_set_is_empty(ri.ri_qset)) {
		ERR("Empty queue set! Bailing out.");
		ri.ri_err = ENOSPC;
		return ri;
	}
	register_mutex_g.lock();
	if (!has_enough_spare_segments(get_size(), get_grain_size(), get_seg_size(), stream_nr)) {
		register_mutex_g.unlock();
		ERR("Not enough spare segments.");
		ri.ri_err = EINVAL;
		return ri;
	}
	used_streams_ += stream_nr;
	register_mutex_g.unlock();
	ri.ri_stream_nr = stream_nr;

	ri.ri_err = scm_register_sto_ctlr(scm_, ctlr, &ri.ri_ctlr_id);
	if (ri.ri_err) {
		ERR("scm_register_sto_ctlr failed with err=%d", ri.ri_err);
		goto fail0;
	}

	ri.ri_err = gc_register_ctlr(scm_->gc, ri.ri_ctlr_id, gc_cb, ri.ri_qset);
	if (ri.ri_err) {
		ERR("gc_register_ctlr failed with err=%d", ri.ri_err);
		goto fail1;
	}

end:
	return ri;

fail1:
	scm_unregister_sto_ctlr(scm_, ctlr);
fail0:
	used_streams_ -= stream_nr;
	goto end;
}

void
Scm::unregister_ctlr(void *ctlr, Scm::RegistrationInfo &ri)
{
	if (ri.ri_err)
		return;

	assert(scm_get_ctlr(scm_, ri.ri_ctlr_id) == ctlr);
	int err = gc_unregister_ctlr(scm_->gc, ri.ri_ctlr_id);
	if (err) {
		ERR("gc_gc_unregister failed with err=%d", err);
	}
	assert(0 < ri.ri_stream_nr);
	assert(ri.ri_stream_nr <= used_streams_);
	used_streams_ -= ri.ri_stream_nr;
	scm_unregister_sto_ctlr(scm_, ctlr);
	ri.ri_err = -1; // invalidate registration info
	return;
}

u64
Scm::get_size(void) const
{
	return scm_get_dev_size(scm_);
}

u64
Scm::get_seg_size(void) const
{
	return scm_get_seg_size(scm_);
}

u64
Scm::get_grain_size(void) const
{
	return scm_get_grain_size(scm_);
}

u64
Scm::seg_to_grain(gc_io_work_parent *const work) const
{
	return scm_seg_to_grain(scm_, work->seg);
}

u64
Scm::grain_to_seg_idx(const u64 grain) const 
{
	return scm_grain_to_seg_idx(scm_, grain);
}

void
Scm::restore_grain_range(const u64 grain, const u64 len, const u8 ctlr_id)
{
	segment *const seg = scm_grain_to_segment(scm_, grain);
	assert(seg == scm_grain_to_segment(scm_, grain + len));
	assert(0 < len);
	segment_getn(seg, len);
	if (SEG_FREE == segment_get_state(seg) || SEG_FREE_REL == segment_get_state(seg)) {
		scm_detach_free_seg(scm_, seg);
		segment_putn(seg, get_seg_size());
		assert(0 < segment_valid_nr(seg));
		segment_set_ctlr(seg, ctlr_id);
		// hand it over to the GC
		gc_seg_destaged(scm_->gc, seg);
		int __attribute__((unused)) rc = provision_grains(len, true);
		assert(0 == rc);
	} else {
		ERR("restore grain range g=%lu len=%lu ctlr_id=%u failed",
                        grain, len, ctlr_id);
        }

	assert(segment_valid_nr(seg) <= get_seg_size());
}

int
Scm::provision_grains(const u64 len, const bool is_reloc)
{
	if (!is_reloc && gc_check_critical(scm_->gc))
		return EAGAIN;

	const u64 old_val = used_capacity_.fetch_add(len);
	// if is_reloc then we'll get it back soon
	if (!is_reloc && max_capacity_ < old_val + len) {
		MSG("requested capacity=%lu max=%lu", old_val + len, max_capacity_);
		used_capacity_.fetch_sub(len);
		return ENOSPC;
	} else if (is_reloc && max_capacity_reloc_ < old_val + len) {
		MSG("requested capacity=%lu max=%lu", old_val + len, max_capacity_reloc_);
		used_capacity_.fetch_sub(len);
		return ENOSPC;
	}
	return 0;
}

void
Scm::invalidate_grains(const u64 grain, const u64 len, const bool is_reloc)
{
	segment *const seg = scm_grain_to_segment(scm_, grain);
	if (!is_reloc)
		gc_page_invalidate(scm_->gc, seg, len);
	else
		gc_page_invalidate_reloc(scm_->gc, seg, len);
	unprovision_grains(len);
}

ScmSegmentIterator
Scm::begin()
{
	ScmSegmentIterator iter(this);
	return iter;
}

ScmSegmentIterator
Scm::end()
{
	ScmSegmentIterator iter(this);
	const u64 seg_nr = scm_get_seg_nr(scm_);
	iter += seg_nr;
	return iter;
}

GrainRange
ScmSegmentIterator::operator*()
{
	GrainRange gr;
	sto_capacity_mgr *const scm = scm_m->scm_get();
	const u64 seg_nr __attribute__((unused)) = scm_get_seg_nr(scm);
	assert(idx_m < seg_nr);
	gr.grain_start = idx_m * scm_get_seg_size(scm);
	gr.grain_len = scm_get_seg_size(scm);
	return gr;
}

} // end namespace salsa
