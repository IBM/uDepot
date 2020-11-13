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

#include <unistd.h>
#include <fcntl.h>
#include <rdma/rdma_cma.h>
#include <assert.h>
#include <err.h>
#include <string.h> // memset
#include <thread>

#include "trt/uapi/trt.hh"

#define TRT_RDMA_SELF
#include "trt_rdma.hh"

// NOTES:
// rdma_destroy_event_channel
// set "modify the underlying fd to be non-blocking"

namespace trt {

thread_local RdmaState RdmaState__;

static int
setnonblocking(int fd, int &flags) {
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int
setnonblocking(int fd) {
    int unused;
    return setnonblocking(fd, unused);
}

RdmaState::RdmaState() {
    recv_aos_free_.set(); // set all bits to 1 (free)
}

int RdmaState::init()
{
    assert(chan_ == nullptr);
    int ret;
	chan_ = rdma_create_event_channel();
	if (!chan_) {
		warn("rdma_create_event_channel failed");
		return -1;
	}

	// set the channel as non-blocking
	ret = setnonblocking(chan_->fd);
	if (ret == -1) {
		warn("setnonblocking failed");
		rdma_destroy_event_channel(chan_);
	    return -1;
	}

    return 0;
}

int RdmaState::stop()
{
    return 0;
}

int RdmaState::create_id(struct rdma_cm_id **id, void *ctx, enum rdma_port_space ps)
{
    int ret;

    if (!chan_) {
        warn("Please call init() before calling create_id()\n");
        return -1;
    }

    ret = rdma_create_id(chan_, id, ctx, ps);
    if (ret == -1)
        return -1;

    // bit of a hack, but set the private channel as non-blocking
    ret = setnonblocking((*id)->channel->fd);
    if (ret == -1) {
		warn("setnonblocking failed");
	    rdma_destroy_id(*id);
	    return -1;
	}

    return 0;
}

struct ibv_cq *
RdmaState::get_or_create_cq(struct rdma_cm_id *remote)
{
    // keep one completion queue per RDMA device context
    struct ibv_context *ctx = remote->verbs;
    auto iter = cqs_.find(ctx);
    if (iter != cqs_.end())
        return iter->second;
    // create a new CQ
    struct ibv_cq *ret;
    ret = ibv_create_cq(ctx, cq_entries_nr(), nullptr, nullptr, 0);
    if (!ret) {
        warn("ibc_create_cq failed");
        return ret;
    }
    cqs_.insert(std::make_pair(ctx, ret));
    return ret;
}

int RdmaState::create_qp(struct rdma_cm_id *remote)
{
    int ret;
    struct ibv_cq *cq = get_or_create_cq(remote);
    if (!cq)
        return -1;

    ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_type = IBV_QPT_RC;
    attr.sq_sig_all = 0;
    attr.send_cq = cq;
    attr.recv_cq = cq;
    attr.cap.max_inline_data = 0;
    attr.cap.max_recv_wr = MAX_RECV_WR;
    attr.cap.max_send_wr = MAX_SEND_WR;
    attr.cap.max_recv_sge = 1;
    attr.cap.max_send_sge = 1;

    ret = rdma_create_qp(remote, remote->pd, &attr);
    if (ret == -1)
        warn("rdma_create_qp failed\n");

    return ret;
}



int RdmaState::destroy_id(struct rdma_cm_id *id)
{
    return rdma_destroy_id(id);
}

int RdmaState::wait_for_connect(struct rdma_cm_id *id, struct rdma_cm_id **client)
{
    Waitset ws(T::self());
    AsyncObj::deallocFn nop_dealloc_fn = [] (AsyncObj *o, void *unused) { return; };
    AsyncObj aobj(nop_dealloc_fn, nullptr);
    Future f {&aobj, &ws, nullptr};

    assert(connect_waiters_.find(id) == connect_waiters_.end());
    connect_waiters_.emplace(std::make_pair(id, ConnectInfo(client, &aobj)));

    trt_dmsg("Waiting on wait_on_connect()\n");
    Future *f__ __attribute__((unused)) = ws.wait_();
    assert(f__ == &f);
    int ret = f.get_val();
    trt_dmsg("Woke up on get_request() ret=%d\n", ret);
    f.drop_ref();

    // poller should have removed the entry and update **client
    assert(connect_waiters_.find(id) == connect_waiters_.end());

    if (ret != 0) {
        errno = ret;
        warn("%s: got an error\n", __PRETTY_FUNCTION__);
        return -1;
    }

    // the poller did the everything for us, so we can just return
    return 0;
}

size_t
RdmaState::alloc_recv_wr_id(void)
{
    size_t idx = recv_aos_free_._Find_first();
    if (idx >= RECV_AOS_SIZE)
        return (size_t)-1;
    recv_aos_free_.reset(idx);
    trt_msg("ao: %p\n",get_recv_ao(idx));
    assert(get_recv_ao(idx)->is_invalid());
    trt_dmsg("ALLOCATING: %zd\n", idx);
    return idx;
}

void
RdmaState::free_recv_wr_id(size_t idx)
{
    trt_dmsg("FREEING: %zd\n", idx);
    assert(idx < RECV_AOS_SIZE);
    AsyncObj *ao = get_recv_ao(idx);
    assert(!ao->is_invalid());
    new (ao) AsyncObj();     // invalidate async object
    trt_msg("ao: %p\n", ao);
    assert(ao->is_invalid());
    //recv_aos_free_.set(idx); // make it free again
}

int
RdmaState::post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wrs,
                     struct ibv_recv_wr **bad_wr,
                     Future *futures,
                     Waitset &waitset)
{
    int ret;

    // Doing the ibv call first simplifies error handling. It is safe to do it
    // before setting the async objects, since the poller will not run until we
    // yield.
    ret = ibv_post_recv(qp, wrs, bad_wr);
    if (ret)
        return ret;

    AsyncObj::deallocFn dealloc_fn = [] (AsyncObj *o, void *self_) {
        RdmaState *self = static_cast<RdmaState *>(self_);
        uintptr_t idx = self->get_recv_idx_from_ao(o);
        assert(idx < RECV_AOS_SIZE);
        self->free_recv_wr_id(idx);
    };

    Future *f = futures;
    for (struct ibv_recv_wr *wr = wrs; wr != nullptr; wr = wr->next) {
        AsyncObj *ao = get_recv_ao(wr->wr_id);
        assert(f->is_invalid());
        assert(ao->is_invalid());
        trt_msg("ao: %p\n", ao);
        new (ao) AsyncObj(dealloc_fn, this);
        new (f) Future(ao, &waitset, nullptr);
        f++;
    }

    return ret;
}



// poll control path
//  This will poll the event channel (via an that we set to non-blocking)
// returns -1 for error, or 0.
int RdmaState::poll_events(void)
{
    if (!chan_) {
        fprintf(stderr, "Please call init() before spawning poller");
        return -1;
    }

    const size_t MAX_EVENTS = 8;

    int ret;
    for (size_t i=0; i < MAX_EVENTS; i++) {
        struct rdma_cm_event *event;
        ret = rdma_get_cm_event(chan_, &event);
        if (ret == -1) {
            ret = (EWOULDBLOCK == errno) ? 0 : -1;
            break;
        }
        ret = handle_cm_event(event);
        rdma_ack_cm_event(event);
        if (ret < 0)
            break;
    }
    return ret;
}

// poll data path
int RdmaState::poll_cq(struct ibv_cq *cq)
{
    auto notify = [] (AsyncObj *ao, int retv) {
        bool x = T::notify_add(ao, retv);
        if (!x) {
            T::notify_submit();
            T::notify_init();
            // XXX: At this point we switch to the scheduler, which means that
            // we might get rescheduled. Need to ensure that nothing will break
            // if this happens.
            x = T::notify_add(ao, retv);
            assert(x);
        }
    };

    const size_t wc_nr = 64;
    struct ibv_wc wcs[wc_nr];
    int ret = ibv_poll_cq(cq, wc_nr, wcs);
    if (ret == 0) {
        return 0;
    } else if (ret < 0) {
        fprintf(stderr, "%s: ibv_poll_cq returned error: ret=%d", __PRETTY_FUNCTION__, ret);
        return -1;
    }


    for (int i=0; i < ret; i++) {
        struct ibv_wc *wc = wcs + i;

        // If we get a disconnect event (RDMA_CM_EVENT_DISCONNECTED), we call
        // rdma_disconnect() which "flushes" the queue. We want to get the
        // proper events here so that we can unblock tasks (passing them proper
        // error values). For example, we might have posted recv buffers and
        // have tasks waiting on their completion. With our current setup, we
        // are notified for posted recv buffers with an IBV_WC_SEND opcode
        // (i.e., instead of an IBV_WC_RECV). Not sure if this is a bug, but
        // Jonas suggested that updating the stack/firmware might fix it.  The
        // way we deal with this (at least for now) is to have diferent ->wr_id
        // for each type of event that tasks can wait, so that in case of an
        // error we can distinguish between the different events.
        //
        // I hate RDMA :/
        if (wc->status == IBV_WC_WR_FLUSH_ERR) {
            RecvInfo *ri;
            if ((ri = get_recv_info(wc->wr_id)) != nullptr) {
                AsyncObj *ao = get_recv_ao(wc->wr_id);
                ri->status = IBV_WC_WR_FLUSH_ERR;
                ri->byte_len = 0;
                notify(ao, wc->wr_id);
            } else {
                // TODO: handle other event types once we support them
                assert(false);
            }
        }

        switch (wc->opcode) {
            case IBV_WC_RECV: {
                trt_msg("%s: Receive: wr_id:%" PRIu64 "\n", __PRETTY_FUNCTION__, wc->wr_id);
                assert(wc->wr_id < RECV_AOS_SIZE);
                AsyncObj *ao = get_recv_ao(wc->wr_id);
                RecvInfo *ri = get_recv_info(wc->wr_id);
                // fill in receive info
                ri->status   = wc->status;
                ri->byte_len = wc->byte_len;
                // notify waiters
                notify(ao,wc->wr_id);
            } break;

            case IBV_WC_SEND: {
                printf("%s: IBV_WC_SEND: wr_id:%" PRIu64 " status:%s\n", __PRETTY_FUNCTION__, wc->wr_id, ibv_wc_status_str(wc->status));
            } break;

            case IBV_WC_RDMA_WRITE:
            case IBV_WC_RDMA_READ:
            case IBV_WC_COMP_SWAP:
            case IBV_WC_FETCH_ADD:
            case IBV_WC_BIND_MW:
            case IBV_WC_RECV_RDMA_WITH_IMM:
            //case IBV_WC_LOCAL_INV:
            default:
            errx(1, "%s: Unexpected wc.opcode: %d\n", __PRETTY_FUNCTION__, wc->opcode);
        }
    }


    return 0;
}

int
RdmaState::handle_cm_event(struct rdma_cm_event *event)
{
    auto notify = [] (AsyncObj *ao, int ret) {
        bool x = T::notify_add(ao, ret);
        if (!x) {
            T::notify_submit();
            T::notify_init();
            // XXX: At this point we switch to the scheduler, which means that
            // we might get rescheduled. Need to ensure that nothing will break
            // if this happens.
            x = T::notify_add(ao, ret);
            assert(x);
        }
    };

    switch (event->event) {

        case RDMA_CM_EVENT_ADDR_RESOLVED:
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_CONNECT_RESPONSE:
        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
        case RDMA_CM_EVENT_ESTABLISHED:
        case RDMA_CM_EVENT_DEVICE_REMOVAL:
        case RDMA_CM_EVENT_MULTICAST_JOIN:
        case RDMA_CM_EVENT_MULTICAST_ERROR:
        case RDMA_CM_EVENT_ADDR_CHANGE:
        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
        trt_msg("%s: Unhandled event: %s\n", __PRETTY_FUNCTION__, rdma_event_str(event->event));
        break;

        case RDMA_CM_EVENT_CONNECT_REQUEST: {
            trt_msg("RDMA_CM_EVENT_CONNECT_REQUEST\n");
            struct rdma_cm_id *cli_id = event->id;
            struct rdma_cm_id *srv_id = event->listen_id;
            auto it = connect_waiters_.find(srv_id);
            assert(it != connect_waiters_.end());
            AsyncObj *ao = it->second.ao;
            struct rdma_cm_id **cli = it->second.cli;
            connect_waiters_.erase(it);
            *cli = cli_id;
            notify(ao, event->status);
        } break;

        case RDMA_CM_EVENT_DISCONNECTED: {
            // we got a disconnect event:
            //  - we spawn a thread, because otherwise rdma_destroy_id() never
            //    returns. Not sure why this is the case, but there is a
            //    pthread_cond_wait() call in its implementation. This might be
            //    caused by setting the event fd to non-blocking.
            //  - Calling rdma_disconnect() will flush the work queue, so that
            //    we can release the buffers when we call the completion queue.
            //    At lest for the hardware we have, we do not need to call
            //    ibv_modify_qp() to move the queue to an error state, but I'm
            //    leaving the code commented-out since this might not be the
            //    case for different hardware according to Jonas.
            struct rdma_cm_id *cli_id = event->id;
            //struct rdma_cm_id *srv_id = event->listen_id;
            trt_msg("RDMA_CM_EVENT_DISCONNECTED. spawinng thread to destroy cli_id:%p\n", cli_id);
            std::thread([] (struct rdma_cm_id *id) {
                int ret;
                #if 0
                struct ibv_qp *qp = id->qp;
                struct ibv_qp_attr attr;
                memset(&attr, 0, sizeof(attr));
                attr.qp_state = IBV_QPS_ERR;
                ret = ibv_modify_qp(qp, &attr, IBV_QP_STATE);
                if (ret)
                    warnx("ibv_modify_qp failed: %s\n", strerror(ret));
                #else
                rdma_disconnect(id);
                #endif
                rdma_destroy_qp(id);
                ret = rdma_destroy_id(id);
                if (ret == -1)
                    warn("Error callign rdma_destroy_id");
                trt_msg("Destroying cli_id:%p on another thread: DONE\n", id);
            }, cli_id).detach();
        } break;

    }

    return 0;
}

void *RdmaState::poller_task(void *arg) {
    trt_dmsg("Starting %s\n", __PRETTY_FUNCTION__);
    uint64_t cnt;
    for (cnt = 0; ; cnt++) {

        // poll data path (completion queue)
        T::notify_init();
        for (auto cq: cqs_)
            poll_cq(cq.second);

        // poll control path (event channel)
        if (cnt % 16 == 0)
            poll_events();

        T::notify_submit();
    }

    return nullptr;
}

} // end namespace trt
