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

 #ifndef UDEPOT_NET_CONNECTION_H__
 #define UDEPOT_NET_CONNECTION_H__

#include <tuple>
#include <cstring>
#include <algorithm>
#include <atomic>

// for ConnectionSocket
#include <sys/types.h>
#include <sys/socket.h>


namespace udepot {

class Mbuff;

struct ConnectionBase {
	// send(2) / recv(2) et al. semantics
	virtual ssize_t send(const void *buf, size_t len, int flags) = 0;
	virtual ssize_t recv(void *buf, size_t len, int flags) = 0;
	virtual ssize_t sendmsg(const struct msghdr *msg, int flags) = 0;
	virtual ssize_t recvmsg(struct msghdr *msg, int flags) = 0;
	virtual ~ConnectionBase() {};

	// Deal with partially-succesfull recv() calls
	//
	//   returns error (-1 or 0) and how many data were recv()ed
	//
	//   If an EOF is received before the whole data is read, this is considered an
	//   error. -1 is returned and errno is set to ECONNRESET
	std::tuple<int, size_t>
	recv_full(void *buff_, size_t buff_size, int flags);

	// Deal with partially-succesful send() calls
	//   returns error (-1 or 0) and how many data were sent
	std::tuple<int, size_t>
	send_full(const void *buff_, size_t buff_size, int flags);

	// Append recv() data to an mbuff
	//
	// returns:
	//  error:  the last call of recv() retuned -1 before filling the buffer
	//           (errno is returned)
	//   0:      the last call of recv() return 0 before filling the buffer
	// size: read size
	//
	// (see: Mbuff::append_recv())
	int recv_append_to_mbuff(Mbuff &mb, size_t size, int flags);

	// Send data from an mbuff
	//
	// Performs multiple ->send_full() calls.
	//
	// returns 0 or error
	//
	// (see Mbuff::send)
	int send_from_mbuff(Mbuff &mb, size_t off, size_t len, int flags);

	ssize_t sendv_full(struct iovec *iov, size_t iov_len);

        std::atomic<size_t> recv_bytes_;
        std::atomic<size_t> send_bytes_;
};

// seems too trivial for its own file
class ConnectionSocket : public ConnectionBase {
public:
	ConnectionSocket() : fd_(-1) {}
	ConnectionSocket(int fd): fd_(fd) {}
	ConnectionSocket(ConnectionSocket const&) = delete;
	void operator=(ConnectionSocket const&) = delete;

	virtual ssize_t send(const void *buff, size_t len, int flags) override final {
		return ::send(fd_, buff, len, flags);
	}
	virtual ssize_t recv(void *buff, size_t len, int flags) override final {
		return ::recv(fd_, buff, len, flags);
	}
	virtual ssize_t sendmsg(const struct msghdr *msg, int flags) override final {
		return ::sendmsg(fd_, msg, flags);
	}
	virtual ssize_t recvmsg(struct msghdr *msg, int flags) override final {
		return ::recvmsg(fd_, msg, flags);
	}
private:
	int fd_;
};



} // end udepot namespace

 #endif /* ifndef UDEPOT_NET_CONNECTION_H__ */
