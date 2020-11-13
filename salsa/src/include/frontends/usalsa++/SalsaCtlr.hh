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
#ifndef	_SALSACTLR_HH_
#define	_SALSACTLR_HH_

#include "frontends/usalsa++/Scm.hh"
extern "C" {
#include "frontends/usalsa++/allocator.h"
#include "libos/os-types.h"
#include "sto-ctlr/scm-dev-properties.h"
#include "sto-ctlr/scm-gfs-flags.h"
}
#include <atomic>

struct gc_io_work_parent;
struct segment;
struct allocator;
namespace salsa {

#define	USALSAPP_MAX_STREAMS	128U
// A minimal controller, intended as a base for simple controllers on top of Scm
class SalsaCtlr {
public:
	SalsaCtlr();
	virtual ~SalsaCtlr();
	virtual int gc_callback(u64 grain_start, u64 grain_nr);
	virtual void seg_md_callback(u64 grain_start, u64 grain_nr);
	int init(Scm *scm, u64 reserved_per_seg, u32 stream_nr = 1, u32 rel_stream_nr = 0, scm_prop_set pset = SCM_PROP_SET_ALL);
	int shutdown();

	int allocate_grains(u64 len, u64 *grain_out, u32 stream = 0, bool is_reloc = false);
	int allocate_grains_no_wait(u64 len, u64 *grain_out, u32 stream = 0, bool is_reloc = false);
	u32 drain_remaining_grains(u32 stream = 0, bool is_reloc = false);
	void wait_for_grains(u32 stream = 0, bool is_reloc = false);
	void release_grains(u64 grain, u64 len, u32 stream = 0, bool is_reloc = false);
	void invalidate_grains(u64 grain, u64 len, bool is_reloc);
	u64 get_size(void) const { return cb_scm_->get_size(); } // bytes
	u64 get_seg_size(void) const { return cb_scm_->get_seg_size(); } // grains
	u64 get_grain_size(void) const { return cb_scm_->get_grain_size(); } // bytes
	u64 seg_to_grain(struct gc_io_work_parent *const work) const { return cb_scm_->seg_to_grain(work); } // grains
	u64 grain_to_seg_idx(u64 grain) const { return cb_scm_->grain_to_seg_idx(grain); }
	Scm *get_scm() const {return cb_scm_;}
	u8 get_ctlr_id(void) const { return cb_ri_.ri_ctlr_id; }
	u64 get_seg_alloc_nr() const {return seg_alloc_nr_.load(); }
	void restore_seg_alloc_nr(u64 nr) { assert(seg_alloc_nr_ < nr); seg_alloc_nr_ = nr; }
	void inc_seg_alloc_nr() { seg_alloc_nr_.fetch_add(1); }
private:
	Scm                  *cb_scm_;
	scm_prop_set          cb_prop_set_;
	Scm::RegistrationInfo cb_ri_;
	scm_gfs_flags         cb_gfs_flags_;
	allocator            *alltr_[USALSAPP_MAX_STREAMS];
	allocator            *rel_alltr_[USALSAPP_MAX_STREAMS];
	u32                   stream_nr_;
	u32                   rel_stream_nr_;
	u32                   reserved_per_seg_;
	std::atomic<u64>      seg_alloc_nr_;
	allocator *stream_to_alltr(u32 stream, bool is_reloc);
	SalsaCtlr(const SalsaCtlr &)            = delete;
	SalsaCtlr& operator=(const SalsaCtlr &) = delete;
};

}; // end namespace salsa

#endif	/*_SALSACTLR_HH_ */
