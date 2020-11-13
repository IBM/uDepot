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
#ifndef _SALSA_STO_CTLR_STATS_H_
#define _SALSA_STO_CTLR_STATS_H_

#include "generic/compiler.h"
#include "libos/os-atomic.h"
#include "libos/os-debug.h"
#include "libos/os-string.h"

// functions ending in '_' are not to be used from code other than
// sto-ctr-common

/* TODO: make these per cpu vars and gather the sum in the end */
struct sto_ctlr_stats {
	os_atomic64_t write_nr;
	os_atomic64_t write_nr_unaligned;
	os_atomic64_t read_nr;
	os_atomic64_t cache_write_hit_nr;
	os_atomic64_t cache_read_hit_nr;
	os_atomic64_t write_unaligned_wasted_nr;
	/*
	 * Garbage collection stats
	 */
	os_atomic64_t relocate_nr;
	os_atomic64_t reloc_wasted_write_nr;
	os_atomic64_t segment_allocate_nr;
	os_atomic64_t segment_recycles_nr;
};

static inline void sto_ctlr_stats_init_(struct sto_ctlr_stats *const scs)
{
	memset(scs, 0, sizeof(*scs));
}


static inline void sto_ctlr_stats_inc_writes_(struct sto_ctlr_stats *const scs)
{
	os_atomic64_inc(&scs->write_nr);
}

static inline void sto_ctlr_stats_add_writes_(
	struct sto_ctlr_stats *const scs,
	const u64                    nr)
{
	os_atomic64_add(&scs->write_nr, nr);
}

static inline void sto_ctlr_stats_set_writes_(
	struct sto_ctlr_stats *const scs,
	const u64                    nr)
{
	os_atomic64_set(&scs->write_nr, nr);
}

static inline u64 sto_ctlr_stats_dec_writes_(struct sto_ctlr_stats *scs)
{
	assert(0 < os_atomic64_read(&scs->write_nr));
	return os_atomic64_dec_return(&scs->write_nr);
}

static inline u64 sto_ctlr_stats_get_writes_(
	struct sto_ctlr_stats *const scs)
{
	return os_atomic64_read(&scs->write_nr);
}

static inline void sto_ctlr_stats_inc_writes_unaligned_(
	struct sto_ctlr_stats *const scs)
{
	os_atomic64_inc(&scs->write_nr_unaligned);
}

static inline void sto_ctlr_stats_add_writes_unaligned_(
	struct sto_ctlr_stats *const scs,
	const u64                    nr)
{
	os_atomic64_add(&scs->write_nr_unaligned, nr);
}

static inline u64 sto_ctlr_stats_dec_writes_unaligned_(
	struct sto_ctlr_stats *const scs)
{
	assert(0 < os_atomic64_read(&scs->write_nr_unaligned));
	return os_atomic64_dec_return(&scs->write_nr_unaligned);
}

static inline u64 sto_ctlr_stats_get_writes_unaligned_(
	struct sto_ctlr_stats *const scs)
{
	return os_atomic64_read(&scs->write_nr_unaligned);
}

static inline void sto_ctlr_stats_inc_relocates_(
	struct sto_ctlr_stats *const scs)
{
	os_atomic64_inc(&scs->relocate_nr);
}

static inline void sto_ctlr_stats_add_relocates_(
	struct sto_ctlr_stats *const scs, u64 nr)
{
	os_atomic64_add(&scs->relocate_nr, nr);
}

static inline void sto_ctlr_stats_inc_reloc_wasted_writes(
	struct sto_ctlr_stats *const scs)
{
	os_atomic64_inc(&scs->reloc_wasted_write_nr);
}

static inline u64 sto_ctlr_stats_get_reloc_wasted_write_nr(
	struct sto_ctlr_stats *const scs)
{
	return os_atomic64_read(&scs->reloc_wasted_write_nr);
}

static inline void sto_ctlr_stats_set_relocs_(
	struct sto_ctlr_stats *const scs,
	const u64                    nr)
{
	os_atomic64_set(&scs->relocate_nr, nr);
}

static inline void sto_ctlr_stats_inc_unaligned_wasted_writes(
	struct sto_ctlr_stats *const scs)
{
	os_atomic64_inc(&scs->write_unaligned_wasted_nr);
}

static inline u64 sto_ctlr_stats_get_write_unaligned_wasted_nr(
	struct sto_ctlr_stats *const scs)
{
	return os_atomic64_read(&scs->write_unaligned_wasted_nr);
}

static inline u64 sto_ctlr_stats_get_relocates_(
	struct sto_ctlr_stats *const scs)
{
	return os_atomic64_read(&scs->relocate_nr);
}

static inline void sto_ctlr_stats_add_reads_(
	struct sto_ctlr_stats *const scs,
	const u64 nr)
{
	os_atomic64_add(&scs->read_nr, nr);
}

static inline u64 sto_ctlr_stats_get_reads_(
	struct sto_ctlr_stats *const scs)
{
	return os_atomic64_read(&scs->read_nr);
}

static inline void sto_ctlr_stats_inc_recycled(
	struct sto_ctlr_stats *const scs)
{
	os_atomic64_inc(&scs->segment_recycles_nr);
}

static inline u64 sto_ctlr_stats_get_seg_recycled(
	struct sto_ctlr_stats *const scs)
{
	return os_atomic64_read(&scs->segment_recycles_nr);
}

static inline void sto_ctlr_stats_inc_cache_write_hits(
	struct sto_ctlr_stats *const scs)
{
	os_atomic64_inc(&scs->cache_write_hit_nr);
}

static inline u64 sto_ctlr_stats_dec_cache_write_hits(
	struct sto_ctlr_stats *scs)
{
	assert(0 < os_atomic64_read(&scs->cache_write_hit_nr));
	return os_atomic64_dec_return(&scs->cache_write_hit_nr);
}

static inline u64 sto_ctlr_stats_get_cache_write_hits(
	struct sto_ctlr_stats *const scs)
{
	return os_atomic64_read(&scs->cache_write_hit_nr);
}

static inline void sto_ctlr_stats_inc_cache_read_hits(
	struct sto_ctlr_stats *const scs)
{
	os_atomic64_inc(&scs->cache_read_hit_nr);
}

static inline u64 sto_ctlr_stats_dec_cache_read_hits(
	struct sto_ctlr_stats *scs)
{
	assert(0 < os_atomic64_read(&scs->cache_read_hit_nr));
	return os_atomic64_dec_return(&scs->cache_read_hit_nr);
}

static inline u64 sto_ctlr_stats_get_cache_read_hits(
	struct sto_ctlr_stats *const scs)
{
	return os_atomic64_read(&scs->cache_read_hit_nr);
}

#endif	/* _SALSA_STO_CTLR_STATS_H_ */
