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
#ifndef _SALSA_MALLOC_H_
#define _SALSA_MALLOC_H_

#include <stdlib.h>
#include "generic/compiler.h"

#define os_malloc(_SIZE)	({		       \
			void *ptr = NULL;	       \
			int rc = posix_memalign(&ptr, CACHE_LINE_SIZE, _SIZE); \
			if (0 != rc)					\
				ptr = NULL;				\
			ptr;						\
		})

#define os_vmalloc(_SIZE)	({		       \
			void *ptr = NULL;	       \
			int rc = posix_memalign(&ptr, 4096, _SIZE); \
			if (0 != rc)					\
				ptr = NULL;				\
			ptr;						\
		})

#define os_free(_ADDRP)		free(_ADDRP)
#define os_vfree(_ADDRP)	free(_ADDRP)

#endif	/* _SALSA_MALLOC_H_ */
