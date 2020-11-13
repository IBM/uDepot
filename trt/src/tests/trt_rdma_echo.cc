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

// Spawn an echo server using trt+rdma

#include <arpa/inet.h>
#include <err.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_rdma.hh"


extern "C" {
    #include "trt_util/net_helpers.h"
}

static int
post_recv_reg_buff(rdma_cm_id *id, uintptr_t buff, size_t len, uint32_t lkey, trt::Future &future, trt::Waitset &ws)
{
    size_t recv_idx = trt::Rdma::alloc_recv_wr_id();
    if (recv_idx == (size_t)-1)
        return -1;

    trt::Rdma::RecvInfo *ri = trt::Rdma::get_recv_info(recv_idx);
    ri->sges[0].addr   = buff;
    ri->sges[0].length = len;
    ri->sges[0].lkey   = lkey;
    ri->wr.wr_id       = recv_idx;
    ri->wr.next        = nullptr;
    ri->wr.sg_list     = &ri->sges[0];
    ri->wr.num_sge     = 1;

    struct ibv_recv_wr *bad_w;
    printf("POSTING ON ID=%zd\n", recv_idx);
    int ret = trt::Rdma::post_recv(id->qp, &ri->wr, &bad_w, &future, ws);
    if (ret) {
        trt::Rdma::free_recv_wr_id(recv_idx);
        return -1;
    }

    return 0;
}

static void *
echo_serve(void *arg)
{
    trt_msg("enter\n");
    struct rdma_cm_id *remote_id = (struct rdma_cm_id *)arg;

    int ret = trt::Rdma::create_qp(remote_id);
    if (ret == -1)
        err(1, "trt::Rdma::create_qp()");

    void *buff;
    ret = posix_memalign(&buff, 4096, 4096);
    if (ret)
        errx(1, "posix_memaligned failed: %s", strerror(ret));
    struct ibv_mr *mr = ibv_reg_mr(remote_id->pd, buff, 4096, IBV_ACCESS_LOCAL_WRITE);
    if (!mr)
        err(1, "ibv_reg_mr");

    trt::Waitset ws(trt::T::self());
    trt::Future future;

    ret = post_recv_reg_buff(remote_id, (uintptr_t)buff, 4096, mr->lkey, future, ws);
    if (ret < 0)
        errx(1, "post_reg_buff failed\n");

    rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.private_data = nullptr;
    conn_param.private_data_len = 0;
    //conn_param.responder_resources = 128; // ??
    //conn_param.initiator_depth = 128; // ??
    trt_msg("Accept\n");
    ret = rdma_accept(remote_id, &conn_param);
    if (ret)
        err(1, "rdma_accept");

    for (;;) {
        trt_msg("waiting on recv()\n");
        trt::Future *f__ __attribute__((unused)) = ws.wait_();
        assert(f__ == &future);
        ret = future.get_val();
        trt_msg("Woke up on receive ret=%d\n", ret);
        trt::Rdma::RecvInfo *ri = trt::Rdma::get_recv_info(ret);
        trt_msg("Status: %s len: %zd\n", ibv_wc_status_str(ri->status), ri->byte_len);
        if (ri->status == IBV_WC_WR_FLUSH_ERR) {
            trt_msg("DISCONNECT EVENT\n");
            future.drop_ref();
            ret = ibv_dereg_mr(mr);
            if (ret)
                warn("ibv_dereg_mr failed");
            free(buff);
            return nullptr;
        } if (ri->status != IBV_WC_SUCCESS) {
            errx(1, "error: %s", ibv_wc_status_str(ri->status));
        }

        // PONG (use the receive buffer)
        struct ibv_sge sg;
        struct ibv_send_wr wr, *bad_wr;
        sg.addr   = ri->sges[0].addr;
        sg.length = ri->byte_len;
        sg.lkey   = 0; // INLINE, lkey will not be checked

        memset(&wr,0, sizeof(wr));
        wr.wr_id      = 0;
        wr.sg_list    = &sg;
        wr.num_sge    = 1;
        wr.opcode     = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
        trt_dmsg("%s: echo: ibv_post_send()\n", __PRETTY_FUNCTION__);
        ret = ibv_post_send(remote_id->qp, &wr, &bad_wr);
        if (ret)
            err(1, "ibv_post_send wr:%p bad_wr:%p", &wr, bad_wr);

        future.drop_ref();

        ret = post_recv_reg_buff(remote_id, (uintptr_t)buff, 4096, mr->lkey, future, ws);
        if (ret < 0)
            errx(1, "post_reg_buff failed\n");
    }
}

static void *
echo_server(void *arg)
{
    int ret;
    const char *url_str = (const char *)arg;
    struct url url;
    sockaddr_in addr;
    struct rdma_cm_id *self_id = NULL;

    ret = url_parse(&url, url_str);
    if (ret) {
        fprintf(stderr, "url_parse failed\n");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    ret = inet_pton(AF_INET, url.node, &addr.sin_addr);
    if (!ret)
        err(1, "inet_pton failed");
    addr.sin_port = htons(atoi(url.serv));
    //addr.sin_addr.s_addr = INADDR_ANY;
    printf("sin_port=%u\n", addr.sin_port);
    printf("s_addr=%u\n", addr.sin_addr.s_addr);

    trt::Rdma::init();
    trt::T::spawn(trt::Rdma::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);

    printf("Rdma::create_id()\n");
    ret = trt::Rdma::create_id(&self_id, nullptr, RDMA_PS_TCP);
    if (ret == -1)
        err(1, "Rdma::create_id failed");

    ret = rdma_bind_addr(self_id, (sockaddr *)&addr);
    if (ret == -1)
        err(1, "rdma_bind_addr failed");

    trt_dmsg("rdma_listen()\n");
    ret = rdma_listen(self_id, 128 /* connection backlog */);
    if (ret == -1)
        err(1, "rdma_listen failed");

    while (true) {
        struct rdma_cm_id *remote_id;
        ret = trt::Rdma::wait_for_connect(self_id, &remote_id);
        if (ret == -1)
            err(1, "get_request");

        trt_msg("Got new connection: %p. Spawning server\n", remote_id);
        trt::T::spawn(echo_serve, (void *)remote_id, nullptr, true, trt::TaskType::TASK);
    }

    url_free_fields(&url);
}

void server(const char url_str[])
{
    trt::Rdma rdma;
    int ret;
    struct rdma_cm_id *self_id = NULL;
    struct url url;
    sockaddr_in addr;

    ret = url_parse(&url, url_str);
    if (ret) {
        fprintf(stderr, "url_parse failed\n");
        exit(1);
    }

    if (rdma.init() == -1)
        err(1, "rdma->init() failed");

    addr.sin_family = AF_INET;
    ret = inet_pton(AF_INET, url.node, &addr.sin_addr);
    if (!ret)
        err(1, "inet_pton failed");
    addr.sin_port = htons(atoi(url.serv));
    printf("sin_port=%u\n", addr.sin_port);
    printf("s_addr=%u\n", addr.sin_addr.s_addr);

    ret = rdma.create_id(&self_id, nullptr, RDMA_PS_TCP);
    if (ret)
        err(1, "rdma_create_id failed");

    ret = rdma_bind_addr(self_id, (sockaddr *)&addr);
    if (ret == -1)
        err(1, "rdma_bind_addr failed");

    printf("rdma_listen()\n");
    ret = rdma_listen(self_id, 128 /* connection backlog */);
    if (ret == -1)
        err(1, "rdma_listen failed");

    while (true) {
        struct rdma_cm_id *remote_id;

        printf("rdma_get_request()\n");
        ret = rdma_get_request(self_id, &remote_id);
        printf("rdma_get_request(): returned %d (ibv_context:%p)\n", ret, remote_id->verbs);
        if (ret == -1)
            err(1, "get_request");

        constexpr size_t max_send_wr = 128;
        constexpr size_t max_recv_wr = 512;
        ibv_cq *cq = ibv_create_cq(remote_id->verbs, max_send_wr + max_recv_wr, nullptr, nullptr, 0);
        if (!cq)
            err(1, "ibv_create_cq");

        ibv_qp_init_attr qp_init_attr;
        memset(&qp_init_attr, 0, sizeof(qp_init_attr));
        qp_init_attr.qp_type = IBV_QPT_RC;
        qp_init_attr.sq_sig_all = 0;
        qp_init_attr.send_cq = cq;
        qp_init_attr.recv_cq = cq;
        qp_init_attr.cap.max_inline_data = 4096;
        qp_init_attr.cap.max_recv_wr = 128;
        qp_init_attr.cap.max_send_wr = 128;
        qp_init_attr.cap.max_recv_sge = 1;
        qp_init_attr.cap.max_send_sge = 1;
        ret = rdma_create_qp(remote_id, remote_id->pd, &qp_init_attr);
        if (ret)
            err(1, "rdma_create_qp");

        size_t nregions = 32;
        size_t region_size = 4096;
        void *regions;
        ret = posix_memalign(&regions, 4096, nregions*region_size);
        if (ret)
            errx(1, "posix_memalign failed:%s", strerror(ret));

        struct ibv_mr *region_mr = ibv_reg_mr(remote_id->pd, regions, region_size*nregions, IBV_ACCESS_LOCAL_WRITE);
        if (!region_mr)
            err(1, "ibv_reg_mr");

        struct ibv_recv_wr recv_wrs[nregions];
        struct ibv_sge     recv_sgs[nregions];
        for (size_t i=0; i<nregions; i++) {
            struct ibv_recv_wr *w  = recv_wrs + i;
            struct ibv_sge     *sg = recv_sgs + i;

            sg->addr = (uintptr_t)regions + (i*region_size);
            sg->length = region_size;
            sg->lkey = region_mr->lkey;

            w->wr_id = i;
            w->next = nullptr;
            w->sg_list = sg;
            w->num_sge = 1;

            struct ibv_recv_wr *bad_w;
            ret = ibv_post_recv(remote_id->qp, w, &bad_w);
            if (ret)
                errx(1, "ibv_post_recv: id=%zd err:%s", i, strerror(ret));
        }

        rdma_conn_param conn_param;
        memset(&conn_param, 0, sizeof(conn_param));
        conn_param.private_data = nullptr;
        conn_param.private_data_len = 0;
        //conn_param.responder_resources = 128; // ??
        //conn_param.initiator_depth = 128; // ??
        ret = rdma_accept(remote_id, &conn_param);
        if (ret)
            err(1, "rdma_accept");

        struct ibv_wc wc;
        bool done = false;
        while (!done) {
            ret = ibv_poll_cq(cq, 1 /* depth */, &wc);
            if (ret == 0) {
                continue; // retry
            } if (ret < 0 || wc.status != IBV_WC_SUCCESS) {
                errx(1, "%s: ibv_poll_cq failed: ret=%d err=%s", __PRETTY_FUNCTION__, ret, ibv_wc_status_str(wc.status));
            }

            switch (wc.opcode) {
                case IBV_WC_RECV: {
                    printf("%s: Receive: wr_id:%" PRIu64 "\n", __PRETTY_FUNCTION__, wc.wr_id);
                    char *xbuff =  (char *)((uintptr_t)regions + region_size*wc.wr_id);
                    printf("%s: %s\n", __PRETTY_FUNCTION__, xbuff);

                    // send back a reply
                    struct ibv_sge sg;
                    struct ibv_send_wr wr, *bad_wr;
                    sg.addr = (uintptr_t)xbuff;
                    sg.length = strlen(xbuff) + 1;
                    sg.lkey = 0; // INLINE, so it will not be checked

                    memset(&wr,0, sizeof(wr));
                    wr.wr_id      = 0;
                    wr.sg_list    = &sg;
                    wr.num_sge    = 1;
                    wr.opcode     = IBV_WR_SEND;
                    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE ;
                    printf("%s: echo: ibv_post_send()\n", __PRETTY_FUNCTION__);
                    ret = ibv_post_send(remote_id->qp, &wr, &bad_wr);
                    if (ret)
                        err(1, "ibv_post_send");
                } break;

                case IBV_WC_SEND: {
                    printf("%s: IBV_WC_SEND: wr_id:%" PRIu64 "\n", __PRETTY_FUNCTION__, wc.wr_id);
                } break;

                case IBV_WC_RDMA_WRITE:
                case IBV_WC_RDMA_READ:
                case IBV_WC_COMP_SWAP:
                case IBV_WC_FETCH_ADD:
                case IBV_WC_BIND_MW:
                case IBV_WC_RECV_RDMA_WITH_IMM:
                //case IBV_WC_LOCAL_INV:
                default:
                errx(1, "%s: Unexpected wc.opcode: %d\n", __PRETTY_FUNCTION__, wc.opcode);
            }
        }
    }
}

void client(const char url_str[])
{
    int ret;
    struct url url;
    //char b[1] = {0};
    const size_t NLOOPS = 1;
    sockaddr_in addr;

    ret = url_parse(&url, url_str);
    if (ret) {
        fprintf(stderr, "url_parse failed\n");
        exit(1);
    }

    addr.sin_family = AF_INET;
    ret = inet_pton(AF_INET, url.node, &addr.sin_addr);
    if (!ret)
        err(1, "inet_pton failed");
    addr.sin_port = htons(atoi(url.serv));
    printf("sin_port=%u\n", addr.sin_port);
    printf("s_addr=%u\n", addr.sin_addr.s_addr);

    for (size_t i=0; i<NLOOPS; i++) {
            printf("client: Client i=%zd\n", i);
            struct rdma_cm_id *client_id;
            ibv_cq *cq = NULL;

            printf("client: Create RDMA id...\n");
            ret = rdma_create_id(nullptr, &client_id, nullptr, RDMA_PS_TCP);
            if (ret)
                err(1, "client: rdma_create_id failed");

            printf("client: Resolve RDMA addr...\n");
            ret = rdma_resolve_addr(client_id, NULL, (struct sockaddr *)(&addr), 1000);
            if (ret)
                err(1, "client: rdma_resolve_addr failed");

            printf("client: Resolve RDMA route...\n");
            ret = rdma_resolve_route(client_id, 1000);
            if (ret)
                err(1, "client: rdma_resolve_route failed");

            printf("client: Create COMP_CHANNEL...\n");
            //ibv_wr_opcode opcode = IBV_WR_RDMA_READ;
            #if 0
            ibv_comp_channel *comp_channel = ibv_create_comp_channel(client_id->verbs);
            if (!comp_channel)
                err(1, "client: ibv_create_comp_channel failed");
            #else
            ibv_comp_channel *comp_channel = nullptr;
            #endif

            printf("client: Create CQ...\n");
            constexpr size_t max_send_wr = 128;
            constexpr size_t max_recv_wr = 512;
            cq = ibv_create_cq(client_id->verbs, max_send_wr + max_recv_wr, NULL, comp_channel, 0);
            if (!cq)
                err(1, "client: ibv_create_cq failed");

            ibv_qp_init_attr qp_init_attr;
            memset(&qp_init_attr,0,sizeof(qp_init_attr));
            qp_init_attr.qp_type = IBV_QPT_RC;
            qp_init_attr.sq_sig_all = 0;
            qp_init_attr.send_cq = cq;
            qp_init_attr.recv_cq = cq;
            qp_init_attr.cap.max_inline_data = 0;
            qp_init_attr.cap.max_recv_wr = 128;
            qp_init_attr.cap.max_send_wr = 128;
            qp_init_attr.cap.max_recv_sge = 1;
            qp_init_attr.cap.max_send_sge = 1;

            printf("client: Create QP...\n");
            ret = rdma_create_qp(client_id, client_id->pd, &qp_init_attr);
            if (ret)
                err(1, "client: rdma_create_qp failed");

            printf("client: IBV query device...\n");
            ibv_device_attr dev_attr;
            ret = ibv_query_device(client_id->verbs, &dev_attr);
            if (ret)
                err(1, "client: ibv_query_device failed");

            rdma_conn_param conn_param;
            conn_param.private_data = nullptr;
            conn_param.private_data_len = 0;
            conn_param.responder_resources = dev_attr.max_qp_rd_atom;
            conn_param.initiator_depth = dev_attr.max_qp_rd_atom;

            printf("client: Conecting...\n");
            ret = rdma_connect(client_id, &conn_param);
            if (ret)
                err(1, "client: rdma_connect failed");

            printf("client: rdma_connect!\n");

            size_t nregions = 32;
            size_t region_size = 4096;
            void *regions;
            ret = posix_memalign(&regions, 4096, nregions*region_size);
            if (ret)
                errx(1, "posix_memalign failed:%s", strerror(ret));

            struct ibv_mr *region_mr = ibv_reg_mr(client_id->pd, regions, region_size*nregions, IBV_ACCESS_LOCAL_WRITE);
            if (!region_mr)
                err(1, "ibv_reg_mr");

            struct ibv_recv_wr recv_wrs[nregions];
            struct ibv_sge     recv_sgs[nregions];
            for (size_t j=0; j<nregions; j++) {
                struct ibv_recv_wr *w  = recv_wrs + j;
                struct ibv_sge     *sg = recv_sgs + j;

                sg->addr = (uintptr_t)regions + (j*region_size);
                sg->length = region_size;
                sg->lkey = region_mr->lkey;

                w->wr_id = j;
                w->next = nullptr;
                w->sg_list = sg;
                w->num_sge = 1;

                struct ibv_recv_wr *bad_w;
                ret = ibv_post_recv(client_id->qp, w, &bad_w);
                if (ret)
                    errx(1, "ibv_post_recv: id=%zd err:%s", j, strerror(ret));
            }

            // try to send something
            struct ibv_sge sg;
            struct ibv_send_wr wr, *bad_wr;
            char buff[] = "I love pizza!";
            memset(&sg, 0, sizeof(sg));
            sg.addr = (uintptr_t)buff;
            sg.length = sizeof(buff);
            sg.lkey = 0; // INLINE, so it will not be checked

            memset(&wr,0, sizeof(wr));
            wr.wr_id      = 0;
            wr.sg_list    = &sg;
            wr.num_sge    = 1;
            wr.opcode     = IBV_WR_SEND;
            wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE ;

            ret = ibv_post_send(client_id->qp, &wr, &bad_wr);
            if (ret)
                err(1, "ibv_post_send");

            struct ibv_wc wc;
            for (;;) {
                ret = ibv_poll_cq(cq, 1 /* depth */, &wc);
                if (ret == 0) {
                    continue; // retry
                } if (ret < 0 || wc.status != IBV_WC_SUCCESS) {
                    errx(1, "%s: ibv_poll_cq failed: ret=%d err=(%d) %s", __PRETTY_FUNCTION__, ret, wc.status, ibv_wc_status_str(wc.status));
                }

                switch (wc.opcode) {
                    case IBV_WC_SEND:
                    printf("%s: IBV_WC_SEND: wr_id:%" PRIu64 "\n", __PRETTY_FUNCTION__, wc.wr_id);
                    break;

                    case IBV_WC_RECV:
                    printf("%s: IBV_WC_RECV: wr_id:%" PRIu64 "\n", __PRETTY_FUNCTION__, wc.wr_id);
                    printf("Received: %s\n", (char *)((uintptr_t)regions + region_size*wc.wr_id));
                    goto client_done;
                    break;

                    case IBV_WC_RDMA_WRITE:
                    case IBV_WC_RDMA_READ:
                    case IBV_WC_COMP_SWAP:
                    case IBV_WC_FETCH_ADD:
                    case IBV_WC_BIND_MW:
                    case IBV_WC_RECV_RDMA_WITH_IMM:
                    //case IBV_WC_LOCAL_INV:
                    default:
                    errx(1, "Unexpected wc.opcode: %d\n", wc.opcode);
                }

            }


        client_done:
            printf("Client DONE!\n");
            rdma_destroy_qp(client_id);
            ibv_destroy_cq(cq);
            rdma_destroy_id(client_id);
            sleep(1);
    }

    url_free_fields(&url);
    return;
}

int main(int argc, char *argv[])
{
    //const char server_url[] = "127.0.0.1:8000";
    static char server_url[128] = "192.168.1.64:8000";
    pid_t p;
    if ((p = fork()) == 0) {
        /* client */
        sleep(1);
        client(server_url);
        exit(0);
    } else if (p == -1) {
        fprintf(stderr, "fork() failed: %d (%s)", errno, strerror(errno));
        abort();
    }

    //server(server_url);

    trt::Controller c;
    c.spawn_scheduler(echo_server, (void *)server_url, trt::TaskType::TASK);

    printf("Waiting for client %d\n", p);
    p = waitpid(p, NULL, 0);
    printf("Client DONE.\n");

    return 0;
}
