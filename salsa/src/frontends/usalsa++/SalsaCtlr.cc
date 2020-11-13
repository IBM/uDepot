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

#include "frontends/usalsa++/SalsaCtlr.hh"
#include "frontends/usalsa++/Scm.hh"
#include <cerrno>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <algorithm>
extern "C" {
	#include "frontends/usalsa++/allocator.h"
	#include "gc/gc.h"
	#include "sto-ctlr/scm-dev-properties.h"
	#include "sto-ctlr/sto-capacity-mgr.h"
}

namespace salsa {

SalsaCtlr::SalsaCtlr() : cb_scm_(nullptr), cb_prop_set_(SCM_PROP_SET_ALL),
			 stream_nr_(0), rel_stream_nr_(0), reserved_per_seg_(0), seg_alloc_nr_(0)
{
	memset(&cb_gfs_flags_, 0, sizeof(cb_gfs_flags_));
	memset(&alltr_, 0, sizeof(*alltr_) * USALSAPP_MAX_STREAMS);
	memset(&rel_alltr_, 0, sizeof(*rel_alltr_) * USALSAPP_MAX_STREAMS);
	cb_gfs_flags_.prop_set_default   = cb_prop_set_;
	cb_gfs_flags_.prop_set_preferred = cb_prop_set_;
}

int
SalsaCtlr::shutdown()
{
	if (nullptr == cb_scm_)
		return EINVAL;
	for (u32 i = 0; i < stream_nr_; ++i) {
		assert(nullptr != alltr_[i]);
		int rc __attribute__((unused)) = allocator_exit(alltr_[i]);
		assert(0 == rc);
		alltr_[i] = nullptr;
	}
	for (u32 i = 0; i < rel_stream_nr_; ++i) {
		assert(nullptr != rel_alltr_[i]);
		int rc __attribute__((unused)) = allocator_exit(rel_alltr_[i]);
		assert(0 == rc);
		rel_alltr_[i] = nullptr;
	}
	cb_scm_->unregister_ctlr(this, cb_ri_);
	cb_scm_ = nullptr;
	return 0;
}

SalsaCtlr::~SalsaCtlr()
{
	if (nullptr == cb_scm_)
		return;

	int rc __attribute__((unused)) = cb_scm_->exit_threads();
	assert(0 == rc);
	for (u32 i = 0; i < stream_nr_; ++i) {
		assert(nullptr != alltr_[i]);
		rc = allocator_exit(alltr_[i]);
		assert(0 == rc);
		alltr_[i] = nullptr;
	}
	for (u32 i = 0; i < rel_stream_nr_; ++i) {
		assert(nullptr != rel_alltr_[i]);
		rc = allocator_exit(rel_alltr_[i]);
		assert(0 == rc);
		rel_alltr_[i] = nullptr;
	}
	cb_scm_->unregister_ctlr(this, cb_ri_);
}

__attribute__((warn_unused_result))
static int
gc_callback(void *arg, struct gc_io_work_parent *const work)
{
	SalsaCtlr *const sc = static_cast<SalsaCtlr *>(arg);
	const u64 grain_start = sc->seg_to_grain(work);
	return sc->gc_callback(grain_start, sc->get_seg_size());
}

static void
seg_md_callback(void *const arg, const u64 grain, const u64 len)
{
	SalsaCtlr *const sc = static_cast<SalsaCtlr *>(arg);
	sc->inc_seg_alloc_nr();
	DBG("alloc-nr=%lu", sc->get_seg_alloc_nr());
	return sc->seg_md_callback(grain, len);
}

__attribute__((warn_unused_result))
int
SalsaCtlr::init(Scm *const scm, const u64 reserved_per_seg,
		const u32 stream_nr, const u32 rel_stream_nr, const struct scm_prop_set pset)
{
	int err = 0;
	int stream = 0;
	scm_gfs_flags flags = cb_gfs_flags_;
	if (nullptr == scm) {
		ERR("invalid scm argument");
		err = EINVAL;
		goto fail0;
	}
	cb_scm_ = scm;

	cb_prop_set_ = pset;
	stream_nr_ = std::max(std::min(stream_nr, USALSAPP_MAX_STREAMS), 1U);
	rel_stream_nr_ = std::max(std::min(rel_stream_nr, USALSAPP_MAX_STREAMS), 0U);
	reserved_per_seg_ = reserved_per_seg;

	cb_ri_ = cb_scm_->register_ctlr(this, ::salsa::gc_callback, pset, stream_nr_ + rel_stream_nr_);
	if (cb_ri_.ri_err) {
		err = cb_ri_.ri_err;
		ERR("scm_register_sto_ctlr failed with err=%d", cb_ri_.ri_err);
		goto fail1;
	}
	cb_gfs_flags_.prop_set_default   = pset;
	cb_gfs_flags_.prop_set_preferred = pset;
	flags.reloc = 0;
	assert(stream_nr_ + rel_stream_nr_ == cb_ri_.ri_stream_nr);
	for (stream = 0; stream < (s32) stream_nr_; ++stream) {
		err = allocator_init(&alltr_[stream], cb_scm_->scm_get(), this,
				reserved_per_seg, &flags, cb_ri_.ri_ctlr_id, ::salsa::seg_md_callback);
		if (0 != err) {
			ERR("allocator init failed with err=%d", err);
			goto fail2;
		}
	}
	flags.reloc = 1;
	for (stream = 0; stream < (s32) rel_stream_nr_; ++stream) {
		err = allocator_init(&rel_alltr_[stream], cb_scm_->scm_get(), this,
				reserved_per_seg, &flags, cb_ri_.ri_ctlr_id, ::salsa::seg_md_callback);
		if (0 != err) {
			ERR("allocator init failed with err=%d", err);
			goto fail3;
		}
	}

	if (0 == rel_stream_nr_) {
		assert(0 < (s32) stream_nr_);
		rel_alltr_[0] = alltr_[stream_nr_ - 1];
	}
	MSG("stream-nr=%u rel-stream-nr=%u", stream_nr_, rel_stream_nr_);
	return err;
fail3:
	for (--stream; 0 <= stream; --stream) {
		assert(nullptr != rel_alltr_[stream]);
		int rc __attribute__((unused)) = allocator_exit(rel_alltr_[stream]);
		assert(0 == rc);
		rel_alltr_[stream] = nullptr;
	}
	stream = stream_nr_ + 1;
fail2:
	for (--stream; 0 <= stream; --stream) {
		assert(nullptr != alltr_[stream]);
		int rc __attribute__((unused)) = allocator_exit(alltr_[stream]);
		assert(0 == rc);
		alltr_[stream] = nullptr;
	}

	cb_scm_->unregister_ctlr(this, cb_ri_);
fail1:
	cb_scm_ = nullptr;
fail0:
	return err;
}

int
SalsaCtlr::gc_callback(u64 grain_start, u64 grain_nr)
{
	assert(0);
	return ENOSYS;
}

void
SalsaCtlr::seg_md_callback(u64 grain_start, u64 grain_nr)
{
	assert(0);
}

allocator *
SalsaCtlr::stream_to_alltr(u32 stream, const bool is_reloc)
{
	stream %= !is_reloc ? stream_nr_ : (0 == rel_stream_nr_ ? 1 : rel_stream_nr_);
	allocator *const alltr = !is_reloc ? alltr_[stream] : rel_alltr_[stream];
	DBG("alltr=%p stream=%u stream_nr=%u", alltr, stream, stream_nr_);
	return alltr;
}

int
SalsaCtlr::allocate_grains_no_wait(const u64 len, u64 *const grain_out, const u32 stream, const bool is_reloc)
{
	if (unlikely(get_seg_size() < len + reserved_per_seg_)) {
		MSG("segment size (%lu) smaller than requested size %lu.", get_seg_size(), len);
		return EINVAL;
	}
	int rc = cb_scm_->provision_grains(len, is_reloc);
	if (0 != rc)
		return rc;
	allocator *const alltr = stream_to_alltr(stream, is_reloc);
	// could sleep
	u64 grain = allocator_allocate_grains(alltr, len);
	DBG("alltr=%p stream=%u grain=%lu", alltr, stream, grain);
	if (SALSA_INVAL_GRAIN == grain) {
		cb_scm_->unprovision_grains(len);
		return EAGAIN;
	}
	*grain_out = grain;
	return 0;
}

void
SalsaCtlr::wait_for_grains(const u32 stream, const bool is_reloc)
{
	allocator *const alltr = stream_to_alltr(stream, is_reloc);
	allocator_wait_for_grains(alltr);
}

int
SalsaCtlr::allocate_grains(const u64 len, u64 *const grain_out, const u32 stream, const bool is_reloc)
{
	int rc = allocate_grains_no_wait(len, grain_out, stream, is_reloc);
	if (EAGAIN == rc) {
		allocator *const alltr = stream_to_alltr(stream, is_reloc);
		allocator_wait_for_grains(alltr);
		rc = allocate_grains_no_wait(len, grain_out, stream, is_reloc);
	}
	return rc;
}

u32
SalsaCtlr::drain_remaining_grains(const u32 stream, const bool is_reloc)
{
	allocator *const alltr = stream_to_alltr(stream, is_reloc);
	return allocator_drain_remaining_grains(alltr);
}

void
SalsaCtlr::release_grains(const u64 grain, const u64 len, u32 stream, const bool is_reloc)
{
	allocator *const alltr = stream_to_alltr(stream, is_reloc);
	allocator_release_grains(alltr, grain, len);
}

void
SalsaCtlr::invalidate_grains (u64 grain, const u64 len, const bool is_reloc)
{
	cb_scm_->invalidate_grains(grain, len, is_reloc);
}

};
