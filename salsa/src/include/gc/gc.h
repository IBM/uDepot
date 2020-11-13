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
#ifndef	_SALSA_GC_H_
#define	_SALSA_GC_H_

#include "libos/os-types.h"
#include "sto-ctlr/scm-seg-queues.h"

struct gc;
struct gc_io_work_parent;
struct segment;
struct sto_ctlr_parameters;
struct gc_parameters;
struct sto_capacity_mgr;
struct sto_ctlr;

typedef int
(*gc_reloc_fn_t) (void *, struct gc_io_work_parent *);

int gc_ctr(
	const struct gc_parameters *const params,
	struct sto_capacity_mgr    *const scm,
	struct gc                 **const gc);

int gc_init_threads(struct gc *const gc);

int gc_exit_threads(struct gc *const gc);

int gc_dtr(struct gc *gc);

int gc_register_ctlr(
	struct gc *const    gc,
	const u8       ctlr_id,
	const gc_reloc_fn_t gc_reloc_fn,
	struct scm_seg_queue_set gc_qset);

int gc_unregister_ctlr(
	struct gc *const gc,
	const u8    ctlr_id);

int gc_sto_ctlr_pre_dtr(
	struct gc *const gc,
	const u8    ctlr_id);

void gc_reset_stats(struct gc *const gc);

u32 gc_get_ncntnrs(const struct gc *const gc);

u32 gc_get_bin_nr(const struct gc *const gc);

void gc_seg_destaged(
	struct gc      *const gc,
	struct segment *const seg);

void gc_seg_relocated(
	struct gc      *const gc,
	struct segment *const seg);

void gc_page_invalidate(
	struct gc      *const gc,
        struct segment *const seg,
	const u32        grain_nr);

void gc_page_invalidate_reloc(
	struct gc      *const gc,
	struct segment *const seg,
	const u32        grain_nr);

void gc_assign_heat(
	struct gc      *const gc,
	struct segment *const seg,
	const u32        heat_suggestion);

u32 gc_retrieve_heat(
	struct gc      *const gc,
	struct segment *const seg);

void gc_print_list_info(struct gc *const gc);

void gc_print_info(const struct gc *const gc);

void gc_check_free_segment_low_watermark(
	struct gc *const gc,
	const u64   nfree_segs);

u32 gc_high_wm_get(const struct gc *gc);
u32 gc_low_wm_get(const struct gc *gc);

u32 gc_get_type(const struct gc *gc);

bool gc_is_active(const struct gc *gc);

/* check if GC should start running (low watermark) */
bool gc_check_start(const struct gc *gc);

/* check if GC is running in critical mode (1 free segment)*/
bool gc_check_critical(const struct gc *gc);

/* check if GC should continue running (high watermark) */
bool gc_check_continue(const struct gc *gc);

void gc_precondition_start(struct gc *gc, u32 streams);

void gc_precondition_finish(struct gc *gc, u32 streams);
#endif	/* _SALSA_GC_H_ */
