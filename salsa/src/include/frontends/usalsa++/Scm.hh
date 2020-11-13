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

#ifndef SALSA_SCM_HH__
#define SALSA_SCM_HH__

// C++ API for SCM
//
// This exposes a minimum set of functionality:
//  - segment allocation
//  - GC
//
// It is intended for minimum controllers such as a KV store.

#include <atomic>
#include <cerrno>
extern "C" {
	#include "gc/gc.h"
	#include "sto-ctlr/scm-seg-alloc.h"
	#include "sto-ctlr/sto-capacity-mgr.h"
	#include "frontends/usalsa++/allocator.h"
}
struct gc_io_work_parent;
namespace salsa {
class Scm;
struct GrainRange {
	GrainRange(): grain_start(SALSA_INVAL_GRAIN), grain_len(0) {}
	u64 grain_start;
	u64 grain_len;
};
class ScmSegmentIterator {
	friend class Scm;
public:
	ScmSegmentIterator(Scm *const scm): idx_m(0) , scm_m(scm) {}
	ScmSegmentIterator& operator++() { idx_m++; return *this;}
	ScmSegmentIterator& operator+=(int nr) { idx_m+=nr; return *this;}
	bool operator==(const ScmSegmentIterator &rhs) {return scm_m == rhs.scm_m && idx_m == rhs.idx_m;}
	bool operator!=(const ScmSegmentIterator &rhs) {return scm_m != rhs.scm_m || idx_m != rhs.idx_m;}
	ScmSegmentIterator(ScmSegmentIterator const& iter) {scm_m = iter.scm_m; idx_m = iter.idx_m;}
	void operator=(ScmSegmentIterator const& iter) {scm_m = iter.scm_m; idx_m = iter.idx_m;}

	GrainRange operator*();
protected:
	u64 idx_m;
private:
	Scm *scm_m;
	// ScmSegmentIterator(const ScmSegmentIterator &&)           = delete;
};

// A C++ wrapper for sto_capacity_mgr
class Scm {
public:
	Scm(); // invalid scm
	~Scm();

	int init(int argc, char **argv, u32 overprovision = 200U);

	// register/unregister controler
	struct RegistrationInfo {
		int               ri_err;
		u8                ri_ctlr_id; // valid if ri_err == 0
		u8                ri_stream_nr;  // valid if ri_err == 0
		scm_seg_queue_set ri_qset;    // valid if ri_err == 0
		RegistrationInfo() : ri_err(-1), ri_ctlr_id(-1), ri_stream_nr(-1) {}
	};
	RegistrationInfo register_ctlr(void *ctlr,
	                               gc_reloc_fn_t gc_cb,
	                               scm_prop_set prop_set,
				       u32 stream_nr);
	void unregister_ctlr(void *ctlr, RegistrationInfo &ri);

	int init_threads();
	int exit_threads();

	sto_capacity_mgr * scm_get (void) const { return scm_;}
	u64 get_size(void) const;
	u64 get_seg_size(void) const;
	u64 get_grain_size(void) const;
	int provision_grains(u64 len, bool is_reloc = false);
	void unprovision_grains(const u64 len) {
		const u64 old_val __attribute__((unused)) = used_capacity_.fetch_sub(len);
		assert(len <= old_val);
	}

	void invalidate_grains(const u64 grain, const u64 len, const bool is_reloc);
	void restore_grain_range(u64 grain, u64 len, u8 ctlr_id);
	u64 seg_to_grain(gc_io_work_parent *work) const;
	u64 grain_to_seg_idx(u64 grain) const;
	ScmSegmentIterator begin();
	ScmSegmentIterator end();
private:
	sto_capacity_mgr *scm_;
	std::atomic<u32>  used_streams_; // stream
	std::atomic<u64>  used_capacity_; // grains
	u64               max_capacity_;	 // grains
	u64               max_capacity_reloc_;	 // grains
	u32               overprovision_seg_nr_; // overprovision in # segments
	bool has_enough_spare_segments(u64 dev_size_raw, u64 grain_size, u64 segment_size, u32 stream_nr) const;
	// delete default copy constructor, assignment operator
	Scm(Scm const&) = delete;
	void operator=(Scm const&) = delete;
};

} // end namespace salsa

#endif /* SALSA_SCM_HH__ */
