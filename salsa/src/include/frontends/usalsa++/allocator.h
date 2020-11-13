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
#ifndef	_SALSA_ALLOCATOR_H_
#define	_SALSA_ALLOCATOR_H_

#include "libos/os-types.h"

#define SALSA_INVAL_GRAIN        ((u64) (-1))

struct sto_capacity_mgr;
struct allocator;
struct scm_gfs_flags;
typedef void
(*seg_md_callback_t) (void *sc, u64 start, u64 len);

u64 allocator_allocate_grains(struct allocator *, u64 len);
void allocator_release_grains(struct allocator *, u64 grain, u64 len);
void allocator_wait_for_grains(struct allocator *);
int allocator_init(struct allocator **, struct sto_capacity_mgr *, void *, u64, const struct scm_gfs_flags *, u8 ctlr_id, seg_md_callback_t fn);
u32 allocator_drain_remaining_grains(struct allocator *);

int allocator_exit(struct allocator *);

#endif	/* _SALSA_ALLOCATOR_H_ */
