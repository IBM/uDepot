/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop:4 shiftwidth:4:

#ifndef CPUMASK_UTILS_H__
#define CPUMASK_UTILS_H__

// place cpu mask to given buffer in a hex form
// returns: 0 or errno in case of error
int cpumask_to_str(cpu_set_t *cpuset,
                   size_t     cpuset_size, /* bytes */
                   char      *buff,
                   size_t     buff_len);

#endif

