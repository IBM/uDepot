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

#ifndef	_SALSA_STO_CAPACITY_MGR_PARAMETERS_H_
#define	_SALSA_STO_CAPACITY_MGR_PARAMETERS_H_
#include "libos/os-types.h"
#include "gc/gc-parameters.h"
#include "generic/compiler.h"

#include "scm-dev-properties.h"

#define SALSA_MAX_GRAIN_SIZE	131072U  /* bytes */
#include <limits.h>
/* no need to limit ourselves in user-space: grain-size might be < 4KiB
 * the only real limit is the segment validity count, which is an int
 */
#define	SALSA_MAX_SEGMENT_SIZE	(INT_MAX - 1) /* # of pages => max equal to 12GiB worth of 4K pages */

#define	SALSA_MIN_SEGMENT_SIZE	8U /* # of pages */

#define	SALSA_MAX_BIO_FRONTPAD	32 /* bytes */

#define SCM_MAX_DEVICES  6
#define SCM_MAX_DEV_NAME 256


enum scm_raid_mode {
	RAID0 = 0,
	RAID5 = 5,
	SALSA_LINEAR = 11,
};

struct scm_parameters {
	u64                  dev_size_raw;     /* physical capacity, bytes */
	u64                  segment_size; /* # grains */
	u32                  grain_size;    /* bytes */
	u32                  queue_max_hw_grains;
	u32                  rmap_grain_size; /* # grains */
	bool                 use_rmap;
	bool                 simulation;
	bool                 must_rebuild;
	bool                 dont_rebuild;
	enum scm_raid_mode   raid;
	u16                  dev_nr; /* devs participating in raid */

	struct gc_parameters gc_params;

	// segment queue configuration (see enum scm_seg_queue_conf)
	unsigned                  seg_qs_conf;

	// properties of the resulting scm device
	struct scm_dev_properties cfg_scm_props;

	// properties of each SCM underlying device
	struct scm_dev_properties cfg_dev_props[SCM_MAX_DEVICES];

	char                      cfg_dev_names[SCM_MAX_DEVICES][SCM_MAX_DEV_NAME];
};

// how many segments on this device?
//  Note that this is not the same ass scm_dev_props_size() / bytes_per_segment
static inline u64
scm_cfg_get_segs_nr(struct scm_parameters *scm_cfg)
{
	const u64 bytes_per_seg = scm_cfg->grain_size*scm_cfg->segment_size;
	return scm_dev_props_size_grain(&scm_cfg->cfg_scm_props, bytes_per_seg);
}

static inline u64
scm_cfg_get_real_segs_nr(struct scm_parameters *scm_cfg)
{
	const u64 bytes_per_seg = scm_cfg->grain_size*scm_cfg->segment_size;
	return scm_dev_props_real_size_grain(&scm_cfg->cfg_scm_props, bytes_per_seg);
}

#endif	/* _SALSA_STO_CAPACITY_MGR_PARAMETERS_H_ */
