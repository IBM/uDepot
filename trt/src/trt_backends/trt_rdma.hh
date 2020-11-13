/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4:

// TRT RDMA backend

#ifndef TRT_RDMA_HH_
#define TRT_RDMA_HH_

#include <unordered_set>
#include <bitset>

#include <rdma/rdma_cma.h>

#include "trt/async_obj.hh"

namespace trt {

// per-thread RDMA state
class RdmaState {
    static const size_t MAX_SEND_WR = 32;
    static const size_t MAX_RECV_WR = 32;
    static const size_t MAX_QPS     = 32;

    static constexpr size_t cq_entries_nr(void) {
         return (MAX_SEND_WR + MAX_RECV_WR)*MAX_QPS;
    }

public:
    RdmaState();

    // returns 0 if successfull, -1 if not
    int init(void);
    int stop(void);

    void *poller_task(void *unused);

    int create_id(struct rdma_cm_id **id, void *ctx, enum rdma_port_space ps);
    int destroy_id(struct rdma_cm_id *id);

    // similar to rdma_get_request()
    int wait_for_connect(struct rdma_cm_id *id, struct rdma_cm_id **client);

    int create_qp(struct rdma_cm_id *remote);
    void free_recv_wr_id(size_t idx);
    int post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wrs,
                  struct ibv_recv_wr **bad_wr,
                  Future *futures, Waitset &waitset);

    size_t alloc_recv_wr_id(void);

    struct RecvInfo {
        static const size_t MAX_SGES = 4;
        // filled in by user
        struct ibv_recv_wr wr;
        struct ibv_sge     sges[MAX_SGES];
        // filled in by poller
        enum ibv_wc_status status;
        size_t             byte_len;
    };

private:
    struct ConnectInfo {
        struct rdma_cm_id **cli;
        AsyncObj *ao;

        ConnectInfo(struct rdma_cm_id **cli_, AsyncObj *ao_) : cli(cli_), ao(ao_) {}
    };


    // event channel for all operations of this context
    struct rdma_event_channel *chan_;
    // information about tasks waiting on new connections
    std::unordered_map<struct rdma_cm_id *, ConnectInfo> connect_waiters_;
    // one completion queue per RDMA context
    std::unordered_map<struct ibv_context *, struct ibv_cq *> cqs_;

    static const size_t RECV_AOS_SIZE = MAX_RECV_WR*MAX_QPS;
    AsyncObj                   recv_aos_[RECV_AOS_SIZE];
    RecvInfo                   recv_info_[RECV_AOS_SIZE];
    std::bitset<RECV_AOS_SIZE> recv_aos_free_;

    int poll_events(void);
    int poll_cq(struct ibv_cq *);

    int handle_cm_event(struct rdma_cm_event *event);
    struct ibv_cq *get_or_create_cq(struct rdma_cm_id *remote);

public:
    inline RecvInfo *get_recv_info(size_t idx) {
        if (idx >= RECV_AOS_SIZE)
            return nullptr;
        return recv_info_ + idx;
    }

    inline AsyncObj *get_recv_ao(size_t idx) {
        if (idx >= RECV_AOS_SIZE)
            return nullptr;
        return recv_aos_ + idx;
    }

    inline size_t get_recv_idx_from_ao(AsyncObj *o) {
        size_t ret = o - recv_aos_;
        assert(ret < RECV_AOS_SIZE);
        return ret;
    }
};

// user API


#if !defined(TRT_RDMA_SELF)
extern thread_local RdmaState RdmaState__;

struct Rdma {
    using RecvInfo = RdmaState::RecvInfo;

    // initialize scheduler-local state
    static inline int init(void) { return RdmaState__.init(); }
    // stop scheduler-local state
    static inline void stop(void) { RdmaState__.stop(); }

    static inline int create_id(struct rdma_cm_id **id, void *ctx, enum rdma_port_space ps) {
        return RdmaState__.create_id(id, ctx, ps);
    }

    static inline int destroy_id(struct rdma_cm_id *id) {
        return RdmaState__.destroy_id(id);
    }

    static inline int wait_for_connect(struct rdma_cm_id *id, struct rdma_cm_id **client) {
        return RdmaState__.wait_for_connect(id, client);
    }

    static inline int create_qp(struct rdma_cm_id *remote) {
        return RdmaState__.create_qp(remote);
    }

    static inline int post_recv(struct ibv_qp *qp,
                                struct ibv_recv_wr *wrs,
                                struct ibv_recv_wr **bad_wr,
                                Future *futures, Waitset &waitset) {
        return RdmaState__.post_recv(qp, wrs, bad_wr, futures, waitset);
    }

    static inline size_t alloc_recv_wr_id(void) {
        return RdmaState__.alloc_recv_wr_id();
    }

    static inline void free_recv_wr_id(size_t idx) {
        return RdmaState__.free_recv_wr_id(idx);
    }

    static inline RecvInfo *get_recv_info(size_t idx) {
        return RdmaState__.get_recv_info(idx);
    }

    // poller task for checking queues
    static void *poller_task(void *arg) { return RdmaState__.poller_task(arg); };
};

#endif // TRT_RDMA_SELF

} // end namespace trt

#endif // TRT_RDMA_HH_
