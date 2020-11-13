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

/*
 *  memcached - memory caching daemon
 *
 *       http://www.memcache.org/
 *
 *  Copyright 2003 Danga Interactive, Inc.  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      Anatoly Vorobey <mellon@pobox.com>
 *      Brad Fitzpatrick <brad@danga.com>
 */
#include "uDepot/net/mc-helpers.hh"

#include <cstring>
#include <climits>
#include <unistd.h>
#include <time.h>

#include "util/types.h"
#include "util/debug.h"
#include "uDepot/mbuff-cache.hh"
#include "kv-mbuff.hh"
#include "uDepot/sync.hh"
#include "uDepot/net/mc-timer.hh"

// #define	_UDEPOT_DATA_DEBUG_VERIFY
#ifdef	_UDEPOT_DATA_DEBUG_VERIFY
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace memcache {


msg_buff::~msg_buff()
{
	if (nullptr != p_)
		free(p_);
}

int msg_buff::add_free_size(u32 new_free)
{
	u32 free_space = ab_ - rb_;
	const uintptr_t offset = pos_ - p_;
	u8 *p = p_;
	assert(rb_ <= ab_);
	if (new_free <= free_space)
		return 0;	// fast path

	p = static_cast<u8 *>(realloc(p_, ab_ + new_free));

	if (nullptr == p)
		return ENOMEM;
	p_   = p;
	pos_ = p_ + offset;
	ab_ += new_free;
	return 0;
}

static const char end_get_rsp_msg_[] = "END\r\n";
static const char delimiter_[] ="\r\n";

static const char *const mc_req_strs[] = {
	X_REQ(STRINGIFY)
};

static const char *const mc_rsp_strs[] = {
	X_RSP(STRINGIFY)
};

mc_timer mc_timer_g;

cmd::cmd(msg_buff &buff) : buff_(buff)
{
	memset(&this->req_, 0, sizeof(*this) - sizeof(buff_));
	err_ = -1;
}

__attribute__((warn_unused_result))
inline int cmd::prepare_kvmbuff_for_get(
	KV_MbuffInterface &kv,
 	const token *const key,
	udepot::Mbuff &key_mbuff,
	udepot::Mbuff &val_mbuff)
{
	// this is worst case size since cas and end_get_rsp_msg_ will not be included in non-cas gets
	constexpr u32 fixed_cmd_size = 5 /*VALUE*/ + sizeof(flags_) + sizeof(vlen_) + 8 /* cas */ + 2 /*\r\n*/ + sizeof(end_get_rsp_msg_);
	key_mbuff.reslice(0);
	kv.mbuff_add_buffers(key_mbuff, key->length + fixed_cmd_size);
	if (key_mbuff.get_free_size() < key->length + fixed_cmd_size) {
		UDEPOT_ERR("get for req %s failed to allocate key buffer.", mc_req_strs[req_]);
		return ENOMEM;
	}
	key_mbuff.append_uninitialized(sizeof("VALUE ") - 1);

	// append key
	size_t copied __attribute__((unused)) = key_mbuff.append_from_buffer(key->value, key->length);
	assert(key->length == copied);
	key_mbuff.reslice(key->length, sizeof("VALUE ") - 1);
	assert(key_mbuff.get_valid_size() == key->length);

	val_mbuff.reslice(0);
	return 0;
}

__attribute__((warn_unused_result))
inline int cmd::kv_get_for_mc(
	KV_MbuffInterface &kv,
	udepot::Mbuff &key_mbuff,
	udepot::Mbuff &val_mbuff)
{
	const int rc = kv.get(key_mbuff, val_mbuff);
	switch (rc) {
	case 0:
	{
		// get expiry from val_mbuff
		const size_t copied = val_mbuff.copy_to_buffer(
			val_mbuff.get_valid_size() - sizeof(flags_) - sizeof(expiry_),
			(void *) &expiry_, sizeof(expiry_));
		if (unlikely(sizeof(expiry_) != copied)) {
			UDEPOT_ERR("val mbuff failed to copy expiry data.");
			return ENODATA;
		}
		if (0 != expiry_) {
			const time_t cur_time = mc_timer_g.cur_time();
			if (expiry_ < cur_time) {
				UDEPOT_DBG("entry expired");
				// TODO: delete entry
				return ENODATA;
			}
		}
		// TODO: check cas
	}
	case ENODATA:
		break;
	default:
		UDEPOT_ERR("get for req %s failed w/ %s.",
			mc_req_strs[req_], strerror(rc));
	}
	return rc;
}

__attribute__((warn_unused_result))
inline int cmd::prepare_kvmbuff_for_send(
	KV_MbuffInterface &kv,
	udepot::Mbuff &key_mbuff,
	udepot::Mbuff &val_mbuff)
{
	char mid_msg[32] = { 0 };
	// form
	size_t copied __attribute__((unused)) = key_mbuff.prepend_from_buffer("VALUE ", sizeof("VALUE ") - 1);
	assert(sizeof("VALUE ") - 1 == copied);

	assert(sizeof(flags_) <= val_mbuff.get_valid_size());

	// get flags from val_mbuff
	val_mbuff.copy_to_buffer(val_mbuff.get_valid_size() - sizeof(flags_),
				(void *) &flags_, sizeof(flags_));
	ssize_t n = snprintf(mid_msg, sizeof(mid_msg), " %u %d \r\n", flags_,
			(int) (val_mbuff.get_valid_size() - sizeof(flags_) - sizeof(expiry_)));
	if (n < 7 || sizeof(mid_msg) - 1 < (u64) n) {
		UDEPOT_ERR("invalid flags %s read for req %s failed with %ld.",
			mid_msg, mc_req_strs[req_], n);
		return EINVAL;
	}
	copied = key_mbuff.append_from_buffer(mid_msg, n);
	assert(n == (ssize_t) copied);

	kv.mbuff_add_buffers(val_mbuff, strlen(delimiter_));
	val_mbuff.reslice(val_mbuff.get_valid_size() - sizeof(flags_) - sizeof(expiry_));
	copied = val_mbuff.append_from_buffer(delimiter_, strlen(delimiter_));
	assert(strlen(delimiter_) == copied);
	return 0;
}

__attribute__((warn_unused_result))
inline int cmd::handle_single_kv_get(
	KV_MbuffInterface &kv,
	const token *const key,
	udepot::Mbuff &keymb,
	udepot::Mbuff &valmb,
	size_t &lenk, size_t &lenv,
	size_t &lenkb, size_t &lenvb)
{
	int rc = prepare_kvmbuff_for_get(kv, key, keymb, valmb);
	if (0 != rc)
		return rc;
	rc = kv_get_for_mc(kv, keymb, valmb);
	if (0 != rc)
		return rc;
	rc = prepare_kvmbuff_for_send(kv, keymb, valmb);
	if (0 != rc)
		return rc;
	lenkb = keymb.get_valid_size();
	lenvb = valmb.get_valid_size();
	std::tie(rc, lenk) = keymb.get_valid_nchunks(0, lenkb);
	if (0 != rc)
		return rc;
	std::tie(rc, lenv) = valmb.get_valid_nchunks(0, lenvb);

	return rc;
}

/**
 * From the protocol (https://github.com/memcached/memcached/blob/master/doc/protocol.txt)
 Retrieval command:
 ------------------
 The retrieval commands "get" and "gets" operate like this:
 get <key>*\r\n
 gets <key>*\r\n
 - <key>* means one or more key strings separated by whitespace.
 After this command, the client expects zero or more items, each of
 which is received as a text line followed by a data block. After all
 the items have been transmitted, the server sends the string
 "END\r\n"
 to indicate the end of response.

 Each item sent by the server looks like this:
 VALUE <key> <flags> <bytes> [<cas unique>]\r\n
 <data block>\r\n
 If some of the keys appearing in a retrieval request are not sent back
 by the server in the item list this means that the server does not
 hold items with such keys (because they were never stored, or stored
 but deleted to make space for more items, or expired, or explicitly
 deleted by a client).
 *
 */
void cmd::handle_multiget(
	udepot::ConnectionBase &con,
	KV_MbuffInterface &kv,
	udepot::MbuffCacheBase &mb_cache,
	udepot::Mbuff &val_mbuff_in,
	udepot::Mbuff &key_mbuff_in)
{
	err_ = ENOENT;
	const token *const start = &tokens_[parser::KEY_TOKEN];
	// TODO: if ntokens_ == MAX_TOKENS possibly get one more key
	const token *const end   = &tokens_[ntokens_ - 1];
	u32 keys_found = 0, tot_iov_len = 0, tot_iov_lenb = 0;
	udepot::Mbuff *key_mbuffs[end - start + 1];
	udepot::Mbuff *val_mbuffs[end - start + 1];
	memset(key_mbuffs, 0, sizeof(*key_mbuffs) * (end - start));
	memset(val_mbuffs, 0, sizeof(*val_mbuffs) * (end - start));
	key_mbuffs[0] = &key_mbuff_in;
	val_mbuffs[0] = &val_mbuff_in;
	for (const token *key = start; key < end; ++key) {
		udepot::Mbuff *const keymb = key_mbuffs[keys_found];
		udepot::Mbuff *const valmb = val_mbuffs[keys_found];
		if (nullptr == keymb || nullptr == valmb)
			break;	// will not return any remaining keys
		size_t lenk = 0, lenv = 0, lenkb = 0, lenvb = 0;
		int rc = handle_single_kv_get(kv, key, *keymb, *valmb, lenk, lenv, lenkb, lenvb);
		if (0 != rc)
			continue;
		keys_found++;
		tot_iov_len += lenk + lenv;
		tot_iov_lenb += lenkb + lenvb;
		key_mbuffs[keys_found] = mb_cache.mb_get();
		val_mbuffs[keys_found] = mb_cache.mb_get();
	}
	// last valid key val index is keys_found - 1
	// append end msg to last val mb
	udepot::Mbuff *const lastmb = val_mbuffs[keys_found ? keys_found - 1 : 0];
	size_t copied = lastmb->append_from_buffer(end_get_rsp_msg_, strlen(end_get_rsp_msg_));
	assert(strlen(end_get_rsp_msg_) == copied);
	tot_iov_lenb += copied;
	// populate iov
	struct iovec iov[tot_iov_len], *iovp = &iov[0];
	memset(iov, 0, sizeof(*iov) * tot_iov_len);
	size_t iov_len = tot_iov_len;
	for (u32 i = 0; i < keys_found; ++i) {
		udepot::Mbuff *const keymb = key_mbuffs[i];
		udepot::Mbuff *const valmb = val_mbuffs[i];
		int err;
		size_t iov_lenb = 0;
		std::tie(err, iov_lenb) = keymb->fill_iovec_valid(0, keymb->get_valid_size(), iovp, iov_len);
		if (err <= 0 || (int) iov_len < err || keymb->get_valid_size() != iov_lenb) {
			UDEPOT_ERR("error in fill_iovec err=%d iov_len=%lu.", err, iov_len);
			break;
		}
		iov_len -= err;
		iovp += err;
		std::tie(err, iov_lenb) = valmb->fill_iovec_valid(0, valmb->get_valid_size(), iovp, iov_len);
		if (err <= 0 || (int) iov_len < err || valmb->get_valid_size() != iov_lenb) {
			UDEPOT_ERR("error in fill_iovec err=%d iov_len=%lu.", err, iov_len);
			break;
		}
		iov_len -= err;
		iovp += err;
	}
	if (0 == iov_len) {
		// send all retrieved keys
		ssize_t n = con.sendv_full(iov, tot_iov_len);
		if (unlikely(n < 0)) {
			UDEPOT_ERR("sendmsg for req %s failed with %s %d.",
				mc_req_strs[req_], strerror(errno), errno);
			// send END msg and stop
			iov_len = 1; // make sure we try to send an END msg below
		} else if (unlikely(n != (ssize_t) tot_iov_lenb)) {
			UDEPOT_ERR("sendmsg for req failed to send all bytes.");
			iov_len = 1; // make sure we try to send an END msg below
		}
	}
	// release mbuffs
	for (u32 i = 1; i <= keys_found; ++i) {
		udepot::Mbuff *const keymb = key_mbuffs[i];
		udepot::Mbuff *const valmb = val_mbuffs[i];
		if (nullptr != keymb) mb_cache.mb_put(keymb);
		if (nullptr != valmb) mb_cache.mb_put(valmb);
	}
	// handle error case
	if (0 != iov_len) {
		const u32 len = strlen(end_get_rsp_msg_);
		const ssize_t n = con.send(end_get_rsp_msg_, len, 0);
		if (len != n) {
			UDEPOT_ERR("send resp %s for cmd %s failed with %s %d n=%ld.",
				end_get_rsp_msg_, mc_req_strs[req_], strerror(errno), errno, n);
			rsp_ = RSP_SERVER_ERROR;
			return handle_rsp(con);
		}
	}
	err_ = 0;
}

void cmd::handle_get(
	udepot::ConnectionBase &con,
	KV_MbuffInterface &kv,
	udepot::MbuffCacheBase &mb_cache,
	udepot::Mbuff &val_mbuff,
	udepot::Mbuff &key_mbuff)
{
	if (3 < ntokens_)
		return handle_multiget(con, kv, mb_cache, val_mbuff, key_mbuff);
	do {
		const token *const key = &tokens_[parser::KEY_TOKEN];
		u32 tot_iov_len = 0, tot_iov_lenb = 0;
		size_t lenk = 0, lenv = 0, lenkb = 0, lenvb = 0;
		int rc = handle_single_kv_get(kv, key, key_mbuff, val_mbuff, lenk, lenv, lenkb, lenvb);
		if (0 != rc)
			break;
		tot_iov_len += lenk + lenv;
		tot_iov_lenb += lenkb + lenvb;

		size_t copied = val_mbuff.append_from_buffer(end_get_rsp_msg_, strlen(end_get_rsp_msg_));
		assert(strlen(end_get_rsp_msg_) == copied);
		tot_iov_lenb += copied;

		struct iovec iov[tot_iov_len], *iovp = &iov[0];
		memset(iov, 0, sizeof(*iov) * tot_iov_len);
		size_t iov_len = tot_iov_len;
		size_t iov_lenb = 0;
		int err;
		std::tie(err, iov_lenb) = key_mbuff.fill_iovec_valid(0, key_mbuff.get_valid_size(), iovp, iov_len);
		if (err <= 0 || (int) iov_len < err || key_mbuff.get_valid_size() != iov_lenb) {
			UDEPOT_ERR("error in fill_iovec err=%d iov_len=%lu.", err, iov_len);
			break;
		}
		iov_len -= err;
		iovp += err;
		std::tie(err, iov_lenb) = val_mbuff.fill_iovec_valid(0, val_mbuff.get_valid_size(), iovp, iov_len);
		if (err <= 0 || (int) iov_len < err || val_mbuff.get_valid_size() != iov_lenb) {
			UDEPOT_ERR("error in fill_iovec err=%d iov_len=%lu.", err, iov_len);
			break;
		}
		iov_len -= err;
		iovp += err;
		assert(0 == iov_len);
		// send retrieved key-value pair
		ssize_t n = con.sendv_full(iov, tot_iov_len);
		if (unlikely(n < 0)) {
			UDEPOT_ERR("sendmsg for req %s failed with %s %d.",
				mc_req_strs[req_], strerror(errno), errno);
			break;
		} else if (unlikely(n != (ssize_t) tot_iov_lenb)) {
			UDEPOT_ERR("sendmsg for req failed to send all bytes.");
			break;
		}
		assert(n == (ssize_t) tot_iov_lenb);

		err_ = 0;
		return;
	} while (0);

	// handle case where we didn't find the key, or failed to retrieve it
	// send END msg and stop
	const u32 len = strlen(end_get_rsp_msg_);
	const ssize_t n = con.send(end_get_rsp_msg_, len, 0);
	if (len != n) {
		UDEPOT_ERR("send resp %s for cmd %s failed with %s %d n=%ld.",
			end_get_rsp_msg_, mc_req_strs[req_], strerror(errno), errno, n);
		rsp_ = RSP_SERVER_ERROR;
		return handle_rsp(con);
	}
}

/*
 * From the protocol (https://github.com/memcached/memcached/blob/master/doc/protocol.txt)
 Storage commands
 ----------------
 First, the client sends a command line which looks like this:
 <command name> <key> <flags> <exptime> <bytes> [noreply]\r\n
 cas <key> <flags> <exptime> <bytes> <cas unique> [noreply]\r\n
 - <command name> is "set", "add", "replace", "append" or "prepend"
 "set" means "store this data".
 "add" means "store this data, but only if the server *doesn't* already
 hold data for this key".
 "replace" means "store this data, but only if the server *does*
 already hold data for this key".
 "append" means "add this data to an existing key after existing data".
 "prepend" means "add this data to an existing key before existing data".
 The append and prepend commands do not accept flags or exptime.
 They update existing data portions, and ignore new flag and exptime
 settings.
 "cas" is a check and set operation which means "store this data but
 only if no one else has updated since I last fetched it."
 - <key> is the key under which the client asks to store the data
 - <flags> is an arbitrary 16-bit unsigned integer (written out in
 decimal) that the server stores along with the data and sends back
 when the item is retrieved. Clients may use this as a bit field to
 store data-specific information; this field is opaque to the server.
 Note that in memcached 1.2.1 and higher, flags may be 32-bits, instead
 of 16, but you might want to restrict yourself to 16 bits for
 compatibility with older versions.
 - <exptime> is expiration time. If it's 0, the item never expires
 (although it may be deleted from the cache to make place for other
 items). If it's non-zero (either Unix time or offset in seconds from
 current time), it is guaranteed that clients will not be able to
 retrieve this item after the expiration time arrives (measured by
 server time). If a negative value is given the item is immediately
 expired.
 - <bytes> is the number of bytes in the data block to follow, *not*
 including the delimiting \r\n. <bytes> may be zero (in which case
 it's followed by an empty data block).
 - <cas unique> is a unique 64-bit value of an existing entry.
 Clients should use the value returned from the "gets" command
 when issuing "cas" updates.
 - "noreply" optional parameter instructs the server to not send the
 reply.  NOTE: if the request line is malformed, the server can't
 parse "noreply" option reliably.  In this case it may send the error
 to the client, and not reading it on the client side will break
 things.  Client should construct only valid requests.
 After this line, the client sends the data block:
 <data block>\r\n
 - <data block> is a chunk of arbitrary 8-bit data of length <bytes>
 from the previous line.
 After sending the command line and the data block the client awaits
 the reply, which may be:
 - "STORED\r\n", to indicate success.
 - "NOT_STORED\r\n" to indicate the data was not stored, but not
 because of an error. This normally means that the
 condition for an "add" or a "replace" command wasn't met.
 - "EXISTS\r\n" to indicate that the item you are trying to store with
 a "cas" command has been modified since you last fetched it.
 - "NOT_FOUND\r\n" to indicate that the item you are trying to store
 with a "cas" command did not exist.
 */
void cmd::handle_store(udepot::ConnectionBase &con, KV_MbuffInterface &kv, udepot::Mbuff &mbuff)
{
	size_t msg_size = vlen_ + strlen(delimiter_);
	size_t prefix_size = kv.putKeyvalPrefixSize();
	size_t suffix_size = kv.putKeyvalSuffixSize();
	const token &key = tokens_[parser::KEY_TOKEN];
	size_t store_size = prefix_size + key.length + vlen_ + sizeof(flags_) + sizeof(expiry_) + suffix_size;
	rsp_ = RSP_SERVER_ERROR;
	if (0 == vlen_) {
		UDEPOT_MSG("recv for req %s with 0-sized val.", mc_req_strs[req_]);
	}
	mbuff.reslice(0);
	size_t mbuff_size =  std::max(store_size, msg_size);
	kv.mbuff_add_buffers(mbuff, mbuff_size);
	if (mbuff.get_free_size() < mbuff_size) {
		UDEPOT_ERR("recv for req %s failed to allocate buffer.", mc_req_strs[req_]);
		err_ = ENOMEM;
		return;
	}
	// leave space for prefix
	mbuff.append_uninitialized(prefix_size);
	// append key
	size_t copied __attribute__((unused)) = mbuff.append_from_buffer(key.value, key.length);
	assert(copied == key.length);
	// append remaining bytes from cmd recv
	size_t remaining = buff_.rb_ - (buff_.pos_ - buff_.p_);
	if (0 < remaining)
		mbuff.append_from_buffer(buff_.pos_, remaining);
	// receive value
	int err = con.recv_append_to_mbuff(mbuff, msg_size - remaining, 0);
	if (0 != err) {
		UDEPOT_ERR("recv for req %s failed with %s %d.", mc_req_strs[req_], strerror(err), err);
		err_ = err;
		return;
	}
	if ('\r' != mbuff.read_val<char>(prefix_size + key.length + vlen_) ||
		'\n' != mbuff.read_val<char>(prefix_size + key.length + vlen_ + 1)) {
		UDEPOT_ERR("Invalid data format did not find \r\n at the end.");
		rsp_ = RSP_CLIENT_ERROR_BAD_DATA_CHUNK;
		return;
	}
	mbuff.reslice(key.length + vlen_, prefix_size);
	switch(req_) {
	case REQ_ADD:
	case REQ_REPLACE:
	case REQ_SET:
		// execute PUT
		// append expiration time
		copied = mbuff.append_from_buffer((void *) &expiry_, sizeof(expiry_));
		assert(copied == sizeof(expiry_));
		// append flags
		copied = mbuff.append_from_buffer((void *) &flags_, sizeof(flags_));
		assert(copied == sizeof(flags_));
		// clear space for prefix, so that put() can do prepend()
		mbuff.reslice(store_size); // TODO: figure out if this should be store_size - prefix_size instead
		if (REQ_SET == req_)
			err = kv.put(mbuff, key.length);
		else {
			const KV_MbuffInterface::PutOp op = REQ_ADD == req_ ?
				KV_MbuffInterface::CREATE : KV_MbuffInterface::REPLACE;
			assert(REQ_REPLACE == req_ || REQ_ADD == req_);
			err = kv.put(mbuff, key.length, op);
		}
		if (0 == err) {
			rsp_ = RSP_STORED; // return success msg to user
			UDEPOT_DBG("kv put for req %s success with %s %d.", mc_req_strs[req_], strerror(err), err);
#ifdef	_UDEPOT_DATA_DEBUG_VERIFY
			{
				udepot::Mbuff keymb(kv.mbuff_type_index());
				kv.mbuff_add_buffers(keymb, key.length);
				keymb.append_from_buffer(key.value, key.length);
				mbuff.reslice(0);
				int rc = kv.get(keymb, mbuff);
				assert(0 == rc);
				u32 flags_verify = 0;
				mbuff.copy_to_buffer(mbuff.get_valid_size() - sizeof(flags_verify),
						(void *) &flags_verify, sizeof(flags_verify));
				if (0xBEEF != flags_verify) {
					size_t iov_lenb, len;
					std::tie(rc, len) = mbuff.get_valid_nchunks(0, mbuff.get_valid_size());
					struct iovec iov[len];
					std::tie(rc, iov_lenb) = mbuff.fill_iovec_valid(0, mbuff.get_valid_size(), iov, len);
					assert((size_t) rc == len && iov_lenb == mbuff.get_valid_size());
					int fd = ::open("/tmp/test-file-mbuff-out-dbg.dump", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
					assert(-1 != fd);
					ssize_t n = ::pwritev(fd, iov, len, 0);
					assert(n == (ssize_t) mbuff.get_valid_size());
					::close(fd);
				}
				assert(0xBEEF == flags_ && flags_ == flags_verify);
				kv.mbuff_free_buffers(keymb);
			}
#endif
		} else {
			UDEPOT_ERR("kv put for req %s failed with %s %d.", mc_req_strs[req_], strerror(err), err);
			rsp_ = RSP_NOT_STORED; // return success msg to user
		}
		break;
	case REQ_APPEND:
		// do a get and put the old data with the new data appended
	case REQ_PREPEND:
		// do a get and put the old data with the new data prepened
	{
		udepot::Mbuff keymb(kv.mbuff_type_index());
		udepot::Mbuff oldvalmb(kv.mbuff_type_index());
		err = ENOENT;
		do {
			kv.mbuff_add_buffers(keymb, key.length);
			if (keymb.get_free_size() < key.length) {
				UDEPOT_ERR("get for req %s failed to allocate key buffer.", mc_req_strs[req_]);
				break;
			}
			size_t copied = keymb.append_from_buffer(key.value, key.length);
			if (key.length != copied) {
				UDEPOT_ERR("get for req %s failed to copy key buffer.", mc_req_strs[req_]);
				break;
			}
			int rc = kv_get_for_mc(kv, keymb, oldvalmb);
			if (0 != rc) {
				UDEPOT_ERR("kv get for req %s failed with %s %d.", mc_req_strs[req_], strerror(rc), rc);
				break;
			}
			if (REQ_PREPEND == req_) {
				kv.mbuff_add_buffers(mbuff, oldvalmb.get_valid_size());
				if (mbuff.get_free_size() < oldvalmb.get_valid_size()) {
					UDEPOT_ERR("req %s failed to allocate merged buffer.", mc_req_strs[req_]);
					break;
				}
				// prepend newly received value to old value, aka append old to new
				copied = mbuff.append_from_mbuff(oldvalmb, 0, oldvalmb.get_valid_size());
				if (oldvalmb.get_valid_size() != copied) {
					UDEPOT_ERR("get for req %s failed to copy key buffer.", mc_req_strs[req_]);
					break;
				}
				rc = kv.put(mbuff, key.length);
				if (0 != rc)
					UDEPOT_ERR("kv put for req %s failed with %s %d.", mc_req_strs[req_], strerror(rc), rc);
			} else {
				assert(req_ == REQ_APPEND);
				kv.mbuff_add_buffers(oldvalmb, mbuff.get_valid_size());
				if (oldvalmb.get_free_size() < mbuff.get_valid_size()) {
					UDEPOT_ERR("req %s failed to allocate merged buffer.", mc_req_strs[req_]);
					break;
				}
				mbuff.reslice(vlen_, key.length);
				// append flags and expiry
				copied = mbuff.append_from_mbuff(oldvalmb,
								oldvalmb.get_valid_size() - sizeof(flags_) - sizeof(expiry_),
								sizeof(flags_) + sizeof(expiry_));
				assert(copied == sizeof(flags_) + sizeof(expiry_));
				oldvalmb.reslice(oldvalmb.get_valid_size() - sizeof(flags_) - sizeof(expiry_), 0);
				assert(key.length + prefix_size + mbuff.get_valid_size() <= oldvalmb.get_free_size());

				// append new value
				copied = oldvalmb.append_from_mbuff(mbuff, 0, mbuff.get_valid_size());
				assert(copied == mbuff.get_valid_size());
				// prepend key
				copied = oldvalmb.prepend_from_buffer(key.value, key.length);
				assert(copied == key.length);
				// assert(prefix_size <= oldvalmb.get_free_size());
				rc = kv.put(oldvalmb, key.length);
				if (0 != rc)
					UDEPOT_ERR("kv put for req %s failed with %s %d.", mc_req_strs[req_], strerror(rc), rc);
			}
			if (0 == rc) err = 0;
		} while (0);
		kv.mbuff_free_buffers(keymb);
		kv.mbuff_free_buffers(oldvalmb);
		if (0 == err)
			rsp_ = RSP_STORED; // return success msg to user
		break;
	}
	default:
		UDEPOT_ERR("received invalid command %s.", mc_req_strs[req_]);
		break;
	}
	err_ = 0;
}


/*
Increment/Decrement
-------------------

Commands "incr" and "decr" are used to change data for some item
in-place, incrementing or decrementing it. The data for the item is
treated as decimal representation of a 64-bit unsigned integer.  If
the current data value does not conform to such a representation, the
incr/decr commands return an error (memcached <= 1.2.6 treated the
bogus value as if it were 0, leading to confusion). Also, the item
must already exist for incr/decr to work; these commands won't pretend
that a non-existent key exists with value 0; instead, they will fail.

The client sends the command line:

incr <key> <value> [noreply]\r\n

or

decr <key> <value> [noreply]\r\n

- <key> is the key of the item the client wishes to change

- <value> is the amount by which the client wants to increase/decrease
the item. It is a decimal representation of a 64-bit unsigned integer.

- "noreply" optional parameter instructs the server to not send the
  reply.  See the note in Storage commands regarding malformed
  requests.

The response will be one of:

- "NOT_FOUND\r\n" to indicate the item with this value was not found

- <value>\r\n , where <value> is the new value of the item's data,
  after the increment/decrement operation was carried out.

Note that underflow in the "decr" command is caught: if a client tries
to decrease the value below 0, the new value will be 0.  Overflow in
the "incr" command will wrap around the 64 bit mark.

Note also that decrementing a number such that it loses length isn't
guaranteed to decrement its returned length.  The number MAY be
space-padded at the end, but this is purely an implementation
optimization, so you also shouldn't rely on that.
 */
void cmd::handle_arithmetic_cmd(udepot::ConnectionBase &con, KV_MbuffInterface &kv, udepot::Mbuff &mbuff)
{
	const token &key = tokens_[parser::KEY_TOKEN];
	size_t prefix_size = kv.putKeyvalPrefixSize();
	size_t suffix_size = kv.putKeyvalSuffixSize();
	size_t store_size = prefix_size + key.length + 20 /*MAX_VAL_CHAR*/ +
		sizeof(flags_) + sizeof(expiry_) + suffix_size;
	rsp_ = RSP_SERVER_ERROR;

	mbuff.reslice(0);
	kv.mbuff_add_buffers(mbuff, store_size);
	if (mbuff.get_free_size() < store_size) {
		UDEPOT_ERR("recv for req %s failed to allocate buffer.", mc_req_strs[req_]);
		err_ = ENOMEM;
		return;
	}
	// append key
	size_t copied __attribute__((unused)) = mbuff.append_from_buffer(key.value, key.length);
	assert(copied == key.length);
	mbuff.reslice(key.length, prefix_size);

	udepot::Mbuff oldvalmb(kv.mbuff_type_index());
	do {
		int rc = kv_get_for_mc(kv, mbuff, oldvalmb);
		if (0 != rc) {
			UDEPOT_ERR("kv get for req %s failed with %s %d.", mc_req_strs[req_], strerror(rc), rc);
			rsp_ = RSP_NOT_FOUND;
			break;
		}
		u64 oldval;
		char msg[20 + sizeof(delimiter_)] = { 0 }; // 2^64 < 10^20
		assert(sizeof(flags_) + sizeof(expiry_) < oldvalmb.get_valid_size());
		const u64 val_size = oldvalmb.get_valid_size() - sizeof(flags_) - sizeof(expiry_);
		copied = oldvalmb.copy_to_buffer(0, msg, std::min(sizeof(msg) - 1, val_size));
		if (val_size <= 2 || 20 < val_size || !parser::safe_strtoull(msg, &oldval)) {
			UDEPOT_ERR("value non numerical for req %s failed with.", mc_req_strs[req_]);
			rsp_ = RSP_CLIENT_ERROR_NON_NUMERIC;
			break;
		}
		assert(val_size == copied);

		if (REQ_INCR == req_) {
			num_ += oldval;
		} else {
			if (oldval < num_)
				num_ = 0;
			else
				num_ = oldval - num_;
		}
		// now have to put, and return the value
		ssize_t len = snprintf(msg, sizeof(msg), "%lu\r\n", num_);
		if (len <= 2) {	// 2 for \r\n, has to have a t least one digit
			UDEPOT_ERR("failed to format msg snprintf req %s %d %s",
				mc_req_strs[req_], errno, strerror(errno));
			break;
		}
		mbuff.reslice(0);
		mbuff.append_uninitialized(prefix_size);
		size_t copied __attribute__((unused)) = mbuff.append_from_buffer(key.value, key.length);
		assert(copied == key.length);
		copied = mbuff.append_from_buffer(msg, len - 2);
		assert(len - 2 == (s64) copied);
		copied = mbuff.append_from_mbuff(oldvalmb, oldvalmb.get_valid_size() -
						sizeof(flags_) - sizeof(expiry_),
						sizeof(flags_) + sizeof(expiry_));
		assert(sizeof(flags_) + sizeof(expiry_) == copied);
		mbuff.reslice(key.length + len - 2 + sizeof(flags_) + sizeof(expiry_), prefix_size);
		rc = kv.put(mbuff, key.length);
		if (0 != rc) {
			UDEPOT_ERR("kv put for req %s failed with %s %d.", mc_req_strs[req_], strerror(rc), rc);
			break;
		}
		if (unlikely(noreply_))
			break;
		const ssize_t n = con.send(msg, len, 0);
		if (len != n) {
			UDEPOT_ERR("send resp %s for cmd %s failed with %s %d n=%ld.",
				end_get_rsp_msg_, mc_req_strs[req_], strerror(errno), errno, n);
			break;
		}
		kv.mbuff_free_buffers(oldvalmb);
		return;
	} while (0);
	kv.mbuff_free_buffers(oldvalmb);
	if (!noreply_)
		handle_rsp(con);
}

/*
Statistics
----------

The command "stats" is used to query the server about statistics it
maintains and other internal data. It has two forms. Without
arguments:

stats\r\n

it causes the server to output general-purpose statistics and
settings, documented below.  In the other form it has some arguments:

stats <args>\r\n

Depending on <args>, various internal data is sent by the server. The
kinds of arguments and the data sent are not documented in this version
of the protocol, and are subject to change for the convenience of
memcache developers.


General-purpose statistics
--------------------------

Upon receiving the "stats" command without arguments, the server sents
a number of lines which look like this:

STAT <name> <value>\r\n

The server terminates this list with the line

END\r\n

 * only send bytes stat for now
 */
void cmd::handle_stats_cmd(
	udepot::ConnectionBase &con,
	KV_MbuffInterface &kv,
	udepot::Mbuff &mbuff)
{
	const u64 bytes_used = kv.get_kvmbuff_size();
	char msg[sizeof("STAT bytes \r\nEND\r\n") + 20 + 1]; // 2^64 < 10^20
	memset(msg, 0, sizeof(msg));
	ssize_t n = snprintf(msg, sizeof(msg), "STAT bytes %lu\r\nEND\r\n", bytes_used);
	if (n <= 0 || sizeof(msg) - 1 < (u64) n) {
		UDEPOT_ERR("failed to send stats");
		n = strlen("STAT bytes 0\r\nEND\r\n");
		strncpy(msg, "STAT bytes 0\r\nEND\r\n", sizeof(msg));
	}
	const ssize_t send_nr = con.send(msg, n, 0);
	if (send_nr != n) {
		UDEPOT_ERR("sendmsg for req %s failed with %s %d.",
			mc_req_strs[req_], strerror(errno), errno);
	}
}

void cmd::handle_rsp(udepot::ConnectionBase &con)
{
	// response
	const char *rspstr = nullptr;
	switch (rsp_) {
	case RSP_STORED:
		rspstr = "STORED\r\n";
		break;
	case RSP_NOT_STORED:
		rspstr = "NOT_STORED\r\n";
		break;
	case RSP_NOT_FOUND:
		rspstr = "NOT_FOUND\r\n";
		break;

	// errors that will not make the server nuke the connection
	case RSP_ERROR:
		if (!rspstr) rspstr = "ERROR\r\n";
	case RSP_CLIENT_ERROR_FORMAT:
		if (!rspstr) rspstr = "CLIENT_ERROR bad command line format\r\n";
	case RSP_CLIENT_ERROR_NON_NUMERIC:
		if (!rspstr) rspstr = "CLIENT_ERROR cannot increment or decrement non-numeric value\r\n";
	case RSP_CLIENT_ERROR_BOUND:
	case RSP_CLIENT_ERROR_BAD_DATA_CHUNK:
		if (!rspstr) rspstr = "CLIENT_ERROR bad data chunk\r\n";
		err_ = 0;
		UDEPOT_ERR("client error in its cmd %s , resp %s.",
			mc_req_strs[req_], mc_rsp_strs[rsp_]);
		break;

	// errors and unhandled commands are server errors. from the
	// memcache spec: "this is the only case in which the server
	// closes a connection to a client"
	case RSP_EXISTS:
	case RSP_TOUCHED:
	case RSP_DELETED:
	case RSP_VERSION:
	case RSP_CONNECTION_CLOSE:
		// above not implemented, collapse into server error which will close the connection
		if (!rspstr) rspstr = "SERVER_ERROR command not supported \r\n";
	case RSP_KEY_VALUE:	// KEY_VALUE good path return is handled in handle_get, should not come here
		if (!rspstr) rspstr = "SERVER_ERROR\r\n";
	case RSP_LAST:
	case RSP_SERVER_ERROR:
		if (!rspstr) rspstr = "SERVER_ERROR\r\n";
	case RSP_SERVER_ERROR_UNSUPPORTED_MULTIGET:
		if (!rspstr) rspstr = "SERVER_ERROR multiget not yet supported\r\n";
	case RSP_INVAL:		// same as server error: rsp was uninitialized -> bug somewhere
	default:
		if (!rspstr) rspstr = "SERVER_ERROR un-handled cmd\r\n";
		err_ = ECONNRESET;
		UDEPOT_ERR("invalid response %s for cmd %s. resp str=%s.",
			mc_rsp_strs[rsp_], mc_req_strs[req_], rspstr);
		break;
	}

	assert(nullptr != rspstr);
	const u32 len = strlen(rspstr);
	UDEPOT_DBG("sending resp %s for cmd %s.", rspstr, mc_req_strs[req_]);
	const ssize_t n = con.send(rspstr, len, 0);
	if (len != n) {
		UDEPOT_ERR("send resp %s for cmd %s failed with %s %d n=%ld.",
			rspstr, mc_req_strs[req_], strerror(errno), errno, n);
		err_ = ECONNRESET;
	}
}

void cmd::handle(
	udepot::ConnectionBase &con,
	KV_MbuffInterface &kv,
	udepot::MbuffCacheBase &mb_cache,
	udepot::Mbuff &mbuff,
	udepot::Mbuff &keymbuff)
{
	UDEPOT_DBG("handling command %s.", mc_req_strs[req_]);
	switch (req_) {
	case REQ_GET:
	case REQ_GETS:
		return handle_get(con, kv, mb_cache, mbuff, keymbuff);
	case REQ_SET:
	case REQ_ADD:
	case REQ_REPLACE:
	case REQ_APPEND:
	case REQ_PREPEND:
		handle_store(con, kv, mbuff);
		if (unlikely(noreply_))
			return;	// user asked not to send them a reply
		break;
	case REQ_QUIT:
		err_ = ECONNRESET;
		return;
	case REQ_INCR:
	case REQ_DECR:
		return handle_arithmetic_cmd(con, kv, mbuff); // handles reply internally
	case REQ_CAS:
	case REQ_DEL:
	case REQ_STATS:
		return handle_stats_cmd(con, kv, mbuff); // handles reply internally
	case REQ_VERSION:
	case REQ_FLUSH_ALL:
	case REQ_WATCH:
	case REQ_VERBOSITY:
	case REQ_LRU_CRAWLER:
		UDEPOT_ERR("received non-implemented command %s", mc_req_strs[req_]);
		assert(RSP_ERROR <= rsp_);
		break;
	case REQ_INVAL:
		UDEPOT_ERR("received invalid command %s.", mc_req_strs[req_]);
		break;
	case REQ_LAST:
	default:
		UDEPOT_ERR("received invalid command %s.", mc_req_strs[req_]);
		rsp_ = RSP_SERVER_ERROR;
		break;
	}

	handle_rsp(con);
}

int parser::start(void)
{
	return mc_timer_g.start();
}

void parser::stop(void)
{
	mc_timer_g.stop();
}

cmd parser::read_cmd(udepot::ConnectionBase &con, msg_buff &buff)
{
	int rc = 0;
	cmd cmd(buff);
	cmd.err_ = 0;
	do {
		rc = buff.add_free_size(read_cmd_chunk);
		if (unlikely(0 != rc)) {
			UDEPOT_ERR("failed to allocate buff.");
			cmd.err_ = ENOMEM;
			break;
		}
		ssize_t n = con.recv(buff.p_ + buff.rb_, read_cmd_chunk, 0);
		if (0 == n) {
			cmd.err_ = ECONNRESET;
			break;
		}
		if (n < 0) {
			if (EINTR == errno) {
				UDEPOT_MSG("read returned error %s %d", strerror(errno), errno);
				continue;
			} else if (EAGAIN == errno || EWOULDBLOCK == errno) {
				UDEPOT_DBG("read returned error %s %d", strerror(errno), errno);
				rc = EAGAIN;
				continue;
			} else if (ECONNRESET == errno) {
				UDEPOT_DBG("read returned error %s %d", strerror(errno), errno);
				cmd.err_ = errno;
				break;
			} else {
				UDEPOT_ERR("read returned error %s %d", strerror(errno), errno);
				cmd.err_ = errno;
				break;
			}
		}
		buff.rb_ += static_cast<u32>(n);
		assert(buff.rb_ <= buff.ab_);

		rc = try_parse_command(cmd);
		if (0 != rc) {
			if (EAGAIN == rc)
				continue;
			UDEPOT_MSG("try parse command ret %s %d", strerror(rc), rc);
			break;
		}
		// buf.pos_ points to the first byte after the \r\n of the cmd header
		parse(cmd);

	} while (EAGAIN == rc);

	return cmd;
}

// Part of the code below is from the memcache github master branch,
// heavily modified to suit this use

int parser::try_parse_command(cmd &cmd)
{
	msg_buff &buff = cmd.buff_;
	assert(buff.pos_ <= (buff.p_ + buff.ab_));
        u8 *el, *cont;

        el = static_cast<u8 *>(memchr(buff.pos_, '\n', buff.rb_));
        if (!el) {
		if (buff.rb_ > 1024) {
			/*
			 * We didn't have a '\n' in the first k. This _has_ to be a
			 * large multiget, if not we should just nuke the connection.
			 */
			const char *ptr = (const char *) buff.pos_;
			while (*ptr == ' ') { /* ignore leading whitespaces */
				++ptr;
			}

			if (ptr - (const char *) buff.pos_ > 100 ||
				(strncmp(ptr, "get ", 4) && strncmp(ptr, "gets ", 5))) {
				cmd.req_ = REQ_INVAL;
				return EINVAL;
			}
		}

		return EAGAIN;
        }
        cont = el + 1;
        if ((el - buff.pos_) > 1 && *(el - 1) == '\r') {
		el--;
        }
        *el = '\0';

	assert(cont <= (buff.pos_ + buff.rb_));

	buff.pos_ = cont;

	return 0;
}

void parser::parse(cmd &cmd)
{
	const u32 ntokens = cmd.ntokens_ = tokenize_command((char *) cmd.buff_.p_,
							cmd.tokens_, cmd::MAX_TOKENS);
	token *const tokens = cmd.tokens_;
	if (ntokens >= 3 &&
		((strcmp(tokens[COMMAND_TOKEN].value, "get") == 0 && (cmd.req_ = REQ_GET)) ||
			(strcmp(tokens[COMMAND_TOKEN].value, "gets") == 0 && (cmd.req_ = REQ_GETS)) ||
			(strcmp(tokens[COMMAND_TOKEN].value, "bget") == 0 && (cmd.req_ = REQ_GET)))) {

	} else if ((ntokens == 6 || ntokens == 7) &&
		((strcmp(tokens[COMMAND_TOKEN].value, "add") == 0 && (cmd.req_ = REQ_ADD)) ||
			(strcmp(tokens[COMMAND_TOKEN].value, "set") == 0 && (cmd.req_ = REQ_SET)) ||
			(strcmp(tokens[COMMAND_TOKEN].value, "replace") == 0 && (cmd.req_ = REQ_REPLACE)) ||
			(strcmp(tokens[COMMAND_TOKEN].value, "prepend") == 0 && (cmd.req_ = REQ_PREPEND)) ||
			(strcmp(tokens[COMMAND_TOKEN].value, "append") == 0 && (cmd.req_ = REQ_APPEND)) )) {
		parse_storage_arguments(cmd);
	} else if ((ntokens == 7 || ntokens == 8) && (strcmp(tokens[COMMAND_TOKEN].value, "cas") == 0 && (cmd.req_ = REQ_CAS))) {
	} else if ((ntokens == 4 || ntokens == 5) &&
		((strcmp(tokens[COMMAND_TOKEN].value, "incr") == 0 && (cmd.req_ = REQ_INCR)) ||
			(strcmp(tokens[COMMAND_TOKEN].value, "decr") == 0 && (cmd.req_ = REQ_DECR)) )) {
		parse_arithmetic_arguments(cmd);
	} else if (ntokens >= 3 && ntokens <= 5 && (strcmp(tokens[COMMAND_TOKEN].value, "delete") == 0)) {
		cmd.req_ = REQ_DEL;
	} else if ((ntokens == 4 || ntokens == 5) && (strcmp(tokens[COMMAND_TOKEN].value, "touch") == 0)) {
		cmd.req_ = REQ_TOUCH;
	} else if (ntokens >= 2 && (strcmp(tokens[COMMAND_TOKEN].value, "stats") == 0)) {
		cmd.req_ = REQ_STATS;
	} else if (ntokens >= 2 && ntokens <= 4 && (strcmp(tokens[COMMAND_TOKEN].value, "flush_all") == 0)) {
		cmd.req_ = REQ_FLUSH_ALL;
	} else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "version") == 0)) {
		cmd.req_ = REQ_VERSION;
	} else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "quit") == 0)) {
		cmd.req_ = REQ_QUIT;
	} else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "shutdown") == 0)) {
		cmd.req_ = REQ_QUIT;
	} else if (ntokens > 1 && strcmp(tokens[COMMAND_TOKEN].value, "slabs") == 0) {
		cmd.req_ = REQ_SLABS;
	} else if (ntokens > 1 && strcmp(tokens[COMMAND_TOKEN].value, "lru_crawler") == 0) {
		cmd.req_ = REQ_LRU_CRAWLER;
	} else if (ntokens > 1 && strcmp(tokens[COMMAND_TOKEN].value, "watch") == 0) {
		cmd.req_ = REQ_WATCH;
	} else if ((ntokens == 3 || ntokens == 4) && (strcmp(tokens[COMMAND_TOKEN].value, "cache_memlimit") == 0)) {
		cmd.req_ = REQ_CACHE_MEMLIMIT;
	} else if ((ntokens == 3 || ntokens == 4) && (strcmp(tokens[COMMAND_TOKEN].value, "verbosity") == 0)) {
		cmd.req_ = REQ_VERBOSITY;
	} else {
		cmd.req_ = REQ_INVAL;
		cmd.rsp_ = RSP_ERROR;
	}
}

void parser::parse_arithmetic_arguments(cmd &cmd)
{
	cmd.rsp_ = RSP_CLIENT_ERROR_FORMAT;
	do {
		{
			// check noreply
			token &tok = cmd.tokens_[cmd.ntokens_ - 2];
			if (0 == strncmp(tok.value, "noreply", tok.length))
				cmd.noreply_ = true;
		}

		if (max_key_length < cmd.tokens_[KEY_TOKEN].length) {
			cmd.req_ = REQ_INVAL;
			UDEPOT_ERR("invalid key len=%lu max=%u", cmd.tokens_[KEY_TOKEN].length, max_key_length);
			break;
		}
		u64 delta = 0;
		if (!safe_strtoull(cmd.tokens_[2].value, &delta)) {
			cmd.req_ = REQ_INVAL;
			UDEPOT_ERR("invalid numeric delta arg.");
			break;
		}

		cmd.num_ = delta;

		cmd.rsp_ = RSP_INVAL; // will be set by the command handler
	} while (0);
}

void parser::parse_storage_arguments(cmd &cmd)
{
	cmd.rsp_ = RSP_CLIENT_ERROR_FORMAT;
	do {
		{
			// check noreply
			token &tok = cmd.tokens_[cmd.ntokens_ - 2];
			if (0 == strncmp(tok.value, "noreply", tok.length)) {
				cmd.noreply_ = true;
			}
		}
		s64 vlen = 0;
		if (max_key_length < cmd.tokens_[KEY_TOKEN].length) {
			cmd.req_ = REQ_INVAL;
			UDEPOT_ERR("invalid key len=%lu max=%u", cmd.tokens_[KEY_TOKEN].length, max_key_length);
			break;
		}
		if (! (safe_strtoul(cmd.tokens_[2].value, (uint32_t *)&cmd.flags_)
				&& safe_strtol(cmd.tokens_[3].value, &cmd.expiry_)
				&& safe_strtol(cmd.tokens_[4].value, &vlen))) {
			cmd.req_ = REQ_INVAL;
			UDEPOT_ERR("safe_strtoul failed for flags expiry or vlen.");
			break;
		}

		if (0 != cmd.expiry_) {
			time_t cur_time = mc_timer_g.cur_time();
			cmd.sanitize_exp_time(cur_time);
			#if	0
			if (0 != cmd.verify_exp_time(cur_time)) {
				UDEPOT_ERR("Expire time in the past, invalid.");
				cmd.req_ = REQ_INVAL;
				break;
			}
			#endif
		}
		cmd.vlen_ = vlen;
		if (cmd.vlen_ < 0 || cmd.vlen_ > (INT_MAX - 2)) {
			UDEPOT_ERR("Invalid vlen.");
			cmd.req_ = REQ_INVAL;
			break;
		}

		cmd.rsp_ = RSP_INVAL; // will be set by the command handler
	} while (0);
}

size_t parser::tokenize_command(char *command, token *tokens, const size_t max_tokens)
{
	char *s, *e;
	size_t ntokens = 0;
	size_t len = strlen(command);
	unsigned int i = 0;

	assert(command != NULL && tokens != NULL && max_tokens > 1);

	s = e = command;
	for (i = 0; i < len; i++) {
		if (*e == ' ') {
			if (s != e) {
				tokens[ntokens].value = s;
				tokens[ntokens].length = e - s;
				ntokens++;
				*e = '\0';
				if (ntokens == max_tokens - 1) {
					e++;
					s = e; /* so we don't add an extra token */
					break;
				}
			}
			s = e + 1;
		}
		e++;
	}

	if (s != e) {
		tokens[ntokens].value = s;
		tokens[ntokens].length = e - s;
		ntokens++;
	}

	/*
	 * If we scanned the whole string, the terminal value pointer is null,
	 * otherwise it is the first unprocessed character.
	 */
	tokens[ntokens].value =  *e == '\0' ? NULL : e;
	tokens[ntokens].length = 0;
	ntokens++;

	return ntokens;
}

inline void cmd::sanitize_exp_time(const time_t cur_time)
{
	if (expiry_ < 0)
		expiry_ = cur_time ? cur_time - 1 : 1; // make sure we expire immediately
	else if (0 != expiry_ && expiry_ <= parser::REALTIME_MAXDELTA)
		expiry_ += cur_time; // treat it as delta
	// treat is a absolute unix time
}

inline int cmd::verify_exp_time(const time_t cur_time) const
{
	if (0 != expiry_ && expiry_ <= cur_time)
		return EINVAL;
	return 0;
}

inline bool parser::safe_strtoull(const char *const str, uint64_t *const out)
{
    assert(out != NULL);
    errno = 0;
    *out = 0;
    char *endptr;
    unsigned long long ull = strtoull(str, &endptr, 10);
    if ((errno == ERANGE) || (str == endptr)) {
        return false;
    }

    if (isspace(*endptr) || (*endptr == '\0' && endptr != str)) {
        if ((long long) ull < 0) {
            /* only check for negative signs in the uncommon case when
             * the unsigned number is so big that it's negative as a
             * signed number. */
            if (strchr(str, '-') != NULL) {
                return false;
            }
        }
        *out = ull;
        return true;
    }
    return false;
}

inline bool parser::safe_strtoul(const char *const str, uint32_t *const out)
{
	char *endptr = NULL;
	unsigned long l = 0;
	assert(out);
	assert(str);
	*out = 0;
	errno = 0;

	l = strtoul(str, &endptr, 10);
	if ((errno == ERANGE) || (str == endptr)) {
		return false;
	}

	if (isspace(*endptr) || (*endptr == '\0' && endptr != str)) {
		if ((long) l < 0) {
			/* only check for negative signs in the uncommon case when
			 * the unsigned number is so big that it's negative as a
			 * signed number. */
			if (strchr(str, '-') != NULL) {
				return false;
			}
		}
		*out = l;
		return true;
	}

	return false;
}

inline bool parser::safe_strtol(const char *const str, int64_t *const out)
{
	assert(out != NULL);
	errno = 0;
	*out = 0;
	char *endptr;
	long l = strtol(str, &endptr, 10);
	if ((errno == ERANGE) || (str == endptr)) {
		return false;
	}

	if (isspace(*endptr) || (*endptr == '\0' && endptr != str)) {
		*out = l;
		return true;
	}
	return false;
}

}				// end namespace memcache
