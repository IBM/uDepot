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
#ifndef	_SALSA_STO_CTLR_PARAMETERS_H_
#define	_SALSA_STO_CTLR_PARAMETERS_H_

#include "libos/os-types.h"

#include <sto-ctlr/scm-dev-properties.h> // scm_prop_set

struct sto_capacity_mgr;

enum ctlr_type {
	STO_CTLR_GENERIC          = 0,
	STO_CTLR_LSA              = 1,
	STO_CTLR_OBJECT_STORE     = 2,
	STO_CTLR_LSA_SIMULATION   = 3,
	STO_CTLR_LSA_IOMEM        = 4,
	STO_CTLR_LSA_BLACK        = 5,
	STO_CTLR_LSA_BLACK_PST    = 6, /* with persistent metadata */
	STO_CTLR_LSA_BLACK_PST_LW = 7, /* with persistent metadata and large writes */
	STO_CTLR_LSA_HYBRID       = 8,
	STO_CTLR_LSA_SMALL        = 9,
	STO_CTLR_LAST             = 10
};

struct block_device;

/* start, len, buf, private */
typedef int (*write_seg_fn) (u64, u64, const void *, void *);
typedef void (*get_pba_and_dev_fn) (void *, u64 *, struct block_device **);

struct sto_ctlr_parameters {
	enum ctlr_type           type;
	struct sto_capacity_mgr *scm;		 /* capacity manager */
	u64                      dev_size;     /* in bytes */
	u64                      logical_size; /* in bytes */
	u64                      seed;	       /* session identifier */
	u64                      uip_cache_size; /* unaligned access read-only cache size in bytes */
	u64                      small_logical_size; /* for hybrid, small page LSA, bytes*/
	u32                      page_size;    /* bytes */
	u32                      stream_nr;  /* for heat segregation */
	u32                      rel_stream_nr;  /* only for uspace sim */
	u32                      generation_nr;  /* for segregation */
	u32                      lba_heat_bits; /* for heat segregation */
	u32                      cache_size;	  /* # of segments */
	u32                      small_page_size; /* for hybrid, small page LSA, bytes*/
	u32                      large_page_size; /* for hybrid, large page LSA, bytes*/
	u16                      pages_per_md_pg; /* lsa pages per metadata pages  */
	u16                      small_pages_per_md_pg; /* lsa small pages per metadata pages */
	u8                       precondition;
	bool                     inject_read_delay;
	bool                     inject_write_delay;
	bool                     was_rebuild; /* true: controller was rebuild, false: clean start */
	bool                     dont_rebuild;
	/* Amount of rnd preconditioning following a full sequential
	 * write as a % of total device size */
	u32                      precond_rnd_percent;
	int                      overprovision;
	void                    *private;
	write_seg_fn             write_seg;
	get_pba_and_dev_fn       get_pba_and_dev;

	struct scm_prop_set      sc_prop_set;
	struct scm_prop_set      sc_small_prop_set;

	bool                     magic_stream_oracle;
};

#endif	/* _SALSA_STO_CTLR_PARAMETERS_H_ */
