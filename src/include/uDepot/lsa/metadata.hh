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

#ifndef	_UDEPOT_SALSA_MD_H_
#define	_UDEPOT_SALSA_MD_H_

#include <vector>
#include <atomic>

#include "frontends/usalsa++/SalsaMD.hh"
#include "uDepot/lsa/type.hh"
#include "util/types.h"
namespace udepot {

struct Timestamp {
	Timestamp() : time_ ({0}) {}
	Timestamp(struct timespec time) : time_ ({.tv_sec = time.tv_sec, .tv_nsec = time.tv_nsec}) {}
	struct timespec time_;

	friend bool operator< (const Timestamp &ts1, const Timestamp &ts2) {
		return (ts1.time_.tv_sec < ts2.time_.tv_sec) ||
			((ts1.time_.tv_sec == ts2.time_.tv_sec) && (ts1.time_.tv_nsec < ts2.time_.tv_nsec));
	}
};

template<typename RT>
class uDepotSalsaMD {
public:
	uDepotSalsaMD(size_t size, u64 grain_size, typename RT::IO &udepot_io);
	~uDepotSalsaMD() {}
	// if valid the configuration will be returned in dev_md
	bool validate_dev_md(salsa::salsa_dev_md *dev_md);
	bool validate_seg_md(u64 grain, uDepotSalsaType ctlr_type);
	bool validate_seg_md(const salsa::salsa_seg_md &seg_md, uDepotSalsaType ctlr_type);
	int read_seg_md(u64 grain, salsa::salsa_seg_md *const seg_md);
	void init(const salsa::salsa_dev_md &dev_md);
	salsa::salsa_dev_md default_dev_md(u64 grain_size, u64 segment_size) const;
	int persist_seg_md(u64 grain, u64 timestamp, uDepotSalsaType ctlr_type);
	int persist_dev_md();
	int check_dev_size(u64 segment_size);
	u64 get_seg_md_size() const;
	u64 get_dev_md_size() const;
	u64 get_dev_size() const;
	bool is_restored() const {return restored_m;}
	u32 checksum32(const u32 seed, const u8 *buf, u32 len) const;
	u16 checksum16(const u32 seed, const u8 *buf, u32 len) const;
	const u64 get_seg_ts(const u64 seg_idx) const {
		assert(seg_idx < seg_ts_m.size());
		return seg_ts_m[seg_idx];
	}
	const void restore_seg_ts(const u64 seg_idx, const u64 ts) {
		assert(seg_idx < seg_ts_m.size());
		seg_ts_m[seg_idx] = ts;
	}
	static u64 timestamp();
private:
	u64 seed_m, grain_size_m;
	bool restored_m;
	typename RT::IO     &udepot_io_m;
	salsa::salsa_dev_md dev_md_m;
	salsa::salsa_seg_md seg_md_m;
	std::vector<std::atomic<u64>> seg_ts_m;
	u64 dev_md_grain_offset() const;
	u64 get_seg_md_sizeb() const { return sizeof(salsa::salsa_seg_md); } // grains
	u64 get_dev_md_sizeb() const { return sizeof(salsa::salsa_dev_md); } // grains

	uDepotSalsaMD(const uDepotSalsaMD &)            = delete;
	uDepotSalsaMD(const uDepotSalsaMD &&)           = delete;
	uDepotSalsaMD& operator=(const uDepotSalsaMD &) = delete;
};

};				// namespace udepot
#endif	// _UDEPOT_SALSA_MD_H_
