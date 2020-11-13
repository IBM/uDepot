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

#ifndef SCM_USR_HELPERS_H__
#define SCM_USR_HELPERS_H__


/*
 * SCM helper utilities for userspace
 */

struct scm_parameters;

// initialize SCM parameters based on arguments
int
scm_usr_init_cfg(char **const argv, const int argc, struct scm_parameters *scm_cfg);

#endif /* SCM_USR_HELPERS_H__ */
