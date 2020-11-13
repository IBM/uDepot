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
#ifndef	_SALSA_COMMON_H_
#define	_SALSA_COMMON_H_

#include "libos/os-types.h"

#include "data-structures/list.h"

struct region {
	u64 start;
	u64 length;
};

static inline u64
region_end(struct region *r)
{
	return r->start + r->length;
}

struct region_node {
	struct region        region;
	struct cds_list_head list;
	bool                 duplicate;
};

static inline int region_equal(
	const struct region *const r1,
	const struct region *const r2)
{
	return !(r1->start != r2->start || r1->length != r2->length);
}


static inline int region_cmp(
	const struct region *const r1,
	const struct region *const r2)
{
	if (r1->start < r2->start) {
		return -1;
	} else if (r1->start > r2->start) {
		return 1;
	} else {
		if (r1->length < r2->length) {
			return -1;
		} else if (r1->length > r2->length) {
			return 1;
		} else {
			return 0;
		}
	}
}
#endif  /* _SALSA_COMMON_H_ */
