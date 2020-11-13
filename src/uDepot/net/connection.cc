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

#include "uDepot/net/connection.hh"
#include "uDepot/mbuff.hh"

// XXX: for _full() functions we could also have used MSG_WAITALL flag

namespace udepot {

// Deal with partially-succesfull recv() calls
//
//   returns error (-1 or 0) and how many data were recv()ed
//
//   If an EOF is received before the whole data is read, this is considered an
//   error. -1 is returned and errno is set to ECONNRESET
std::tuple<int, size_t>
ConnectionBase::recv_full(void *buff_, size_t buff_size, int flags) {
	int ret = 0; // will be returned if buff_size == 0
	size_t data_read = 0;
	char *buff = static_cast<char *>(buff_);
	while (data_read != buff_size) {
		ret = recv(buff + data_read, buff_size - data_read, flags);
		if (ret < 0)
			break;
		if (ret == 0) {
			ret = -1;
			errno = ECONNRESET;
			break;
		}
		data_read += ret;
		if (data_read == buff_size)
			break;
	}
	return std::make_tuple(ret < 0 ? -1 : 0, data_read);
}

// Deal with partially-succesful send() calls
//   returns error (-1 or 0) and how many data were sent
std::tuple<int, size_t>
ConnectionBase::send_full(const void *buff_, size_t buff_size, int flags) {
	int ret = 0; // will be returned if buff_size == 0
	size_t data_sent = 0;
	const char *buff = static_cast<const char *>(buff_);
	while (data_sent != buff_size) {
		assert(data_sent < buff_size);
		ret = send(buff + data_sent, buff_size - data_sent, flags);
		if (ret < 0)
			break;

		assert(ret > 0); // I dont think send() is supposed to return 0
		data_sent += ret;
		if (data_sent == buff_size)
			break;
	}

	return std::make_tuple(ret < 0 ? -1 : 0, data_sent);
}
// Append recv() data to an mbuff
//
// returns:
//  error:  the last call of recv() retuned -1 before filling the buffer
//           (errno is returned)
//   0:      the last call of recv() return 0 before filling the buffer
// size: read size
//
// (see: Mbuff::append_recv())
int
ConnectionBase::recv_append_to_mbuff(Mbuff &mb, size_t size, int flags) {
	if (mb.append_avail_size() < size)
		return ENOSPC; // ENOMEM?

	int recv_err;
	size_t remaining = size;
	mb.append(
		[&remaining, &recv_err, this, flags]
		(unsigned char *buff, size_t len) -> size_t {
			size_t recv_len = std::min(remaining, len);
			if (recv_len == 0)
				return 0;
			std::tie(recv_err, recv_len) = this->recv_full(buff, recv_len, flags);
			assert(recv_len == std::min(remaining, len) || recv_err);
			remaining -= recv_len;
			return recv_len;
		}
	);

	if (recv_err)
		return errno;
	assert(remaining == 0);
	return 0;
}


// returns 0 or error
//
// An alternative way to implement this so that we execute a single syscall
// is via sendmsg()
//
// (see Mbuff::send)
int
ConnectionBase::send_from_mbuff(Mbuff &mb, size_t off, size_t len, int flags) {
	Mbuff::Iterator iter;
	int err;

	err = mb.iterate_valid_init(iter, off, len);
	if (err)
		return EINVAL;

	while (true) {
		Mbuff::Chunk const& c = iter.get_chunk();
		if (c.is_invalid())
			break;
		size_t data_sent;
		std::tie(err, data_sent) = send_full(static_cast<const void *>(c.getConstPtr()), c.len, flags);
		if (err) {
			iter.stop();
			return errno;
		}
		len -= data_sent;
		iter.next();
	}
	if (0 != len)
		return EINVAL;
	return 0;
}


ssize_t ConnectionBase::sendv_full(struct iovec *iov, size_t iov_len) {
	ssize_t send_bytes = 0;
	while (iov_len) {
		struct msghdr msghdr;
		memset(&msghdr, 0, sizeof(msghdr));
		msghdr.msg_iov = iov;
		msghdr.msg_iovlen = iov_len;
		ssize_t ret = sendmsg(&msghdr, 0);
		if (ret < 0)
			return ret;
		send_bytes += ret;
		while (ret) {
			size_t l = std::min((ssize_t) iov->iov_len, ret);
			if (l == iov->iov_len) {
				iov++;
				iov_len--;
			} else {
				iov->iov_base =  (char *) iov->iov_base + l;
				iov->iov_len -= l;
			}
			ret -= l;
		}
	}
	return send_bytes;
}

} // end namespace udepot
