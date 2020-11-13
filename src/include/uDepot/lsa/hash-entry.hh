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

#ifndef	_UDEPOT_SALSA_HASH_ENTRY_H_
#define	_UDEPOT_SALSA_HASH_ENTRY_H_

#include "util/types.h"
#include "util/debug.h"

namespace udepot {
#define	_UDEPOT_HOP_SCOTCH_BUCKET_SIZE	(32UL)

#define	UDEPOT_SALSA_KEYTAG_BITS	(8UL)
#define	UDEPOT_SALSA_KEYTAG_BITMASK	((1UL << UDEPOT_SALSA_KEYTAG_BITS) - 1)
#define UDEPOT_SALSA_KV_SIZE_BITS	(11UL)
#define UDEPOT_SALSA_HOP_OFF_BITS	NUM_BITS(_UDEPOT_HOP_SCOTCH_BUCKET_SIZE)
#define UDEPOT_SALSA_PBA_BITS		(64UL - UDEPOT_SALSA_KEYTAG_BITS - UDEPOT_SALSA_KV_SIZE_BITS - UDEPOT_SALSA_HOP_OFF_BITS)
#define UDEPOT_SALSA_UNUSED_PBA_ENTRY	((1UL << UDEPOT_SALSA_PBA_BITS) - 1)
#define UDEPOT_SALSA_DELETED_KV_SIZE_ENTRY	(0UL)
#define UDEPOT_SALSA_MAX_KV_SIZE	((1UL << UDEPOT_SALSA_KV_SIZE_BITS) - 1)
struct HashEntry {
	u64 bucket_offset:UDEPOT_SALSA_HOP_OFF_BITS;
	u64 key_tag:UDEPOT_SALSA_KEYTAG_BITS;
	u64 kv_size:UDEPOT_SALSA_KV_SIZE_BITS;
	u64 pba:UDEPOT_SALSA_PBA_BITS;
	HashEntry(u32 bucket_offset, u64 key_tag) : bucket_offset(bucket_offset), key_tag(key_tag),
						    kv_size(1), pba(UDEPOT_SALSA_UNUSED_PBA_ENTRY) {}
	HashEntry() : bucket_offset(0), key_tag(0), kv_size(1), pba(UDEPOT_SALSA_UNUSED_PBA_ENTRY) {}
	void purge(void) {bucket_offset = 0; key_tag = 0; reset_deleted(); reset_used();};
	bool deleted(void) const { return kv_size == UDEPOT_SALSA_DELETED_KV_SIZE_ENTRY; }
	void set_deleted(void) { kv_size = UDEPOT_SALSA_DELETED_KV_SIZE_ENTRY; }
	void reset_deleted(void) { kv_size = UDEPOT_SALSA_MAX_KV_SIZE; } // non UDEPOT_SALSA_DELETED_KV_SIZE_ENTRY
	bool used(void) const { return pba != UDEPOT_SALSA_UNUSED_PBA_ENTRY; }
	void set_used(void) const { }
	void reset_used(void) { pba = UDEPOT_SALSA_UNUSED_PBA_ENTRY; }
}__attribute__((packed));
static_assert(8 == sizeof(HashEntry), "HashEntry has to be cache aligned");
};				// namespace udepot ends

#endif	// _UDEPOT_SALSA_HASH_ENTRY_H_
