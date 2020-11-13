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

#ifndef	_SALSA_STO_CAPACITY_MGR_H_
#define	_SALSA_STO_CAPACITY_MGR_H_

#include "sto-ctlr/scm-dev-properties.h" // struct scm_prop_set

struct sto_capacity_mgr;
struct scm_parameters;

int sto_capacity_mgr_ctr(
	struct scm_parameters    *params,
	struct sto_capacity_mgr **scm);
int sto_capacity_mgr_dtr(struct sto_capacity_mgr *scm);

int sto_capacity_mgr_init_threads(struct sto_capacity_mgr *scm);
int sto_capacity_mgr_exit_threads(struct sto_capacity_mgr *scm);

u64 sto_capacity_mgr_get_capacity(struct sto_capacity_mgr *scm);
u64 sto_capacity_mgr_get_capacity_set(struct sto_capacity_mgr *scm, struct scm_prop_set set);

u64 sto_capacity_mgr_get_grain_size(struct sto_capacity_mgr *scm);

void scm_dump_stats(const struct sto_capacity_mgr *);
void scm_reset_stats(struct sto_capacity_mgr *);
#endif	/* _SALSA_STO_CAPACITY_MGR_H_ */
