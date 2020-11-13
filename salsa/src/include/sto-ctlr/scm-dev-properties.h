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

#ifndef	_SALSA_SCM_DEV_PROPERTIES_H_
#define	_SALSA_SCM_DEV_PROPERTIES_H_

#include <linux/version.h>

#include "libos/os-types.h"
#include "libos/os-debug.h"
#include "generic/compiler.h"

#include "sto-ctlr/scm-dev-addr.h"

/**
 * properties differentiate different classes of storage.
 *
 * Note that the ordering is important here: lower number suggests higher
 * quality storage.
 */
enum scm_property {
	// RAM-like performance
	SCM_PROP_NVRAM	   = 0,
	// Flash storage
	SCM_PROP_FLASH	   = 1,
	// SMR disks have conventional zones
	SCM_PROP_SMR_CONV  = 2,
	// Sequential zones in SMR disks. Different zones have different
	// performance characteristics due to their difference in rotational
	// speeds.
	SCM_PROP_SMR_SEQ1  = 3,
	SCM_PROP_SMR_SEQ2  = 4,
	#define SCM_PROP_SMR_SEQ_LAST SCM_PROP_SMR_SEQ2
	SCM_PROP_LAST	   = 5,
	/* SCM_PROP_HOLE ? */
} __attribute__((packed));

#define SCM_PROP_DEFAULT SCM_PROP_FLASH // flash is the default for now
#define SCM_PROP_FIRST	 SCM_PROP_NVRAM // used for iteration (SCM_PROP_{FIRST,LAST})

/**
 * Property sets, stored as bitmasks
 */

struct scm_prop_set {
	uint8_t mask__;
} __attribute((packed));

STATIC_ASSERT(8 > SCM_PROP_LAST, scm_prop_mask_not_enough_bits);

#define SCM_PROP_SET_ALL   ((struct scm_prop_set){.mask__ = ((1<<SCM_PROP_LAST)-1)})
#define SCM_PROP_SET_EMPTY ((struct scm_prop_set){.mask__ = 0x0})

static inline bool
scm_prop_set_contains(struct scm_prop_set s, enum scm_property p)
{
	return (s.mask__ & (1<<p)) != 0;
}

static inline bool
scm_prop_set_is_subset_of(struct scm_prop_set s1, struct scm_prop_set s2)
{
	// check if the mask is compatible, i.e., all bit set in
	// the first mask are set in the second mask
	return (s1.mask__ & s2.mask__) == s1.mask__;
}

static inline struct scm_prop_set
scm_prop_set_union(struct scm_prop_set s1, struct scm_prop_set s2)
{
	uint8_t m = s1.mask__ | s2.mask__;
	return (struct scm_prop_set){.mask__ = m};
}


static inline bool
scm_dev_prop_is_valid(enum scm_property p)
{
	#if defined(__cplusplus)
	// C++ issues a warning for p >= SCM_PROP_FIRST, because it's always
	// true
	return p < SCM_PROP_LAST;
	#else
	return (p >= SCM_PROP_FIRST) && (p < SCM_PROP_LAST);
	#endif
}

static inline bool
scm_dev_prop_is_smr_seq(enum scm_property p)
{
	return (p >= SCM_PROP_SMR_SEQ1) && (p <= SCM_PROP_SMR_SEQ_LAST);
}

// Property ranges notes (subject to change)
//
// The idea here is that we split the device into ranges based on its
// properties. There are no holes, so @r_len is sufficent. The device
// size is the sum of @r_len for all @dev_prop_ranges_nr
//
// These properties correspond to the "virtual" SCM device that is
// created by the backing devices. The capacity manager will split this
// (virtual) device into segments and create the appropriate metadata
// for each segment.
//
// It's up to the underlying implementations (e.g., RAID5) to ensure
// that the properties correspond to properties of the actual physical
// storage.

struct dm_dev;			// opaque pointer
struct zbc_device;		// another opaque pointer
struct scm_dev_range_prop;	// defined below

typedef int (*segment_alloc_cb_fn) (u64 offset, u64 len, struct scm_dev_range_prop *prop);

// properties for a single range
struct scm_dev_range_prop {

	u64		  r_len;   // bytes
	enum scm_property r_prop;  // properties

	// We keep the device here for:
	//  - the callback (r_seg_alloc_cb)
	//  - for inquiring what is the device for a particular vpba
	// TODO: add a struct scm_dev, included in device scm_dev_properties,
	// and have indices there for each range.
	struct dm_dev	    *r_dm_dev;
	segment_alloc_cb_fn  r_seg_alloc_cb;
	union {
		// for SCM_PROP_SMR_SEQ* areas
		struct {
			u32 zone_len;
		} r_smr_seq;
	};

	// This is a hack, and should be eventually removed. For now, it is used
	// to specify that a only some	segments (typically distributed across
	// all of the range) will actually be inserted into the queue. So
	// segments for @r_real_len are created, but only segments for @r_len
	// are inserted into the queue
	// If r_real_len_ is 0, it means that this value is not used
	u64 r_real_len_;
};

// helpers for building scm_dev_range_pro structures:
//  - includes .r_real_len and .r_dm_dev
#define SCM_DEV_RPROP__(l,p,r,d) \
 ((struct scm_dev_range_prop) {	 \
	.r_len = l,		 \
	.r_prop = p,		 \
	.r_real_len_ = r,	 \
	.r_dm_dev = d,		 \
})
//  - .r_real_len == 0
#define SCM_DEV_RPROP_(l,p,d) SCM_DEV_RPROP__(l,p,0,d)
// - .r_real_len ==  0 && r_dm_dev == NULL
#define SCM_DEV_RPROP(l,p) SCM_DEV_RPROP_(l,p,NULL)

#define SCM_DEV_RPROP_SMR__(l,p,r,d,z) ({   \
	struct scm_dev_range_prop _r;	    \
	_r.r_len = l;			    \
	_r.r_prop = p;			    \
	_r.r_real_len_ = r;		    \
	_r.r_dm_dev = d;		    \
	_r.r_smr_seq.zone_len = z;	    \
					    \
	_r;				    \
})
//  - .r_real_len == 0
#define SCM_DEV_RPROP_SMR_(l,p,d,z) SCM_DEV_RPROP_SMR__(l,p,0,d,z)
// - .r_real_len ==  0 && r_dm_dev == NULL
#define SCM_DEV_RPROP_SMR(l,p,z) SCM_DEV_RPROP_SMR_(l,p,NULL,z)

// properties for the whole device
struct scm_dev_properties {
	#define	SCM_DEV_RANGES_MAX 16
	struct scm_dev_range_prop ranges[SCM_DEV_RANGES_MAX];
	u32 ranges_nr;
};

static inline void
scm_dev_props_reset(struct scm_dev_properties *props)
{
	props->ranges_nr = 0;
}

static inline bool
scm_dev_props_valid(const struct scm_dev_properties *props)
{
	return props->ranges_nr > 0;
}

void
scm_dev_props_set_default(struct scm_dev_properties *props, u64 dev_bytes
			  /* struct dm_dev *dm_dev */);

// check if two ranges are compatbile. This is used for merging in
// scm_dev_props_add_range()
static inline bool
scm_dev_range_props_compatible(const struct scm_dev_range_prop *p1,
			       const struct scm_dev_range_prop *p2) {

	// different storage: not compatible
	if (p1->r_prop != p2->r_prop)
		return false;

	// different device: not compatible
	if (p1->r_dm_dev != p2->r_dm_dev)
		return false;

	// If ranges have the same property, and they are SMR sequential zones,
	// check if they have the same zone length
	switch (p1->r_prop) {
		case SCM_PROP_FLASH:
		case SCM_PROP_SMR_CONV:
		return true;

		case SCM_PROP_SMR_SEQ1:
		case SCM_PROP_SMR_SEQ2:
		return p1->r_smr_seq.zone_len == p2->r_smr_seq.zone_len;

		default:
		BUG_ON(true || "Uknown property");
	}
	return false;
}

/*
 * measure size:
 *   @set:  properties specified by set
 *   @grain: measure size in granularity of @grain
 *
 * NB: In general,
 *  scm_dev_props_size_grain_set(ps, G, 0)
 * is not equal to
 *  scm_dev_props_size_grain_set(ps, 1, 0) / G
 *
 * This is the more generic version, there are common wrappers below.
 */
u64 scm_dev_props_size_grain_set(
	const struct scm_dev_properties *props, u64 grain, struct scm_prop_set set);

// return the size of the device, excluding the areas with properties
// included in the set
static inline u64 scm_dev_props_size_set(const struct scm_dev_properties *ps, struct scm_prop_set set) {
	return scm_dev_props_size_grain_set(ps, 1, set);
}

// return the size of the device in @grains granularity
static inline u64 scm_dev_props_size_grain(const struct scm_dev_properties *ps, u64 grain) {
	return scm_dev_props_size_grain_set(ps, grain, SCM_PROP_SET_ALL);
}

// return the (total) size of the device
static inline u64 scm_dev_props_size(const struct scm_dev_properties *ps) {
	return scm_dev_props_size_grain_set(ps, 1, SCM_PROP_SET_ALL);
}

// same as above, but using ->r_real_len_
u64 scm_dev_props_real_size_grain_set(
	const struct scm_dev_properties *props, u64 grain, struct scm_prop_set set);
// helpers
static inline u64 scm_dev_props_real_size_set(const struct scm_dev_properties *ps, struct scm_prop_set set) {
	return scm_dev_props_real_size_grain_set(ps, 1, set);
}
static inline u64 scm_dev_props_real_size_grain(const struct scm_dev_properties *ps, u64 grain) {
	return scm_dev_props_real_size_grain_set(ps, grain, SCM_PROP_SET_ALL);
}
static inline u64 scm_dev_props_real_size(const struct scm_dev_properties *ps) {
	return scm_dev_props_real_size_grain_set(ps, 1, SCM_PROP_SET_ALL);
}

// reduce the aggregate size of @props by truncating (at the end)
void scm_dev_props_truncate_grain(struct scm_dev_properties *props, u64 grain, u64 max_grain);
static inline void
scm_dev_props_truncate(struct scm_dev_properties *props, u64 max_len)
{
	return scm_dev_props_truncate_grain(props, 1, max_len);
}


u64  scm_dev_props_copy(struct scm_dev_properties *dst, const struct scm_dev_properties *src, u64 max_len);
// add @range to @props, merging it with he last one if possible
void scm_dev_props_add_range_prop(struct scm_dev_properties *props, struct scm_dev_range_prop new_r);
void scm_dev_props_append(struct scm_dev_properties *p1, const struct scm_dev_properties *p2);

// full copy @src to @dst
static inline void
scm_dev_props_copy_full(struct scm_dev_properties *dst,
			const struct scm_dev_properties *src)
{
	u64 max_len = scm_dev_props_size(src);
	scm_dev_props_copy(dst, src, max_len);
}

void scm_dev_props_print(const struct scm_dev_properties *props, const char *prefix);
const char *scm_dev_prop_getname(enum scm_property p);
const char *scm_dev_prop_getname_short(enum scm_property p);

void scm_dev_props_parse(struct scm_dev_properties *props, const char *str);

// find the range matching an offset. Returns NULL if offset not within the
// device properties.
struct scm_dev_range_prop *
scm_dev_props_find(struct scm_dev_properties *ps, u64 offset);

// scale functions below are not used, and C++ does like the first one, so
// comment them out.
#if 0
void
scm_dev_props_scale__(const struct scm_dev_properties *p, u64 new_len,
		      u64 new_ranges[static SCM_DEV_RANGES_MAX]);

// it's OK if p == p_new
static inline void
scm_dev_props_scale(const struct scm_dev_properties *p, u64 new_len,
		    struct scm_dev_properties *p_new)
{
	// not implemented
	BUG_ON(true);
	// Issues:
	//  - real_len: we probably need to scale them as well?
	//  - ranges with len = 0? we might want to shift ranges and reduce
	//  ->ranges_nr
}
#endif


// vpba -> pba
//
// find the underlying device and offset (pba) of an address in the SCM device
// (vpba)
//
// Implementation assumption: underlying devices have consecutive ranges, and
// their offset starts at 0.
scm_pba_t
scm_dev_linear_map(struct scm_dev_properties *ps, scm_vpba_t vpba);

static inline bool
scm_prop_set_equal(struct scm_prop_set s1, struct scm_prop_set s2)
{
	return s1.mask__ == s2.mask__;
}

static inline bool
scm_prop_set_is_empty(struct scm_prop_set s)
{
	return scm_prop_set_equal(s, SCM_PROP_SET_EMPTY);
}


#endif	/* _SALSA_SCM_DEV_PROPERTIES_H_ */

