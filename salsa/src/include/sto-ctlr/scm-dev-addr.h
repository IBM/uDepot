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

 #ifndef  _SALSA_SCM_DEV_ADDR_H_
 #define  _SALSA_SCM_DEV_ADDR_H_

struct dm_dev;

struct scm_vpba {
	u64 addr; // bytes
};
typedef struct scm_vpba scm_vpba_t;

#define SCM_VPBA(x) ((struct scm_vpba){.addr = x})

struct scm_pba {
	struct dm_dev *dm_dev;
	u64 addr; // bytes
};
typedef struct scm_pba scm_pba_t;

#define SCM_PBA(d,a) ((struct scm_pba){.dm_dev = d, .addr = a})

 #endif   // _SALSA_SCM_DEV_ADDR_H_
