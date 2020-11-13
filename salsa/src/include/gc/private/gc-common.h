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
#ifndef	_SALSA_GC_COMMON_H_
#define	_SALSA_GC_COMMON_H_

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#include "data-structures/list.h"
#include "generic/compiler.h"
#include "libos/os-atomic.h"
#include "libos/os-lock.h"
#include "libos/os-thread.h"
#include "sto-ctlr/sto-ctlr.h"
#include "gc/gc.h"
#include "gc/gc-parameters.h"
#include "gc/gc-io-work.h"
#include "sto-ctlr/private/sto-capacity-mgr-common.h"

extern const char *const gc_string[];

#ifdef __KERNEL__
#define	GC_MAX_THREADS			1 /* (SALSA_MIN_REL_SEGMENTS) */
#else
#define	GC_MAX_THREADS			4 /* (SALSA_MIN_REL_SEGMENTS) */
#endif

#define	GC_WAIT_WATERMARK_TIMEOUT	20000U	/* ms */

// controller-specific information passed from the controller to the GC
struct gc_ctl_data {
	void                    *sc;
	// GC needs to call back to the controller for the relocation of
	// segments
	gc_reloc_fn_t            sc_reloc_fn;
	// queues that GC needs to work for this controller
	struct scm_seg_queue_set sc_qset;
};

struct gc_fns;
struct gc {
	struct sto_capacity_mgr  *const scm;
	const struct gc_fns      *const fns;
	os_thread_t                     gc[GC_MAX_THREADS];
	os_atomic32_t                   gc_done[GC_MAX_THREADS];
	os_atomic64_t                   valid_seg_nr;
	os_atomic64_t                   inval_page_nr;
	const u64                       seg_size;
	const u32                       thread_nr;
	os_atomic32_t                   exit;
	const enum gc_type              gc_type;
	os_waitqueue_head_t             low_watermark_waitq;
	const u64                       LOW_WATERMARK;
	const u64                       HIGH_WATERMARK;

	os_atomic32_t                   gc_active_threads;

	const struct gc_ctl_data        ctl_data[SALSA_MAX_CTLRS];

	struct gc_io_work_parent        gciowp[GC_MAX_THREADS];
} __attribute__((aligned(CACHE_LINE_SIZE)));

struct gc_thread_arg {
	struct gc *gc;
	u32   id;
};

struct gc_queue;

void gc_seg_released(
	struct gc       *gc,
	struct gc_queue *fifo,
	u32              relocs);

void gc_seg_selected(struct gc *gc, struct segment *seg);

struct gc_fns {
	int (*ctr) (
		const struct gc_parameters *const params,
		struct gc                 **const gc);
	int (*init) (struct gc *const gc);
	void (*init_stats) (struct gc *const gc);
	int (*dtr) (struct gc *gc);
	os_thread_fun_return (*run) (void *arg);
	void (*seg_write) (
		struct gc      *const gc,
		struct segment *const seg);
	void (*seg_relocate) (
		struct gc      *const gc,
		struct segment *const seg);
	void (*page_invalidate) (
		struct gc      *const gc,
		struct segment *const seg,
		const u32        grain_nr);
	void (*print_list_size) (const struct gc *const gc);
	void (*print_info) (const struct gc *const gc);
	u32 (*get_stream_nr) (const struct gc *const gc);
	u32 (*get_bin_nr) (const struct gc *const gc);
	int (*sto_ctlr_pre_dtr) (struct gc *const gc, const u8 ctlr_id);
};

/* Base: Greedy Window and Circular Buffer */
void gc_base_get_fns(
	const enum gc_type          type,
	const struct gc_fns **const fns_out);

/* Container Marker */
void gc_cm_get_fns(
	const enum gc_type          type,
	const struct gc_fns **const fns_out);

/* SALSA */
void gc_salsa_get_fns(
	const enum gc_type          type,
	const struct gc_fns **const fns_out);

#endif	/* _SALSA_GC_COMMON_H_ */
