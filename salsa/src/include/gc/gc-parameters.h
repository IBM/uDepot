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
#ifndef	_SALSA_GC_PARAMETERS_H_
#define	_SALSA_GC_PARAMETERS_H_

#include "libos/os-types.h"

struct sto_capacity_mgr;
enum gc_type {
 	GC_GREEDY_WINDOW            = 0,
	GC_CIRCULAR_BUFFER          = 1,
	GC_SALSA_3BIN                 = 4,
	GC_SALSA_NBIN                 = 5,
	GC_NBIN_XNP_INTRPT_SAFE     = 10,
	GC_NBIN_XNP_HEAT_SEG        = 11,
	GC_NBIN_HEAT_SEG            = 13,
	GC_NBIN_XNP_OVRW_HEAT_SEG   = 14,
	GC_NBIN_DYN_THRESHOLD       = 15,
	GC_NBIN_XNP_PATENT          = 16,
	GC_LAST                     = 17
};

struct gc_parameters {
	enum gc_type gc;
	u32     low_watermark; /* start gc when below low_watermark */
	u32     high_watermark; /* stop gc when above high_watermark */
	u32     xnp_pct;	     /* for XNP GCs */
	u32     xnp_hot_pct;   /* for XNP GCs and hot data */
	u32     bin_nr;	 /* for NBIN GCs, containers for CONTAINER_MARKER */
	u32     thread_nr;
	struct sto_capacity_mgr *scm;
};
#endif	/* _SALSA_GC_PARAMETERS_H_ */
