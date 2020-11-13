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

#ifndef	_SALSA_SEG_ALLOC_H_
#define	_SALSA_SEG_ALLOC_H_

#include "sto-ctlr/scm-gfs-flags.h"

struct sto_capacity_mgr;
struct segment;

int  scm_get_segs_thin_prov(struct sto_capacity_mgr *, u64 seg_nr);
void scm_put_segs_thin_prov(struct sto_capacity_mgr *, u64 seg_nr);

/**
 * Allocate a new free segment.
 *  @gfs_flags: allocation flags. See below.
 *  @seg_out:   set to NULL if error is returned, otherwise set to the segment
 *
 * Returns:
 * 0:      success, and free segment in @seg_out
 * EAGAIN: try again
 * ENOSPC: ran out of physical space for thin provisioned devs
 */
int scm_get_free_seg(struct sto_capacity_mgr *scm, struct scm_gfs_flags flags,
                     struct segment **seg_out);

/**
 * Release a segment.
 */
void scm_put_free_seg(struct sto_capacity_mgr *const scm,
                      struct segment *const seg, bool thin_prov);


#endif // _SALSA_SEG_ALLOC_H_
