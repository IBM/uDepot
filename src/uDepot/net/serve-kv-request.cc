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

#include <algorithm> // std::min()
#include <errno.h>

#include "uDepot/net/serve-kv-request.hh"
#include "uDepot/stimer.hh"

namespace udepot {

// This function tries to implement a fast path where only one recv() is used
// (e.g., for GET operations)
//
// Assumption: each client can only have one request in-flight at a time.
static int
recv_request(ConnectionBase &cli, ReqHdr &req_hdr, Mbuff &mb_body, MbuffAllocIface &alloc_if)
{
	// Construct a msghdr and iovec to perform a single read
	size_t iov_cnt, iov_bytes;
	size_t iov_len = std::min(1 + mb_body.append_avail_nchunks(), (size_t)8);
	struct iovec iov[iov_len];
	size_t prev_valid_size = mb_body.get_valid_size();

	// body
	size_t avail_size = mb_body.append_avail_size();
	std::tie(iov_cnt, iov_bytes) = mb_body.fill_iovec_append(avail_size, iov + 1, iov_len - 1);
	// header
	iov[0].iov_base = (void *)&req_hdr;
	iov[0].iov_len  = sizeof(ReqHdr);
	iov_cnt++;
	iov_bytes += sizeof(ReqHdr);

	struct msghdr msghdr = {
		.msg_name = NULL,      /* optional address */
		.msg_namelen = 0,      /* size of address */
		.msg_iov = iov,        /* scatter/gather array */
		.msg_iovlen = iov_cnt, /* # elements in msg_iov */
	    .msg_control = NULL,   /* ancillary data, see below */
	    .msg_controllen = 0,   /* ancillary data buffer len */
	    .msg_flags = 0,        /* flags on received message */
	};

	ssize_t recv_bytes = cli.recvmsg(&msghdr, 0);
	if (recv_bytes == -1) {
		return errno;
	} else if (recv_bytes == 0) {
		return ECONNRESET;
	} else if (recv_bytes < static_cast<ssize_t>(sizeof(ReqHdr))) {
		// Technically, this might have been a partial read, but this is very
		// unlikely given the size of the header, so just bail out.
		return ECONNRESET;
	}

	mb_body.reslice(prev_valid_size + recv_bytes - sizeof(ReqHdr));

	if (!req_hdr.verify_magic()) {
		UDEPOT_MSG("Unable to verify magic");
		// try to notify the client, and return an error
		ResHdr res_hdr(EBADMSG, 0);
		int err;
		std::tie(err, std::ignore) = cli.send_full(&res_hdr, sizeof(res_hdr), 0);
		return EBADMSG;
	}

	// OK, now we can assume that we have a valid header
	size_t received_body_bytes = recv_bytes - sizeof(ReqHdr);
	UDEPOT_DBG("valid request op=%s (id=%lu)", req_hdr.op_name(), req_hdr.req_id);
	size_t body_len = req_hdr.body_len();

	// This breaks the assumption that the client can have at most a single
	// request in flight.
	if (received_body_bytes > body_len) {
		UDEPOT_MSG("Received more body bytes (%zd) than body length (%zd)", received_body_bytes, body_len);
		return EBADMSG;
	}
	// Do we need to receive more?
	if (received_body_bytes < body_len) {
		alloc_if.mbuff_add_buffers(mb_body, prev_valid_size + body_len);
		int err = cli.recv_append_to_mbuff(mb_body, body_len - received_body_bytes, 0);
		if (err)
			return err;
	}

	assert(mb_body.get_valid_size() == prev_valid_size + body_len);
	return 0;
}

static int
send_get_responose(ConnectionBase &cli, ResGetHdr &reshdr, Mbuff &val) {

	const size_t iov_stack_size = 8;
	int err;
	size_t nchunks;
	struct iovec *iov;
	struct iovec iov_[iov_stack_size];
	size_t val_len = reshdr.val_len;

	if (val_len == 0) {
		std::tie(err, std::ignore) = cli.send_full(&reshdr, sizeof(reshdr), 0);
		if (err)
			UDEPOT_MSG("send_full (reshdr) returned error: %s (%d)", strerror(errno), errno);
		return err ? errno : 0;
	}

	assert(val_len == val.get_valid_size());
	std::tie(err, nchunks) = val.get_valid_nchunks(0, val_len);
	assert(!err); // should not happen

	size_t iov_size = nchunks + 1;
	if (iov_size > iov_stack_size) {
		iov = static_cast<struct iovec *>(malloc(sizeof(struct iovec)*iov_size));
		if (!iov)
			return ENOMEM;
	} else {
		iov = iov_;
	}

	iov[0].iov_base = &reshdr;
	iov[0].iov_len  = sizeof(reshdr);

	int iov_cnt;
	size_t iov_bytes;
	std::tie(iov_cnt, iov_bytes) = val.fill_iovec_valid(0, val_len, iov + 1, iov_size - 1);
	assert((size_t)iov_cnt == iov_size - 1);
	assert(iov_bytes == val_len);

	ssize_t ret = cli.sendv_full(iov, iov_size);

	if (iov != iov_)
		free(iov);
	return ret < 0 ? errno : 0;
}

// Serve a KV request
// Returns:
//   0   -> if request was served normally
// error -> if there was an error.
//
// If the client closed the connection (e.g., recv() returned 0)
// ECONNRESET is returned so that the caller can close the socket, etc.
int
serve_kv_request(ConnectionBase &cli,
                 KV_MbuffInterface &kv, Mbuff &req_body, Mbuff &result) {

	ReqHdr req_hdr;
	int err;

	req_body.reslice(0);
	// for the PUT operation, we can avoid copies if we allow for some space for
	// a prefix. This enables PUT to do a prepend() without copying data.
	//
	// We can do that in the general case for the following reasons:
	//
	// 1. We know that our networks backends do not have alignment restrictions,
	// which means that we can have the network write anywhere we want. If this
	// assumption breaks, however (e.g,. implementing a DPDK backend), we will
	// need to revisit our approach here. One solution would be to align the
	// network headers with the prefix size, but this makes processing a bit
	// more complicated. Another solution might be to have the body recv() only
	// a small amount of data so that there is a fast path for the GET
	// operations with small keys, and for every other case we copy this small
	// amount of data and do a second recv().
	//
	// 2. GET and DEL operations do not perform IO reads/writes with the
	// provided mbuffs, so no alignment restrictions exist there for the mbuffs
	// we pass, so having having some prepending space for PUTs is fine.
	size_t prefix_size = kv.putKeyvalPrefixSize();
	kv.mbuff_add_buffers(req_body, prefix_size); // at least add space for the prefix
	req_body.append_uninitialized(prefix_size);

	err = recv_request(cli, req_hdr, req_body, kv);
	if (err)
		return err;

	// clear prefix space (allows for prepend())
	size_t body_size = req_hdr.body_len();
	assert(req_body.get_valid_size() == prefix_size + body_size);
	req_body.reslice(body_size, prefix_size);

	switch (req_hdr.op) {
		case ReqHdr::NOP: {
			ResHdr res_hdr(0, req_hdr.req_id);
			std::tie(err, std::ignore) = cli.send_full(&res_hdr, sizeof(res_hdr), 0);
			return err ? errno : 0;
		}

		case ReqHdr::PUT: {
			auto &kv_mbuff = req_body;
			//kv_mbuff.print_data(stdout, 0, req_hdr.put.key_len);
			int ret = kv.put(kv_mbuff, req_hdr.put.key_len);
			UDEPOT_DBG("PUT returned: %d", ret);
			// send reply
			ResHdr res_hdr(ret, req_hdr.req_id);
			std::tie(err, std::ignore) = cli.send_full(&res_hdr, sizeof(res_hdr), 0);
			return err ? errno : 0;
		}

		case ReqHdr::GET: {
			auto &key_mbuff = req_body;
			auto &val_mbuff = result;

			//key_mbuff.print_data(stdout, 0, key_mbuff.get_valid_size());
			val_mbuff.reslice(0);
			int op_err = kv.get(key_mbuff, val_mbuff);
			UDEPOT_DBG("GET returned: %d", op_err);

			ResGetHdr reshdr(op_err, req_hdr.req_id, op_err ? 0 : val_mbuff.get_valid_size());
			return send_get_responose(cli, reshdr, val_mbuff);
		}

		case ReqHdr::DEL: {
			UDEPOT_ERR("%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
			return ENOTSUP;
		}

		default:
			UDEPOT_ERR("%s:%d: Uknown operation: %d", __PRETTY_FUNCTION__, __LINE__, req_hdr.op);
			return EINVAL;
	}
}

// Serve a KV request
// Returns:
//   0   -> if request was served normally
// error -> if there was an error.
//
// If the client closed the connection (e.g., recv() returned 0)
// ECONNRESET is returned so that the caller can close the socket, etc.
static int __attribute__((unused))
serve_kv_request_two_recvs(ConnectionBase &cli,
                           KV_MbuffInterface &kv, Mbuff &mb1, Mbuff &mb2) {
	ReqHdr req_hdr;
	ssize_t ret;
	int err;
	size_t bytes;

	UDEPOT_DBG("Processing client request: recv()");
	// STimer tr("SERVE FROM RECV");
	std::tie(err, bytes) = cli.recv_full(&req_hdr, sizeof(req_hdr), 0);
	if (err) {
		// return an error if the socket was closed
		return (bytes == 0) ? ECONNRESET : errno;
	}
	//UDEPOT_MSG("received %zd bytes", bytes);

	if (!req_hdr.verify_magic()) {
		UDEPOT_MSG("Unable to verify magic");
		ResHdr res_hdr(EBADMSG, 0);
		std::tie(err, std::ignore) = cli.send_full(&res_hdr, sizeof(res_hdr), 0);
		if (err) {
			return errno;
		}
		return 0;
	}

	UDEPOT_DBG("valid request op=%s (id=%zd)", req_hdr.op_name(), req_hdr.req_id);
	switch (req_hdr.op) {
		case ReqHdr::NOP: {
			ResHdr res_hdr(0, req_hdr.req_id);
			std::tie(err, std::ignore) = cli.send_full(&res_hdr, sizeof(res_hdr), 0);
			if (err) {
				return errno;
			}
			return 0;
		}

		case ReqHdr::PUT: {
			// prepare buffer, allocate space for prefix+kv+suffix
			size_t body_size = req_hdr.put.key_len + req_hdr.put.val_len;
			size_t prefix_size = kv.putKeyvalPrefixSize();
			size_t suffix_size = kv.putKeyvalSuffixSize();
			size_t size = prefix_size + body_size + suffix_size;
			mb1.reslice(0);
			kv.mbuff_add_buffers(mb1, size);
			// leave space for prefix
			mb1.append_uninitialized(prefix_size);
			// receive KV
			err = cli.recv_append_to_mbuff(mb1, body_size, 0);
			if (err)
				return err;
			assert(mb1.get_valid_size() == prefix_size + body_size);
			// clear space for prefix, so that put() can do prepend()
			mb1.reslice(body_size, prefix_size);
			// execute PUT
			ret = kv.put(mb1, req_hdr.put.key_len);
			UDEPOT_DBG("PUT returned: %zd", ret);
			// send reply
			ResHdr res_hdr(ret, req_hdr.req_id);
			std::tie(err, std::ignore) = cli.send_full(&res_hdr, sizeof(res_hdr), 0);
			if (err) {
				return errno;
			}
			return 0;
		}

		case ReqHdr::GET: {
			auto &key_mbuff = mb2;
			auto &val_mbuff = mb1;

			key_mbuff.reslice(0);
			kv.mbuff_add_buffers(key_mbuff, req_hdr.get.key_len);
			err = cli.recv_append_to_mbuff(key_mbuff, req_hdr.get.key_len, 0);
			if (err) {
				UDEPOT_MSG("key_mbuff.append_recv() returned error: %s (%d)", strerror(err), err);
				return err;
			}
			assert(key_mbuff.get_valid_size() == req_hdr.get.key_len);

			val_mbuff.reslice(0);
			ret = kv.get(key_mbuff, val_mbuff);
			UDEPOT_DBG("GET returned: %zd", ret);
			ResGetHdr reshdr(ret, req_hdr.req_id, err ? 0 : val_mbuff.get_valid_size());
			std::tie(err, std::ignore) = cli.send_full(&reshdr, sizeof(reshdr), 0);
			if (err) {
				UDEPOT_MSG("send_full (reshdr) returned error: %s (%d)", strerror(errno), errno);
				return errno;
			}

			err = cli.send_from_mbuff(val_mbuff, 0, val_mbuff.get_valid_size(), 0);
			if (err) {
				UDEPOT_MSG("val_mbuff.send() returned error: %s (%d)", strerror(err), err);
				return err;
			}
			return 0;
		}

		case ReqHdr::DEL:
		UDEPOT_ERR("%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
		abort();
	}

	return 0;
}


} // end namespace udepot
