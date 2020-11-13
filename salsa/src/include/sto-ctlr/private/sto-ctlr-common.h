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
#ifndef _SALSA_STO_CTLR_COMMON_H_
#define _SALSA_STO_CTLR_COMMON_H_


#include <asm-generic/errno.h>


#include "generic/compiler.h"
#include "libos/os-types.h"
#include "sto-ctlr/scm-dev-properties.h"
#include "sto-ctlr/sto-ctlr.h"
#include "sto-ctlr/sto-ctlr-parameters.h"
#include "sto-ctlr/private/sto-ctlr-stats.h"
#include "sto-ctlr/private/sto-capacity-mgr-common.h"

#define	SALSA_CACHE_MAX_DESTAGE_THREADS		1U
#define	LSA_SEGMENT_CACHE_FREE_CB_TIMEOUT_MS	5000U
#define	LSA_SEG_CACHE_WRITE_RETRIES		100U

struct sto_ctlr;
struct sto_ctlr_priv_fns;
struct sto_capacity_mgr;
struct gc;

// allocation stream (common part)
struct sto_ctlr_alloc_stream {
	struct sto_ctlr               *sc;
	struct scm_gfs_flags           gfs_flags; // segment allocation flags
	u8                             stream;    // stream id
};

struct sto_ctlr {
	struct sto_capacity_mgr        *const scm;
	struct gc                      *const gc;
	struct sto_ctlr                *const parent; /* back-pointer to parent hybrid controller, if SMALL */
	const u32                             page_size; /* bytes */
	const u32                             page_in_grains; /* # grains */
	const u32                             seg_size; /* # pages, might be
							   different than
							   scm->seg_size */

	// queue set that this controller uses
	struct scm_seg_queue_set             sc_qset;

	// allocators
	struct sto_ctlr_alloc_stream         *usr_streams[SALSA_MAX_STREAMS];
	u16                                  usr_streams_nr;
	struct sto_ctlr_alloc_stream         *rel_streams[SALSA_MAX_STREAMS];
	u16                                  rel_streams_nr;

	const u8                              ctlr_id;
	const enum ctlr_type                  ctlr_type;
	const u8                              page_shift_bits;
	const u16                             pages_per_md_pg;
	void                                 *private; /* set by the frontend */
	const struct sto_ctlr_priv_fns *const fns;
	write_seg_fn                          write_seg;
	get_pba_and_dev_fn                    get_pba_and_dev;

	// For disabling I/O (and saving/and restoring I/O functions)
	// (used for doing preconditioning)
	bool                                  io_disabled;
	write_seg_fn                          old_write_seg;
	segment_alloc_cb_fn                   old_seg_cbs[SCM_DEV_RANGES_MAX];

	const u64                             seed; /* session identifier */

	#define MAGIC_STREAM_MAP_SIZE  (100UL)
	bool                                  magic_stream_oracle;
	uint8_t                               magic_map_stream[MAGIC_STREAM_MAP_SIZE];
	struct sto_ctlr_stats                 scs;
} __attribute__((aligned));

struct sto_ctlr;
struct sto_ctlr_parameters;

struct sto_ctlr_priv_fns {
	 struct sto_ctlr_fns base; /* has to be first */
	 /**
	  * Constructor
	  */
	 int (*ctr) (
		 struct sto_ctlr_parameters *,
		 const struct sto_ctlr *,
		 struct sto_ctlr **);

	 /**
	  * Destructor
	  */
	 int (*dtr) (struct sto_ctlr *);

	 /**
	  * Restore process, will be called after ctr and before init_threads
	  */
	 int (*restore) (struct sto_ctlr_parameters *, struct sto_ctlr *);

	 /**
	  * Post register init
	  */
	 int (*init_post_register) (struct sto_ctlr *);

	 int (*quiesce_seg_reads) (struct sto_ctlr *, struct segment *);

	 /**
	  * for metadata support
	  */
	 u64 (*pba_to_byte) (const struct sto_ctlr *, u64);

	 u64 (*byte_to_pba) (const struct sto_ctlr *, u64);

	 /**
	  * Updating the reverse mapping table
	  */
	void (*update_pba_priv) (struct sto_ctlr *, u64, u32);

	u64 (*get_pba_priv) (struct sto_ctlr *, u64);

	void (*prefetch_pba_priv) (const struct sto_ctlr *, u64);

	 /**
	  * Init function
	  */
	 int (*init_threads) (
		 struct sto_ctlr *,
		 const struct sto_ctlr_parameters *);

	 /**
	  * Exit function
	  */
	 int (*exit_threads) (struct sto_ctlr *);

	 int (*unregister) (struct sto_ctlr *);

	/* start/stop preconditioning phase (no I/O) */
	int (*precond_start)(struct sto_ctlr *sc);
	int (*precond_finish)(struct sto_ctlr *sc);

};

/* implementers of sto_ctlr sub-objects should register their sc_fns
 * through a register fn */
int register_generic_sto_ctlr(struct sto_ctlr *);
int register_lsa_sto_ctlr(struct sto_ctlr *);
int register_lsa_dedup_async_sto_ctlr(struct sto_ctlr *);
int register_sto_ctlr_impossible(struct sto_ctlr *);

int register_lsa_simulation_sto_ctlr(struct sto_ctlr *);
#define register_lsa_iomem_sto_ctlr	register_sto_ctlr_impossible
#define register_lsa_nc_sto_ctlr	register_sto_ctlr_impossible
#define register_lsa_nc_pst_sto_ctlr	register_sto_ctlr_impossible
#define register_lsa_nc_lw_pst_sto_ctlr	register_sto_ctlr_impossible
#define register_lsa_hybrid_sto_ctlr	register_sto_ctlr_impossible
#define register_lsa_nc_small_sto_ctlr	register_sto_ctlr_impossible

u64 sto_ctlr_get_seg_size(struct sto_ctlr *);

__attribute__((pure))
static inline u8 ctlr_get_id(const struct sto_ctlr *const sc)
{
	 return sc->ctlr_id;
}

__attribute__((pure))
static inline enum ctlr_type ctlr_get_type(const struct sto_ctlr *const sc)
{
	 return sc->ctlr_type;
}

__attribute__((pure))
static inline u16 ctlr_get_pages_per_md_pg(const struct sto_ctlr *const sc)
{
	 return sc->pages_per_md_pg;
}

__attribute__((pure))
static inline u64 ctlr_get_seg_size(const struct sto_ctlr *const sc)
{
	 return sc->seg_size;
}

__attribute__((pure))
static inline u32 ctlr_get_page_size(const struct sto_ctlr *const sc)
{
	 return sc->page_size;
}

__attribute__((pure))
static inline u8 ctlr_get_page_shift_bits(const struct sto_ctlr *const sc)
{
	 return sc->page_shift_bits;
}

__attribute__((pure))
static inline struct segment *ctlr_pba_to_seg(
	 const struct sto_ctlr *const sc,
	 const u64                    pba)
{
	 const u64 grain_id = page_to_grain(pba, sc->page_in_grains);
	 return scm_grain_to_segment(sc->scm, grain_id);
}

__attribute__((pure))
static inline u64 ctlr_pba_to_seg_idx(
	 const struct sto_ctlr *const sc,
	 const u64                    pba)
{
	 const u64 grain_id = page_to_grain(pba, sc->page_in_grains);
	 return scm_grain_to_seg_idx(sc->scm, grain_id);
}

__attribute__((pure))
static inline u64 ctlr_get_seg_idx(
	 const struct sto_ctlr *const sc,
	 const struct segment  *const seg)
{
	 return scm_get_seg_idx(sc->scm, seg);
}

__attribute__((pure))
static inline struct sto_capacity_mgr *ctlr_get_scm(
	 const struct sto_ctlr *const sc)
{
	 return sc->scm;
}

void ctlr_update_pba_priv(struct sto_ctlr *sc, u64 pba, u32 private);

__attribute__((pure))
u64 ctlr_get_pba_priv(struct sto_ctlr *sc, u64 pba);

void ctlr_prefetch_pba_priv(const struct sto_ctlr *sc, u64 pba);

/* metadata persistence helper functions */

__attribute__((pure))
static inline u64 ctlr_get_seg_start_pba(
	 const struct sto_ctlr *const sc,
	 const struct segment  *const seg)
{
	const u64 seg_idx = ctlr_get_seg_idx(sc, seg);
	const u64 seg_size = ctlr_get_seg_size(sc);
	return seg_idx * seg_size;
}

__attribute__((pure))
static inline u64 ctlr_get_seg_startb(
	 const struct sto_ctlr *const sc,
	 const struct segment  *const seg)
{
	const u64 pba = ctlr_get_seg_start_pba(sc, seg);
	return pba << sc->page_shift_bits;
}


u64 sto_ctlr_pba_to_byte(const struct sto_ctlr *sc, u64 pba);
u64 sto_ctlr_byte_to_pba(const struct sto_ctlr *sc, u64 byte);
int sto_ctlr_restore(
	struct sto_ctlr_parameters *params,
	struct sto_ctlr            *sc);
void sto_ctlr_print_fns(const struct sto_ctlr *sc);

const char * sto_ctlr_name(const struct sto_ctlr *sc);

void
sto_ctlr_streams_init(struct sto_ctlr *sc,
                      const struct sto_ctlr_parameters *params,
                      struct sto_ctlr_alloc_stream *usr_streams,
                      struct sto_ctlr_alloc_stream *rel_streams);

// Stats

// NB: If you change this, you might want to have a look at
// scm_dev_props_stats_print() and sc_stats_op_page_range()
enum sc_op_type {
	SC_RD,
	SC_WR_USR,                    // full page, update out-of-place
	SC_WR_USR_UIP,                // full-page, update in-place
	SC_WR_USR_UNALIGNED,          // unaligned, read-modify-write
	SC_WR_USR_UNALIGNED_UIP,      // unaligned, update in-place
	SC_WR_RELOC,
	SC_OP_NR__,
};

STATIC_ASSERT(SC_OP_NR__ <= SCM_NSTATS, scm_nstats_too_small);

static inline void sto_ctlr_stats_init(struct sto_ctlr *sc)
{
	sto_ctlr_stats_init_(&sc->scs);
}

static inline void
sc_stats_op_set(struct sto_ctlr *sc, enum sc_op_type type, u64 val)
{
	struct sto_ctlr_stats *const scs = &sc->scs;
	switch (type) {
		case SC_WR_USR:
		sto_ctlr_stats_set_writes_(scs, val);
		break;

		case SC_WR_RELOC:
		sto_ctlr_stats_set_relocs_(scs, val);
		break;

		default:
		BUG_ON(true);
	}
}

static inline void
sc_stats_op_page_range(struct sto_ctlr *sc,
                       enum sc_op_type type,
                       u64 lba, u64 pba, u64 page_nr)
{
	struct sto_ctlr_stats *const scs = &sc->scs;
	struct sto_capacity_mgr *scm = sc->scm;
	struct scm_dev_range_prop *rp;
	unsigned rp_i;
	u64 off;

	switch (type) {
		case SC_RD:
		sto_ctlr_stats_add_reads_(scs, page_nr);
		break;

		case SC_WR_USR:
		sto_ctlr_stats_add_writes_(scs, page_nr);
		break;

		case SC_WR_USR_UNALIGNED:
		sto_ctlr_stats_add_writes_unaligned_(scs, page_nr);
		break;

		case SC_WR_RELOC:
		sto_ctlr_stats_add_relocates_(scs, page_nr);
		break;

		case SC_WR_USR_UIP:
		case SC_WR_USR_UNALIGNED_UIP:
		break;

		default:
		BUG_ON(true);
	}

	// update SCM property range stats
	if (pba == PBA_INVAL_ENTRY)
		return;
	off = pba << sc->page_shift_bits;
	rp = scm_dev_props_find(&scm->scm_dev_props, off);
	BUG_ON(rp == NULL);
	//MSG("rp=%p rp_i=%u pba=%"PRIu64 " off=%"PRIu64, rp, rp_i, pba, off);
	rp_i = rp - &scm->scm_dev_props.ranges[0];
	os_atomic64_add(&scm->prop_stats[rp_i][type], page_nr);
}

static inline void
sc_stats_op_page(struct sto_ctlr *sc,
                    enum sc_op_type type, u64 lba, u64 pba)
{
	return sc_stats_op_page_range(sc, type, lba, pba, 1);
}


__attribute__((pure))
static inline u64 ctlr_get_write_nr(struct sto_ctlr *const sc)
{
	struct sto_ctlr_stats *const scs = &sc->scs;
	u64 ret;

	ret  = sto_ctlr_stats_get_writes_(scs);
	ret += sto_ctlr_stats_get_writes_unaligned_(scs);
	ret += sto_ctlr_stats_get_relocates_(scs);

	return ret;
}

__attribute__((pure))
static inline u64 ctlr_get_read_nr(struct sto_ctlr *const sc)
{
	struct sto_ctlr_stats *const scs = &sc->scs;
	return sto_ctlr_stats_get_reads_(scs);
}

__attribute__((pure)) static inline
u64 sto_ctlr_get_nrelocates(struct sto_ctlr *const sc)
{
	return sto_ctlr_stats_get_relocates_(&sc->scs);
}

__attribute__((pure)) static inline
u64 sto_ctlr_get_user_writes(struct sto_ctlr *const sc)
{
	return sto_ctlr_stats_get_writes_(&sc->scs) +
		sto_ctlr_stats_get_writes_unaligned_(&sc->scs);
}

__attribute__((pure)) static inline
u64 sto_ctlr_get_user_writes_unaligned(struct sto_ctlr *const sc)
{
	return sto_ctlr_stats_get_writes_unaligned_(&sc->scs);
}

static inline void sto_ctlr_stats_pretty_print(struct sto_ctlr *const sc)
{
	struct sto_ctlr_stats *const scs = &sc->scs;

	const u64 user_writes = sto_ctlr_stats_get_writes_(scs);
	const u64 user_unaligned_writes = sto_ctlr_stats_get_writes_unaligned_(scs);
	const u64 relocates = sto_ctlr_stats_get_relocates_(scs);

	const u64 reloc_wasted __attribute__((unused)) =
		sto_ctlr_stats_get_reloc_wasted_write_nr(scs);
	const u64 unaligned_wasted __attribute__((unused)) =
		sto_ctlr_stats_get_write_unaligned_wasted_nr(scs);
	const u64 total_writes __attribute__((unused)) =
		user_writes + user_unaligned_writes + relocates;
	MSG("# TOTAL READS              =%"PRIu64,
		sto_ctlr_stats_get_reads_(scs));
	MSG("# USER WRITES              =%"PRIu64, user_writes);
	MSG("# USER UNALIGNED WRITES    =%"PRIu64, user_unaligned_writes);
	MSG("# TOTAL RELOCATES          =%"PRIu64, relocates);
	MSG("# TOTAL WRITES             =%"PRIu64, total_writes);
	MSG("# WASTED WRITES DUE TO RACE CONDITIONS:");
	MSG("# WRITES WASTED UNALIGNED  =%"PRIu64, unaligned_wasted);
	MSG("# RELOC WRITES WASTED      =%"PRIu64, reloc_wasted);
	MSG("# CACHE WRITE HITS         =%"PRIu64,
		sto_ctlr_stats_get_cache_write_hits(scs));
	MSG("# CACHE READ HITS          =%"PRIu64,
		sto_ctlr_stats_get_cache_read_hits(scs));
	MSG("# TOTAL FREED SEGMENTS     =%"PRIu64,
		sto_ctlr_stats_get_seg_recycled(scs));
}


int sto_ctlr_enable_io(struct sto_ctlr *sc);
int sto_ctlr_disable_io(struct sto_ctlr *sc);

#endif	/* _SALSA_STO_CTLR_COMMON_H_ */
