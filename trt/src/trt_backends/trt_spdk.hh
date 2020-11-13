/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

/* vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4: */

#ifndef TRT_SPDK_HH__
#define TRT_SPDK_HH__

#include "trt_util/spdk.hh"
#include "trt/uapi/trt.hh" // ControllerBase

#include <vector>
#include <string>

namespace trt {

SpdkState *get_tls_SpdkState__();

// To use trt::SPDK you need:
//  - A global state (SpdkGlobalState): In it's constructor, this class will
//    probe and register the controllers
//
//  - A per-thread state (SpdkState): This is  essentially an SPDK queue pair
//  used to do IO.
class SPDK { // using class instead of namespace for easy befriending
    public:
    static inline void init(SpdkGlobalState &gs) {
        get_tls_SpdkState__()->init(gs);
    }

    static inline std::shared_ptr<SpdkQpair>
    getQpair(std::string a) {
        return get_tls_SpdkState__()->getQpair(a);
    }

    static inline std::shared_ptr<SpdkQpair>
    getQpair_by_prefix(std::string a) {
        return get_tls_SpdkState__()->getQpair_by_prefix(a);
    }

    static std::vector<std::string>
    getNamespaceNames() {
        return get_tls_SpdkState__()->getNamespaceNames();
    }

    static void stop() { get_tls_SpdkState__()->stop(); }

    // return -1 or the number of lbas read/written
    // (We might want to change the interface at some point to return a
    // meaningful error number either via errno or as a negative number)
    static ssize_t read(SpdkQpair *qp,  SpdkPtr &buff, uint64_t lba, uint32_t lba_cnt);
    static ssize_t write(SpdkQpair *qp, SpdkPtr &buff, uint64_t lba, uint32_t lba_cnt);

    static void *poller_task(void *arg);

    static ssize_t pread(SpdkQpair *qp, void *buff, size_t len, off_t off);
    static ssize_t pwrite(SpdkQpair *qp, const void *buff, size_t len, off_t off);
    static ssize_t preadv(SpdkQpair *qp, const struct iovec *iov, size_t iovcnt, off_t off);
    static ssize_t pwritev(SpdkQpair *qp, const struct iovec *iov, int iovcnt, off_t off);

protected:
    static void spdk_io_cb(void *, const struct spdk_nvme_cpl *);
};

class RteController : public ControllerBase {
public:
    void spawn_scheduler(TaskFn main_fn, TaskFnArg main_arg,
                         TaskType main_type,
                         unsigned lcore);
    void wait_for_all();
    virtual ~RteController();
};

} // end namespace trt

#endif /* TRT_SPDK_HH__ */
