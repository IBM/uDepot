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
#ifndef _SALSA_STRING_H_
#define _SALSA_STRING_H_

#include <string.h>
#include <stdlib.h>
#define	os_strtol(x, y, z)	strtol(x, y, z)
#define	os_strtoll(x, y, z)	strtoll(x, y, z)
#define os_strtoul(x, y, z)	strtoul(x, y, z)
#define os_strncasecmp(x, y, z)	strncasecmp(x, y, z)

#endif	/* _SALSA_STRING_H_ */
