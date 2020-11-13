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

#ifndef	__SALSA_PARSE_STO_ARGS_H__
#define	__SALSA_PARSE_STO_ARGS_H__

#include "libos/os-types.h"

struct scm_parameters;
struct sto_ctlr_parameters;
struct gc_parameters;
/*
 ***********************************
 * Default configuration values
 ***********************************
 */

void reset_gc_config (
	struct gc_parameters *const gc_cfg);

void reset_scm_config (
	struct scm_parameters *const scm_cfg);

void reset_storage_config (
	struct sto_ctlr_parameters *const sto_cfg);

void set_private_storage_config (
	struct sto_ctlr_parameters *const sto_cfg,
	void                             *priv);

int parse_scm_args(
	struct scm_parameters *const scm_cfg,
	const unsigned int           argc,
	char                 **const argv,
	char                 **const err);

int parse_storage_args(
	struct sto_ctlr_parameters *const sto_cfg,
	const unsigned int                argc,
	char                      **const argv,
	char                      **const err);

int check_storage_params(
	struct sto_ctlr_parameters *const sto_cfg,
	char                      **const err);

int check_sto_capacity_mgr_parameters(
	const struct scm_parameters *const scm_cfg);

int check_ctlr_storage_parameters(
	const struct sto_ctlr_parameters *const ctlr_cfg);

int check_lsa_storage_parameters(
        const struct sto_ctlr_parameters *const lsa_cfg);

int set_storage_params_dev_size(
	struct sto_ctlr_parameters *const sto_cfg,
	u64                          dev_size, /* # sectors */
	char                      **const err);

#endif	/* __SALSA_PARSE_STO_ARGS_H__ */
