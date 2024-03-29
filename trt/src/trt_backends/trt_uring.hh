/*
 *  Copyright (c) 2020,2022 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *           Nikolas Ioannou (nio@zurich.ibm.com, nicioan@gmail.com)
 */

// vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4:

#ifndef TRT_IOU_HH_
#define TRT_IOU_HH_

#include <inttypes.h>
#include <string.h> // memset()
#include <sys/uio.h> // iovec
#include <functional>
#include <iostream>

#include "trt/local_single_sync.hh"
#include "trt_util/uring.hh"

namespace trt {

UringState *get_tls_IouState__();

class IOU;



struct IouOp {
public:
  IouOp(const IouOp&) = delete;
  IouOp& operator=(const IouOp&) = delete;
  IouOp(IouOp &&op) = delete;

  enum State { INVALID, INITIALIZED, IO_SUBMITTED, IO_ERROR, IO_DONE, };
  enum IouOpCode { INVALID_OP, PREADV, PWRITEV };

  IouOp() : iou_lsaobj_(), state_(State::INVALID), op_code_(IouOpCode::INVALID_OP) {}
  IouOp(IouOpCode op, int fd, struct iovec *iov, int iovcnt, off_t off)
    : iou_lsaobj_(), state_(State::INITIALIZED),
      op_code_(op), fd_(fd), iovec_(iov), iovec_cnt_(iovcnt), offset_(off) {
    switch(op_code_) {
    case IouOpCode::PREADV:
    case IouOpCode::PWRITEV:
      break;
    default:
      fprintf(stderr, "invalid IOU OP Code=(%d)\n", op);
      abort();
    };
  }

  ~IouOp() {
    // I cannot think any legit case where this happens since the poller
    // will hold a reference to this op.
    if (state_ == State::IO_SUBMITTED) {
      fprintf(stderr, "[%s +%d] ***** destructor (%s) called while state being IO_SUBMITTED\n", __FILE__, __LINE__, __FUNCTION__);
      abort();
    }
  };

  bool is_invalid() { return state_ == State::INVALID; }
  bool is_initialized() { return state_ == State::INITIALIZED; }
  bool is_io_submitted() { return state_ == State::IO_SUBMITTED; }
  bool is_io_done() { return state_ == State::IO_DONE; }
  bool is_in_error() { return state_ == State::IO_ERROR; }

  void expect_state(State s) {
#if !defined(NDEBUG)
    if (s != state_) {
      std::cerr << "Expected state:" << state_to_str(s)
		<< " but got:" << state_to_str(state_) << std::endl;
      abort();
    }
#endif
  }
  void expect_invalid() { expect_state(State::INVALID); }
  void expect_io_done() { expect_state(State::IO_DONE); }

  // interface for the poller:
  void set_error(void) { state_ = State::IO_ERROR; }
  void set_done(void)  { state_ = State::IO_DONE; }
  void complete(RetT val);
  // Executed by the initiator of the request
  int submit();
  bool is_ready() { return is_io_done(); }
  RetT wait();
  // in some cases, we might want to perform a "dummy" read, i.e., memcpy data
  // from a buffer istead of actually doing IO. In other words, we run the
  // completion in the request, and not in the context of the poller.
  void fake_read(const void *src, size_t src_nbytes);

  static std::string state_to_str(State &s) {
    switch (s) {
    case State::INVALID: return "INVALID";
    case State::INITIALIZED: return "INITIALIZED";
    case State::IO_SUBMITTED: return "IO_SUBMITTED";
    case State::IO_ERROR: return "IO_ERROR";
    case State::IO_DONE: return "IO_DONE";
    default: return "__INVALID__";
    }
  }

  State state() { return state_; }
private:
  //
  LocalSingleAsyncObj iou_lsaobj_;
  State state_;
  IouOpCode op_code_;
  int fd_;
  struct iovec *iovec_;
  int iovec_cnt_;
  int64_t offset_;
};


class IOU { // using class instead of namespace for easy befriending
   public:
    // Do we want a different context per thread? we probably do.  This should be
    // called for each thread. The idea is that it will be called by the main task
    // on each scheduler.
    static inline bool is_initialized(void) { return get_tls_IouState__()->is_initialized(); }
    static inline bool is_done(void) { return get_tls_IouState__()->is_done(); }
    static inline void init(void) { get_tls_IouState__()->init(); }
    // notify aio to drain queues and not accept any more requests
    static void stop(void) { get_tls_IouState__()->stop(); }

    static ssize_t pread(int fd, void *buff, size_t nbytes, off_t off);
    static ssize_t pwrite(int fd, const void *buff, size_t nbytes, off_t off);

    static ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t off);
    static ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t off);

    static void *poller_task(void *unused);
};

} // end namespace trt;

#endif /* TRT_IOU_HH_ */
