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

#ifndef _SALSA_GFS_FLAGS_H_
#define _SALSA_GFS_FLAGS_H_

#include "generic/compiler.h" // STATIC_ASSERT
#include "sto-ctlr/scm-dev-properties.h" // SCM_PROP_LAST


/**
 * gfs (get_free_segment) flags encode segment allocation requirements
 */

/*
 *
 * If the hard property requirement bit is set, the allocation will fail if the
 * property cannot be satisfied. Note that a failure dones not necessarily mean
 * that there are not available segments with the required property. It might be
 * the case, for example, that the allocator cannot find such segments in an
 * efficient manner (e.g., it uses a single unordered-list for all segments).
 */
struct scm_gfs_flags {
	unsigned reloc:1;               // 0: normal/user, 1: relocation
	unsigned thin_prov:1;           // thin provisioning
	unsigned prop_hard:1;           // hard property allocation (see above)
	unsigned no_blocking:1;         // don't block, return an error

	// Initially, allocation flags provided @prop_set, a set of acceptable
	// properties for the allocator to respect. The allocator checks the
	// segment queues in order, for queues that match the given properties
	// with available segments.
	//
	// One way to use this functionality is for different streams to
	// allocate segments with different properties so that a stream can, for
	// example, allocate segments of better quality. A setup for doing this
	// might be:
	//  - stream 0 allocates from FLASH, and SMR
	//  - stream 1 allocates from SMR only
	//
	// This provides stream 0 with better storage, but there is the risk of
	// allocation problems if stream 1 wants to allocate segments but there
	// are no available SMR segments, even though there are available FLASH
	// segments.
	//
	// To solve this issue, we specify two sets, a preferred set and the
	// default set. The allocator will first test the preferred test, and if
	// this does not work, it will try the default set.
	struct scm_prop_set prop_set_preferred;
	struct scm_prop_set prop_set_default;

} __attribute__((packed));

#define scm_print_gfs_flags(x) scm_do_print_gfs_flags(x, __FUNCTION__)
void scm_do_print_gfs_flags(struct scm_gfs_flags flags, const char *callfn);

#endif // _SALSA_GFS_FLAGS_H_
