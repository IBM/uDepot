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

#include "sto-ctlr/scm-dev-properties.h"
#include "sto-ctlr/scm-dev-addr.h"

#include "generic/compiler.h"
#include "libos/os-string.h"
#include "libos/os-malloc.h"

static void test_compare(struct scm_dev_properties *r, struct scm_dev_properties *e);

// helper strings
static const char *scm_property_str[] __attribute__((unused)) = {
	[SCM_PROP_NVRAM]    = "SCM_PROP_NVRAM",
	[SCM_PROP_FLASH]    = "SCM_PROP_FLASH",
	[SCM_PROP_SMR_CONV] = "SCM_PROP_SMR_CONV",
	[SCM_PROP_SMR_SEQ1] = "SCM_PROP_SMR_SEQ1",
	[SCM_PROP_SMR_SEQ2] = "SCM_PROP_SMR_SEQ2",
	//[SCM_PROP_SMR_SEQ3] = "SCM_PROP_SMR_SEQ3",
	//[SCM_PROP_SMR_SEQ4] = "SCM_PROP_SMR_SEQ4",
	[SCM_PROP_LAST]     = "SCM_PROP_LAST",
};


const char *
scm_dev_prop_getname(enum scm_property p)
{
	if (!scm_dev_prop_is_valid(p))
		return "----INVALID SCM PROPERTY----";
	return scm_property_str[p];
}
const char *
scm_dev_prop_getname_short(enum scm_property p)
{
	if (!scm_dev_prop_is_valid(p))
		return "----INVALID SCM PROPERTY----";

	return scm_property_str[p] + strlen("SCM_PROP_");
}

// copy @src to @dst so that the @dst has no more than @max_len bytes  size
//  This should have the same effect as:
//    scm_dev_props_copy_full()
//    scm_dev_props_truncate()
u64
scm_dev_props_copy(struct scm_dev_properties *dst,
                   const struct scm_dev_properties *src,
                   u64 max_len)
{
	unsigned i;
	u64 rem_len;

	scm_dev_props_reset(dst);
	rem_len = max_len;
	for (i=0; i<src->ranges_nr && rem_len > 0; i++) {
		u64 len = min2(src->ranges[i].r_len, rem_len);
		// copy everything, and then adjust len
		dst->ranges[i] = src->ranges[i];
		dst->ranges[i].r_len = len;
		rem_len -= len;
	}
	dst->ranges_nr = i;

	return max_len - rem_len;
}

void
scm_dev_props_set_default(struct scm_dev_properties *props, u64 dev_bytes
                          /* struct dm_dev *dm_dev */)
{
	props->ranges_nr = 1;
	props->ranges[0] = (struct scm_dev_range_prop){
		.r_len  = dev_bytes,
		.r_prop = SCM_PROP_DEFAULT,
		.r_dm_dev = NULL,
		.r_seg_alloc_cb = NULL};
}

static __attribute__((unused))
void scm_dev_prop_copy_test(void) {

	unsigned i;
	struct {
		const char *cp_src; // source scm_dev_properties (in string)
		u64         cp_len; // copy len
		const char *cp_res; // expected result
	} tests[] = {{
		.cp_src = "100:200", .cp_len = 150, .cp_res = "100:50",
	},{
		NULL
	}};

	for (i=0;;i++) {
		const char *cp_src = tests[i].cp_src;
		u64         cp_len = tests[i].cp_len;
		const char *cp_res = tests[i].cp_res;

		struct scm_dev_properties src, dst, expected;

		scm_dev_props_reset(&src);
		scm_dev_props_reset(&dst);
		scm_dev_props_reset(&expected);

		if (cp_src == NULL)
			break;
		MSG("Testing copy: %s %"PRIu64" => %s", cp_src, cp_len, cp_res);

		scm_dev_props_parse(&src, cp_src);
		scm_dev_props_parse(&expected, cp_res);

		scm_dev_props_copy(&dst, &src, cp_len);

		test_compare(&dst, &expected);
	}
}

void
scm_dev_props_truncate_grain(struct scm_dev_properties *props, u64 grain, u64 max_grains)
{
	unsigned i;
	u64 rem_grains, *ri_len;

	rem_grains = max_grains;
	for (i=0; i<props->ranges_nr && rem_grains > 0; i++) {
		ri_len = &props->ranges[i].r_len;
		if (*ri_len > (grain*rem_grains)) {
			*ri_len = grain*rem_grains;
			props->ranges_nr = i + 1;
			return;
		}
		rem_grains -= (*ri_len / grain);
	}
}

static __attribute__((unused)) void
scm_dev_prop_truncate_test(void) {

	unsigned i;
	struct {
		const char *tr_src;    // source scm_dev_properties (in string)
		u64         tr_grains; //
		const char *tr_res;    // expected result
		u64         tr_grain;
	} tests[] = {
		{ .tr_grain = 1, .tr_src = "100:200",     .tr_grains = 300, .tr_res = "100:200" },
		{ .tr_grain = 1, .tr_src = "100:200",     .tr_grains = 150, .tr_res = "100:50"  },
		{ .tr_grain = 1, .tr_src = "100.1:200.0", .tr_grains = 50,  .tr_res = "50.1"    },
		{ .tr_grain = 4, .tr_src = "5:30",        .tr_grains = 3,   .tr_res = "5:8"    },
		{ NULL }
	};

	for (i=0;;i++) {
		const char *tr_src = tests[i].tr_src;
		const char *tr_res = tests[i].tr_res;
		u64         tr_grains = tests[i].tr_grains;
		u64         tr_grain = tests[i].tr_grain;

		struct scm_dev_properties src, expected;
		if (tr_src == NULL)
			break;
		MSG("Testing truncate: %s %"PRIu64" (grain:%"PRIu64") => %s",
			tr_src, tr_grains, tr_grain, tr_res);

		scm_dev_props_reset(&src);
		scm_dev_props_reset(&expected);

		scm_dev_props_parse(&src, tr_src);
		scm_dev_props_parse(&expected, tr_res);

		scm_dev_props_truncate_grain(&src, tr_grain, tr_grains);

		test_compare(&src, &expected);
	}
}

void
scm_dev_props_add_range_prop(struct scm_dev_properties *props,
                             struct scm_dev_range_prop  new)
{
	u32 idx;
	struct scm_dev_range_prop *last;

	if (props->ranges_nr == 0) {
		props->ranges[0] = new;
		props->ranges_nr = 1;
		return;
	}

	idx = props->ranges_nr - 1;
	last = props->ranges + idx;
	if (scm_dev_range_props_compatible(last, &new)) {
		last->r_len += new.r_len;
	} else {
		idx++;
		BUG_ON(idx >= SCM_DEV_RANGES_MAX);
		props->ranges[idx] = new;
		props->ranges_nr++;
	}
}

// append @p2 to @p1
void
scm_dev_props_append(struct scm_dev_properties *p1, const struct scm_dev_properties *p2)
{
	unsigned i;
	for (i=0; i<p2->ranges_nr; i++) {
		scm_dev_props_add_range_prop(p1, p2->ranges[i]);
	}
}

static __attribute__((unused)) void
scm_dev_prop_append_test(void) {

	unsigned i;
	struct {
		const char *p1;
		const char *p2;
		const char *r;
	} ts[] = {
		{ .p1 = "100", .p2 = "200", .r = "300" },
		{ .p1 = "100.1", .p2 = "100.0", .r = "100.1:100.0" },
		{ .p1 = "100@1", .p2 = "200@2", .r = "100@1:200@2" },
		{NULL}
	};

	for (i=0;;i++) {
		struct scm_dev_properties p1, p2, r;

		if (ts[i].p1 == NULL)
			break;

		MSG("Testing append: %s %s => %s", ts[i].p1, ts[i].p2, ts[i].r);
		scm_dev_props_parse(&p1, ts[i].p1);
		scm_dev_props_parse(&p2, ts[i].p2);
		scm_dev_props_parse(&r,  ts[i].r);

		scm_dev_props_append(&p1, &p2);
		test_compare(&p1, &r);
	}
}

// size
u64
scm_dev_props_size_grain_set(
	const struct scm_dev_properties *props, u64 grain, struct scm_prop_set set)
{
	unsigned i;
	u64 ret = 0;

	for (i=0; i<props->ranges_nr; i++) {
		const struct scm_dev_range_prop *rp = props->ranges + i;
		if (scm_prop_set_contains(set, rp->r_prop))
			ret += (rp->r_len / grain);
	}

	return ret;
}

u64
scm_dev_props_real_size_grain_set(
	const struct scm_dev_properties *props, u64 grain, struct scm_prop_set set)
{
	unsigned i;
	u64 ret = 0;

	for (i=0; i<props->ranges_nr; i++) {
		const struct scm_dev_range_prop *rp = props->ranges + i;
		u64 r_len;

		if (scm_prop_set_contains(set, rp->r_prop)) {
			r_len = rp->r_real_len_ != 0 ? rp->r_real_len_ : rp->r_len;
			ret += (r_len / grain);
		}
	}

	return ret;
}

void
scm_dev_props_scale__(const struct scm_dev_properties *p, u64 new_len,
                      u64 new_ranges[static SCM_DEV_RANGES_MAX])
{
	// not enough elements for all ranges
	u64 old_len, rem;
	unsigned i;

	if (p->ranges_nr == 0)
		return;

	BUG_ON(new_len < p->ranges_nr);

	rem = new_len;
	old_len = scm_dev_props_size(p);
	for (i=0; i<p->ranges_nr; i++) {
		const struct scm_dev_range_prop *rp = p->ranges + i;
		new_ranges[i] = (new_len*rp->r_len + old_len - 1) / old_len;
		if (new_ranges[i] > rem) {
			new_ranges[i] = rem;
		}
		rem -= new_ranges[i];
	}

	// we could somehthing to avoid having zero areas, but I'm not sure we
	// want to.
	return;
}

static __attribute__((unused)) void
scm_dev_props_scale__test(void)
{
	unsigned i, j;
	struct {
		const char *p;
		u64        new_len;
		u64        res[SCM_DEV_RANGES_MAX];
	} ts[] = {
		{.p  = "100:200", .new_len = 600,  .res = {200,400}},
		{.p  = "100:200", .new_len = 150,  .res = {50,100}},
		{NULL}
	};

	for (i=0;;i++) {
		struct scm_dev_properties props;
		u64 res[SCM_DEV_RANGES_MAX] = {0};

		if (ts[i].p == NULL)
			break;

		MSG_("Testing props_scale__: %s %"PRIu64" => [", ts[i].p, ts[i].new_len);
		for (j=0; j<SCM_DEV_RANGES_MAX; j++) {
			MSG_(" %"PRIu64, ts[i].res[j]);
		}
		MSG_(" ]\n");

		scm_dev_props_parse(&props, ts[i].p);
		scm_dev_props_scale__(&props, ts[i].new_len, res);

		for (j=0; j<SCM_DEV_RANGES_MAX; j++) {
			if (res[j] != ts[i].res[j])
				MSG("===> FAIL: result j=%u is res=%"PRIu64" and differs from expected result expected=%"PRIu64, j, res[j], ts[i].res[j]);
		}
	}
}

void
scm_dev_props_print(const struct scm_dev_properties *props, const char *prefix)
{
	unsigned i;
	u64 total = 0;
	for (i=0; i<props->ranges_nr; i++) {
		const struct scm_dev_range_prop *rp = props->ranges + i;
		total +=  rp->r_len;
		MSG("%sAREA: %2u  prop: %10s len: %18"PRIu64" real_len: %18"PRIu64" zone_len: %10u",
			prefix, i,
			scm_dev_prop_getname_short(rp->r_prop),
			rp->r_len, rp->r_real_len_, rp->r_smr_seq.zone_len);
	}
	MSG("%sTOTAL:    len: %18"PRIu64, prefix, total);
}

// find the range property for a given offset
struct scm_dev_range_prop *
scm_dev_props_find(struct scm_dev_properties *ps, u64 offset)
{
	unsigned i;
	struct scm_dev_range_prop *ret = NULL;

	u64 len = 0, next_len;
	for (i=0; i<ps->ranges_nr; i++) {
		struct scm_dev_range_prop *rp = ps->ranges + i;
		next_len = len + rp->r_len;
		if (offset < next_len) {
			ret = rp;
			break;
		}
		len = next_len;
	}

	return ret;
}

static __attribute__((unused)) void
scm_dev_props_find_test(void) {

	unsigned i;
	struct {
		const char *p;  // string to build scm_dev_properties
		u64        off; // byte offset to query
		ssize_t    res; // -1 for NULL, range_prop offset otherwise
	} ts[] = {
		{.p  = "100:200", .off = 300,  .res = -1},
		{.p  = "100:200", .off = 50,   .res =  0},
		{.p  = "100:200", .off = 99,   .res =  0},
		{.p  = "100:200", .off = 100,  .res =  1},
		{.p  = "100:200", .off = 299,  .res =  1},
		{NULL}
	};

	for (i=0;;i++) {
		struct scm_dev_properties p;
		struct scm_dev_range_prop *rp;
		size_t res;

		if (ts[i].p == NULL)
			break;

		MSG("Testing prop_find: %s %"PRIu64" => %zd", ts[i].p, ts[i].off, ts[i].res);
		scm_dev_props_parse(&p, ts[i].p);
		rp = scm_dev_props_find(&p, ts[i].off);
		res = ts[i].res;
		if (rp == NULL) {
			if (res != -1)
				MSG("===> FAIL: props_find() returned NULL, but expected res is:%zd", res);
		} else if (rp - p.ranges != res) {
			MSG("===> FAIL: props_find() range:%zd, but expected res is:%zd",rp - p.ranges, res);
		}
	}
}

// vpba -> pba
//
// find the underlying device and offset (pba) of an address in the SCM device
// (vpba)
//
// Implementation assumption: underlying devices have consecutive ranges, and
// their offset starts at 0.
scm_pba_t
scm_dev_linear_map(struct scm_dev_properties *ps, scm_vpba_t vpba)
{
	unsigned i;
	scm_pba_t ret = SCM_PBA(NULL, 0);
	u64 rem;

	rem = vpba.addr;
	for (i=0; i<ps->ranges_nr; i++) {

		struct scm_dev_range_prop *rp = ps->ranges + i;

		assert(rp->r_dm_dev != NULL);
		if (rp->r_dm_dev != ret.dm_dev) {
			ret = SCM_PBA(rp->r_dm_dev, 0);
		} else {
			ret.addr += ps->ranges[i-1].r_len;
		}

		if (rem < rp->r_len){
			ret.addr += rem;
			//MSG("Returning: dev:%p addr:%"PRIu64, ret.dm_dev, ret.addr);
			return ret;
		} else {
			rem -= rp->r_len;
		}
	}

	MSG("failed to find mapping");
	return SCM_PBA(NULL, 0);
}

static __attribute__((unused)) void
scm_dev_linear_map_test(void) {

	unsigned i;
	struct {
		const char *p;
		scm_vpba_t  vpba;
		scm_pba_t   pba;
	} ts[] = {
		{ .p = "100@1", .vpba = SCM_VPBA(50), .pba = SCM_PBA((void *)1, 50)},
		{ .p = "100@1:50@2", .vpba = SCM_VPBA(110), .pba = SCM_PBA((void *)2, 10)},
		{ .p = "100@1:50@2", .vpba = SCM_VPBA(99), .pba = SCM_PBA((void *)1, 99)},
		{ .p = "100@1:50@2", .vpba = SCM_VPBA(100), .pba = SCM_PBA((void *)2, 0)},
		{NULL}
	};

	for (i=0;;i++) {
		struct scm_dev_properties props;
		scm_pba_t r;
		scm_pba_t expected;

		if (ts[i].p == NULL)
			break;

		expected = ts[i].pba;

		MSG("Testing linear mapping: %s %"PRIu64"  => %p %"PRIu64,
			ts[i].p, ts[i].vpba.addr, expected.dm_dev, expected.addr);
		scm_dev_props_parse(&props, ts[i].p);

		r = scm_dev_linear_map(&props, ts[i].vpba);
		if (r.dm_dev != expected.dm_dev) {
			MSG("===> FAIL: dm_dev (%p) differs from expected (%p)",
				r.dm_dev, expected.dm_dev);
		} else if (r.addr != expected.addr) {
			MSG("===> FAIL: addr (%"PRId64") differs from expected (%"PRId64")",
				r.addr, expected.addr);
		}
	}
}

/*
 * scm dev properties parsing
 */

static void
scm_dev_props_parse_attribute(struct scm_dev_range_prop *rp, const char *attribute)
{
	const char *s;
	char *endp;

	if (strcmp(attribute, "zone_len=") == 0) {
		u64 zone_len;

		s = attribute + strlen("zone_len=");
		zone_len = os_strtoul(s, &endp, 0);

		if (scm_dev_prop_is_smr_seq(rp->r_prop)) {
			rp->r_smr_seq.zone_len = zone_len;
		}
	}
}

// parse attributes
static void
scm_dev_props_parse_attrs(struct scm_dev_range_prop *rp, char *attrs)
{
	char *start = attrs;

	//printf("%s: %s\n", __FUNCTION__, attrs);
	while (*start != '\0') {
		char *s, *next_start;
		for (s = start; ; s++) {
			if (*s == '\0') {
				next_start = s;
				break;
			}
			if (*s == ',') {
				*s = '\0';
				next_start = s + 1;
				break;
			}
		}
		scm_dev_props_parse_attribute(rp, start);
		start = next_start;
	}

}

// string should be in the form of range1:range2 '\0'
// where range is: len[.prop][/real_size]
//
// See parse_test() for examples
//
// TODO: return an error code
void
scm_dev_props_parse(struct scm_dev_properties *props, const char *str)
{

	unsigned r_i;
	char *endp;
	const char *s;
	const char *attrs_start=NULL;
	size_t attrs_len = 0;

	s = str;
	if (scm_dev_props_valid(props)) {
		//MSG("WARNING: props were expected to be invalid at this point. Resetting");
		scm_dev_props_reset(props);
	}

	for (r_i=0;;) {
		enum scm_property r_prop;
		u64 r_len = os_strtoul(s, &endp, 0);
		u64 r_real_len = 0;
		void *r_dev = NULL;
		s = endp;

		r_prop = SCM_PROP_DEFAULT;

		for (;;) {
			if (*s == '.') { // property specifier
				s64 v = os_strtoul(s + 1, &endp, 0);
				s = endp;
				if (v < SCM_PROP_FIRST || v >= SCM_PROP_LAST) {
					MSG("Uknown property %"PRId64". Using default", v);
				} else {
					r_prop = (enum scm_property)v;
				}
			} else if (*s == '@') { // device specifier
				// This is only used for testing:
				//  we parse the device as a number
				r_dev = (void *)(uintptr_t)os_strtoul(s + 1, &endp, 0);
				s = endp;
			} else if (*s == '/') { // real_len specifier
				r_real_len = os_strtoul(s + 1, &endp, 0);
				s = endp;
			} else if (*s == '{') { // attributes
				attrs_start = s + 1;
				attrs_len   = 0;
				while (true) {
					s++;
					if (*s == '\0') { // error, expected '}'
						attrs_len = 0;
						break;
					} else if (*s == '}') { // done
						s++;
						break;
					}
					attrs_len++;
				}
			} else break;
		}

		props->ranges[r_i] = SCM_DEV_RPROP__(r_len, r_prop, r_real_len, r_dev);
		if (attrs_len > 0) {
			char *attrs = os_malloc(attrs_len + 1);
			if (!attrs) {
				MSG("memory allocation failed");
				break;
			}
			memcpy(attrs, attrs_start, attrs_len);
			attrs[attrs_len] = '\0';
			scm_dev_props_parse_attrs(&props->ranges[r_i], attrs);
			os_free(attrs);
		}

		r_i++;
		if (r_i == SCM_DEV_RANGES_MAX)
			break;

		if (*s != ':')
			break;
		s++;
	}

	props->ranges_nr = r_i;
	return;
}

typedef enum {
	SCM_DEV_PROP_CMP_EQ = 0,
	SCM_DEV_PROP_CMP_NEQ_RANGES = 1, // different ->ranges_nr
	SCM_DEV_PROP_CMP_NEQ_I_PROP = 2, // different ->r_prop     for range i
	SCM_DEV_PROP_CMP_NEQ_I_LEN  = 3, // different ->r_len      for range i
	SCM_DEV_PROP_CMP_NEQ_I_RLEN = 4, // different ->r_real_len for range i
	SCM_DEV_PROP_CMP_NEQ_I_DEV  = 5, // different ->r_dm_dev   for range i
} scm_dev_props_cmp_t;

static scm_dev_props_cmp_t
scm_dev_props_cmp__(struct scm_dev_properties *p1,
                    struct scm_dev_properties *p2,
                    unsigned *ri_ptr)
{
	unsigned ri;
	struct scm_dev_range_prop *r1, *r2;
	int ret;

	if ( p1->ranges_nr != p2->ranges_nr)
		return SCM_DEV_PROP_CMP_NEQ_RANGES;

	ret = SCM_DEV_PROP_CMP_EQ;
	r1 = p1->ranges;
	r2 = p2->ranges;
	for (ri=0 ; ri < p1->ranges_nr; ri++, r1++, r2++) {

		if (r1->r_prop != r2->r_prop) {
			ret = SCM_DEV_PROP_CMP_NEQ_I_PROP;
			break;
		} else if (r1->r_len != r2->r_len) {
			ret = SCM_DEV_PROP_CMP_NEQ_I_LEN;
			break;
		} else if (r1->r_real_len_ != r2->r_real_len_) {
			ret = SCM_DEV_PROP_CMP_NEQ_I_RLEN;
			break;
		} else if (r1->r_dm_dev != r2->r_dm_dev) {
			ret = SCM_DEV_PROP_CMP_NEQ_I_DEV;
			break;
		}
	}

	if (ri_ptr != NULL && ret >= SCM_DEV_PROP_CMP_NEQ_I_PROP)
		*ri_ptr = ri;

	return ret;
}

static scm_dev_props_cmp_t
scm_dev_props_cmp(struct scm_dev_properties *p1, struct scm_dev_properties *p2)
{
	return scm_dev_props_cmp__(p1, p2, NULL);
}

bool
scm_dev_props_eq(struct scm_dev_properties *p1, struct scm_dev_properties *p2)
{
	return scm_dev_props_cmp(p1, p2) == SCM_DEV_PROP_CMP_EQ;
}

// Print a failure message if the two properties differ
// @r: result
// @e: expected
static void
test_compare(struct scm_dev_properties *r, struct scm_dev_properties *e)
{

	struct scm_dev_range_prop *rr __attribute__((unused)), *re __attribute__((unused));
	unsigned ri = 0;
	int ret;

	ret = scm_dev_props_cmp__(r, e, &ri);
	rr = r->ranges + ri;
	re = e->ranges + ri;

	switch (ret) {
		case SCM_DEV_PROP_CMP_NEQ_RANGES:
		MSG("===> FAIL: parsed ranged_nr (%u) differs from expected (%u)",
			r->ranges_nr, e->ranges_nr);
		break;

		case SCM_DEV_PROP_CMP_NEQ_I_PROP:
		MSG("===> FAIL: property for range %u (%u) differs from expected (%u)",
			ri, rr->r_prop, re->r_prop);
		break;

		case SCM_DEV_PROP_CMP_NEQ_I_LEN:
		MSG("===> FAIL: len for range %u (%"PRIu64") differs from expected (%"PRIu64")",
			ri, rr->r_len, re->r_len);
		break;

		case SCM_DEV_PROP_CMP_NEQ_I_RLEN:
		MSG("===> FAIL: real len for range %u (%"PRIu64") differs from expected (%"PRIu64")",
			ri, rr->r_real_len_, re->r_real_len_);
		break;

		case SCM_DEV_PROP_CMP_NEQ_I_DEV:
		MSG("===> FAIL: dev for range %u (%p) differs from expected (%p)",
			ri, rr->r_dm_dev, re->r_dm_dev);
		break;
	}
}

#if defined(SCM_DEV_PROPS_TEST)
static void
parse_test(void)
{
	unsigned i;
	struct {
		const char                *input;
		struct scm_dev_properties expected;
	} tests[] = {{
		.input = "1000",
		.expected = {
			.ranges_nr = 1,
			.ranges    = {
				SCM_DEV_RPROP(1000, SCM_PROP_DEFAULT)
	}}}, {
		.input = "100@55:300",
		.expected = {
			.ranges_nr = 2,
			.ranges    = {
				SCM_DEV_RPROP_(100, SCM_PROP_DEFAULT, (void *)55),
				SCM_DEV_RPROP(300, SCM_PROP_DEFAULT),
	}}}, {
		.input = "100:200",
		.expected = {
			.ranges_nr = 2,
			.ranges    = {
				SCM_DEV_RPROP(100, SCM_PROP_DEFAULT),
				SCM_DEV_RPROP(200, SCM_PROP_DEFAULT),
	}}}, {
		.input = "100:200.1",
		.expected = {
			.ranges_nr = 2,
			.ranges    = {
				SCM_DEV_RPROP(100, SCM_PROP_DEFAULT),
				SCM_DEV_RPROP(200, 1),
	}}}, {
		.input = "100:200.1:100.2",
		.expected = {
			.ranges_nr = 3,
			.ranges    = {
				SCM_DEV_RPROP(100, SCM_PROP_DEFAULT),
				SCM_DEV_RPROP(200, 1),
				SCM_DEV_RPROP(100, 2),
	}}}, {
		.input = "100/1000:200.1/1000:100/2000.2",
		.expected = {
			.ranges_nr = 3,
			.ranges    = {
				SCM_DEV_RPROP__(100, SCM_PROP_DEFAULT, 1000, NULL),
				SCM_DEV_RPROP__(200, 1, 1000, NULL),
				SCM_DEV_RPROP__(100, 2, 2000, NULL),
	}}}, {
		.input = "100.3{zone_len=536870912}",
		.expected = {
			.ranges_nr = 1,
			.ranges    = {
				SCM_DEV_RPROP_SMR(100,3,536870912),
	}}}, {
		.input = "100.1:100.3{zone_len=536870912}:100.2",
		.expected = {
			.ranges_nr = 3,
			.ranges    = {
				SCM_DEV_RPROP(100, 1),
				SCM_DEV_RPROP_SMR(100,3,536870912),
				SCM_DEV_RPROP(100, 2),
	}}}, {
		NULL
	}};

	for (i=0;;i++) {
		const char *input                   = tests[i].input;
		struct scm_dev_properties *expected = &tests[i].expected;
		struct scm_dev_properties result;
		if (input == NULL)
			break;
		MSG("Testing parse: %s", input);
		scm_dev_props_reset(&result);
		scm_dev_props_parse(&result, input);
		test_compare(&result, expected);
	}
}

int main(int argc, char *argv[])
{
	parse_test();
	scm_dev_prop_copy_test();
	scm_dev_prop_truncate_test();
	scm_dev_prop_append_test();
	scm_dev_linear_map_test();
	scm_dev_props_find_test();
	scm_dev_props_scale__test();
	return 0;
}
#endif
#undef SCM_DEV_PROP
