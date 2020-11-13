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

#ifndef	_KV_WIRE_H_
#define	_KV_WIRE_H_

// Wire format for the K/V service.
//  todo: eventually we will have to handle byte ordering

#include <assert.h>

#include "util/types.h"
#include "uDepot/mbuff.hh"
#include "util/debug.h" // UDEPOT_ERR()

namespace udepot {

const u32 MAGIC = 0xfa73fa11;

// Request header
struct ReqHdr {
	u32    magic;
	enum Op { NOP, PUT, GET, DEL, };
	Op    op;
	uint64_t req_id; // defined by the client, used for matching replies
	union {
		struct { uint32_t key_len; }          get;
		struct { uint32_t key_len, val_len; } put;
		struct { uint32_t key_len; }          del;
	};
	// the rest of the request (body) contains:
	//  NOP: nothing
	//  GET: key
	//  PUT: key_and_val
	//  DEL: key
	//
	// NB: put.val_len introduces 4 bytes in the packet header that are not
	// needed for the other operations. It, however, simplifies the code for
	// writing the buffer directly to the IO device (for some IO backends, the
	// buffer needs to adhere to specific requirements). We can change it if
	// needed.

	size_t key_len(void) const {
		switch (op) {
			case NOP:
			UDEPOT_ERR("Requesting key_len in NOP ReqHdr");
			return 0;

			case PUT:
			return put.key_len;

			case GET:
			return get.key_len;

			case DEL:
			return del.key_len;
		}
		UDEPOT_ERR("Unexpected error");
		abort();
	}

	size_t body_len(void) const {
		switch (op) {
			case NOP:
			return 0;

			case GET:
			case DEL:
			return get.key_len;

			case PUT:
			return put.key_len + put.val_len;
		}

		UDEPOT_ERR("Unexpected error");
		abort();
	}

	static const char *op_name_(Op op) {
		switch (op) {
			case NOP: return "NOP";
			case PUT: return "PUT";
			case GET: return "GET";
			case DEL: return "DEL";
			default:  return "__INVALID__";
		}
	}

	const char *op_name() { return op_name_(op); }

	size_t val_len(void) const {
		switch (op) {
			case NOP:
			case GET:
			case DEL:
			UDEPOT_ERR("Requesting val_len in %d ReqHdr op", op);
			return 0;

			case PUT:
			return put.val_len;
		}
		UDEPOT_ERR("Unexpected error");
		abort();
	}

	static ReqHdr mkGET(uint64_t req_id, uint32_t key_len) {
		ReqHdr ret;
		ret.magic = MAGIC;
		ret.op = Op::GET;
		ret.req_id = req_id;
		ret.get.key_len = key_len;
		return ret;
	}

	static ReqHdr mkPUT(uint64_t req_id, uint32_t key_len, uint32_t val_len) {
		ReqHdr ret;
		ret.magic = MAGIC;
		ret.op = Op::PUT;
		ret.req_id = req_id;
		ret.put.key_len = key_len;
		ret.put.val_len = val_len;
		return ret;
	}

	static ReqHdr mkNOP(uint64_t req_id) {
		ReqHdr ret;
		ret.magic= MAGIC;
		ret.op = Op::NOP;
		ret.req_id = req_id;
		return ret;
	}

	bool verify_magic(void) { return magic == MAGIC; }

} __attribute__((packed));

// There is no particular dependency to this value, I just add it here for
// documentation purposes.
static_assert(sizeof(ReqHdr) == 24, "");

// Responses

// Errors on the wire
//  For errors we use traditional UNIX error numbers.
//
//  Some examples (we might want to move this in a more generic)
//
//  0         -> success
//  EEXIST    -> key already exists
//  ENOSPC    -> not enough space
//  ENODATA   -> key does not exist (there is also ENOKEY but we dont use it)
//  EBADMSG   -> invalid message (e.g,. bad magic number)

// Response header
struct ResHdr {
	uint32_t   magic;
	uint64_t   req_id;
	uint32_t   error; // see Errors comment above

	ResHdr() {}
	ResHdr(int res, uint64_t rid) : magic(MAGIC), req_id(rid), error(res) {}

	bool verify_magic(void) { return magic == MAGIC; }

	// if error == 0, the rest of the packet contains:
	//  NOP: nothing
	//  GET: [uint64_t val_len][...val...]
	//  PUT: nothing
	//  DEL: nothing

} __attribute__((packed));

struct ResGetHdr {
	ResHdr   hdr;
	uint64_t val_len;

	ResGetHdr() {}
	ResGetHdr(int res, uint64_t rid, uint64_t vlen) :
		hdr(res, rid),
		val_len(vlen) {}

} __attribute__((packed));

} // namespace udepot end

#endif /* _KV_WIRE_H_ */
