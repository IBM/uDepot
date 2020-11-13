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

#ifndef SALSA_MD_HH__
#define SALSA_MD_HH__

extern "C" {
#include "libos/os-types.h"
}
namespace salsa {

struct salsa_dev_md {
	u64 physical_size;
	u64 logical_size;
	u64 segment_size;
	u64 grain_size;
	u64 seed;
	u32 csum;
}__attribute__((packed));

struct salsa_seg_md {
	u64 segment_size;
	u64 grain_size;
	u64 timestamp;
	u64 seed;
	u8  ctlr_type;
	u32 csum;
	u64 write_nr;
	u64 reloc_nr;
}__attribute__((packed));

};				// namespace salsa
#endif	// SALSA_MD_HH__
