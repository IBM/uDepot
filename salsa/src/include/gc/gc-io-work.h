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
#ifndef	_SALSA_GC_IO_WORK_H_
#define	_SALSA_GC_IO_WORK_H_
struct gc_io_work_parent;

#include "data-structures/list.h"
#include "libos/os-atomic.h"
#include "libos/os-debug.h"
#include "libos/os-lock.h"

#include "sto-ctlr/sto-capacity-mgr-parameters.h"
#define	SALSA_GC_MAX_IO_WORKERS	1024	/* in uspace sim don't
					 * limit ourselves,
					 * there are no buffers */
#define	SALSA_GC_BVS	(SALSA_GC_MAX_IO_WORKERS * (SALSA_MAX_GRAIN_SIZE / PAGE_SIZE))

struct sto_ctlr;

struct gc_io_work {
	struct cds_list_head      list;
	int                       err;
	u8                        stream;
	u8                        new_gen;
	u16                       new_lba_heat;
	u32                       grain_len;
	u64                       pba;
	u64                       new_pba;
	struct gc_io_work_parent *parent;
	struct sto_ctlr          *sc;
} __attribute__((aligned));

struct gc_io_work_parent {
	os_waitqueue_head_t waitq;
	os_atomic32_t     outstanding;
	os_atomic32_t     io_nr;
	os_atomic32_t     cnt;
	int               err;
	os_completion_t   completion;
	struct segment   *seg;
	struct gc_io_work workers[SALSA_GC_MAX_IO_WORKERS];
} __attribute__((aligned));

static inline void gc_io_work_parent_init(
	struct gc_io_work_parent *const parent,
	struct segment           *const seg)
{
	os_init_completion(&parent->completion);
	os_atomic32_zero(&parent->cnt);
	os_atomic32_zero(&parent->outstanding);
	parent->err = 0;
	parent->seg = seg;
}

static inline u64 gc_io_work_get_pos(
	struct gc_io_work_parent *const gciowp,
	struct gc_io_work        *const gciow)
{
	return (gciow - &gciowp->workers[0]);
}

#endif	/* _SALSA_GC_IO_WORK_H_ */
