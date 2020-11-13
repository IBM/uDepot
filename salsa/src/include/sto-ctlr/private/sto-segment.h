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
#ifndef	_SALSA_STO_SEGMENT_H_
#define	_SALSA_STO_SEGMENT_H_

#include "data-structures/list.h"
#include "data-structures/hlist-hash.h"
#include "generic/compiler.h"
#include "libos/os-atomic.h"
#include "libos/os-lock.h"

#include "sto-ctlr/scm-dev-properties.h" // scm_property

enum segment_state {

        SEG_INVALID      =0,    /* make sure 0 is not a valid s */
        SEG_FREE         =1,
        SEG_FREE_REL     =2,
        SEG_LSA_ALLOCATOR=3,
        SEG_GC           =4,
        SEG_GC_XNP       =5,
        SEG_STATE_LAST   =6
};


#define	SALSA_SEGMENT_STREAM_BITS	5
#define SALSA_SEGMENT_CTLR_BITS		8
#define SALSA_SEGMENT_PRIV_BITS		8
struct segment {
	os_spinlock_t sl;
	union {
		struct cds_list_head list;
		struct {
			struct hash_list hhead;
			os_atomic32_t nr1;
			os_atomic32_t nr2;
		};
	};
	union {
		struct hash_list hlist;
		struct {
			os_atomic32_t nr3;
			os_atomic32_t nr4;
		};
	};
	os_atomic32_t    valid_nr; /* number of valid pages */
	os_atomic32_t    outs_read_nr;
	volatile u32    *rmap;
	volatile u8      ctlr_id;
	volatile u8      stream;
	volatile u8      priv; /* gc and cache */

	/**
	 * we use 1 bit to identify if this is a relocation segment, and the
	 * rest to identify the free queue id it was placed, so that we can
	 * return it there when it is freed again. This obviously restricts us
	 * to 128 queues, which should be enough for now (tm).
	 *
	 * Is volatile necessary here? volatile bitfields seem tricky to
	 * get right for the compiler. --KOU
	 */
	#define SALSA_SEGMENT_QUEUEID_BITS 7
	volatile u8       is_reloc:1;
	volatile u8       queue_id:SALSA_SEGMENT_QUEUEID_BITS;
	volatile u8       state;
	enum scm_property prop;
}__attribute__((aligned));

/* COMPILE_TIME_ASSERT_INFO(32 == sizeof(struct segment), seg); */
/* COMPILE_TIME_ASSERT_INFO(0 == (CACHE_LINE_SIZE % sizeof(struct segment)), seg); */
/* COMPILE_TIME_ASSERT_INFO(0 == (sizeof(struct segment) % (CACHE_LINE_SIZE / 2)), seg); */

#define	SALSA_MAX_PRIVATE_ID	((1U << SALSA_SEGMENT_PRIV_BITS) - 1U)
#define	SALSA_INVALID_PRIVATE_ID	SALSA_MAX_PRIVATE_ID
#define	SALSA_INVALID_CTLR_ID	((1U << SALSA_SEGMENT_CTLR_BITS) - 1U)

static inline void segment_reset(
	struct segment *const seg,
	const u32             page_nr)
{
	seg->is_reloc = 0;
	seg->stream   = 0;
	seg->priv  = SALSA_INVALID_PRIVATE_ID;
	seg->ctlr_id  = SALSA_INVALID_CTLR_ID;
	seg->state    = SEG_INVALID;
	os_atomic32_set(&seg->valid_nr, page_nr);
	CDS_INIT_LIST_HEAD(&seg->list);
	hash_list_init(&seg->hlist);
}

static inline void segment_read_init(struct segment *const seg)
{
	/* BUG_ON(0 != os_atomic32_read(&seg->outs_read_nr)); */
	assert(0 == os_atomic32_read(&seg->outs_read_nr));
	assert(0 == ((uintptr_t) (&seg->outs_read_nr) & 3));
	os_atomic32_set(&seg->outs_read_nr, 1);
}

static inline void segment_init(
	struct segment *const seg,
	const u32        page_nr,
	enum scm_property prop,
	u8               queue_id)
{
	segment_reset(seg, page_nr);
	segment_read_init(seg);
	os_spinlock_init(&seg->sl);
	seg->prop     = prop;
	seg->queue_id = queue_id;
	seg->rmap     = NULL;
	assert(0 == ((uintptr_t) (&seg->valid_nr) & 3));
	assert(SEG_INVALID == seg->state);
}

static inline void segment_set_ctlr(
	struct segment *const seg,
	const u8         ctlr_id)
{
	seg->ctlr_id = ctlr_id;
}

static inline void segment_set_reloc(struct segment *const seg)
{
	seg->is_reloc = 1;
}

static inline bool
segment_is_reloc(struct segment *const seg)
{
	return (1 == seg->is_reloc);
}

static inline u32 segment_get(struct segment *const seg)
{
	return os_atomic32_inc_return(&seg->valid_nr);
}

static inline u32 segment_put(struct segment *const seg)
{
	assert(0 < os_atomic32_read(&seg->valid_nr));
	return os_atomic32_dec_return(&seg->valid_nr);
}

static inline u32 segment_read_nr(struct segment *const seg)
{
	return os_atomic32_read(&seg->outs_read_nr);
}

static inline u32 segment_read_get(struct segment *const seg)
{
	return os_atomic32_inc_return(&seg->outs_read_nr);
}

static inline u32 segment_read_put(struct segment *const seg)
{
	assert(0 < os_atomic32_read(&seg->outs_read_nr));
	return os_atomic32_dec_return(&seg->outs_read_nr);
}

static inline u32 segment_getn(
	struct segment *const seg,
	const u32        n)
{
	assert(n);
	return os_atomic32_add_return(&seg->valid_nr, n);
}

static inline u32 segment_putn(
	struct segment *const seg,
	const u32        n)
{
	BUG_ON(os_atomic32_read(&seg->valid_nr) < (s32) n);
	return os_atomic32_sub_return(&seg->valid_nr, n);
}

__attribute__((pure))
static inline u32 segment_valid_nr(struct segment *const seg)
{
	return os_atomic32_read(&seg->valid_nr);
}

__attribute__((pure))
static inline u32 segment_get_stream(
        const struct segment *const seg)
{
        return seg->stream;
}

static inline void segment_set_stream(
        struct segment *const seg,
        const u32        stream)
{
        seg->stream = stream;
}

__attribute__((pure))
static inline u32 segment_get_ctlr_id(
        const struct segment *const seg)
{
        return seg->ctlr_id;
}

__attribute__((pure))
static inline u32 segment_get_private(
        const struct segment *const seg)
{
	return seg->priv;
}

static inline void segment_set_private(
        struct segment *const seg,
        const u32        priv)
{
	assert(priv <= SALSA_MAX_PRIVATE_ID);
	seg->priv = priv;
}

__attribute__((pure))
static inline u32 segment_get_state(
	const struct segment *const seg)
{
	return seg->state;
}

static inline void segment_set_state(
	struct segment    *const seg,
	const enum segment_state state)
{
	assert(state < SEG_STATE_LAST);
	seg->state = state;
}
#endif	/* _SALSA_STO_SEGMENT_H_ */
