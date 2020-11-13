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
#ifndef _SALSA_RANDOM_H_
#define _SALSA_RANDOM_H_
#include <stdlib.h>
#include <time.h>

#define	os_get_random_int(x)	do {*(x) = rand();} while (0)
#define	os_get_random_bytes(x, b)	do {				\
		unsigned long i;					\
		srand48((unsigned long) time(NULL));			\
		for (i = 0; i < b / sizeof(unsigned long); ++i) {	\
			*(((unsigned long *) (x)) + i) = lrand48();	\
		} \
	} while (0)
#endif	/* _SALSA_RANDOM_H_ */
