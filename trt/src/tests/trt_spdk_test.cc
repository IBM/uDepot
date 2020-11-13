/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop:4 shiftwidth:4:

#include <memory>
#include <unistd.h>

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_spdk.hh"

#include <rte_config.h>
#include <rte_malloc.h>

using namespace trt;

#define READ_SIZE (1024*1024)

struct GlobArg {
    SpdkGlobalState g_spdk;
    std::string     g_namepace;
    GlobArg(const char *n) : g_namepace(n) {}
    void init(void) { g_spdk.init(); }
};

void *
t_io(void *arg__) {
    SpdkQpair *qp = static_cast<SpdkQpair *>(arg__);

    uint32_t sector_size = qp->sqp_namespace->get_sector_size();
    uint64_t spdk_buff_size = 4096;
    SpdkPtr spdk_buff = std::move(qp->alloc_buffer(4096 / sector_size));
    if (spdk_buff.ptr_m == nullptr) {
        fprintf(stderr, "alloc_buffer failed\n");
        exit(1);
    }

    trt_dmsg("Calling SPDK::read\n");
    SPDK::read(qp, spdk_buff, 0, spdk_buff_size / sector_size);

    qp->free_buffer(std::move(spdk_buff));
    return nullptr;
}

void *
t_init(void *arg__) {

    GlobArg *arg = static_cast<GlobArg *>(arg__);

    SPDK::init(arg->g_spdk);
    std::shared_ptr<SpdkQpair> qp = SPDK::getQpair(arg->g_namepace);

    T::spawn(SPDK::poller_task, nullptr, nullptr, true, TaskType::TASK);
    T::spawn(t_io, qp.get());
    T::task_wait();

    trt_dmsg("DONE: stopping SPDK\n");
    SPDK::stop();

    T::set_exit_all();
    return nullptr;
}

void usage(FILE *f, char *progname) {
    fprintf(f, "Usage: %s: [-h] [-t nthreads]\n", progname);
}

int main(int argc, char *argv[])
{
    int nthreads = 1;
    bool help = false;
    int opt;

    while ((opt = getopt(argc, argv, "ht:")) != -1) {
        switch (opt) {
            case 't':
            nthreads = atol(optarg);
            break;

            case 'h':
            help = true;
            break;

            default:
            usage(stderr, argv[0]);
            exit(1);
        }
    }

    if (help) {
        usage(stdout, argv[0]);
        exit(0);
    }

    if (nthreads <= 0) {
        fprintf(stderr, "Invalid ntrheads value: %d\n", nthreads);
        usage(stderr, argv[0]);
        exit(1);
    }

    GlobArg g("");
    g.init();
    Controller c;

    for (int i=0; i<nthreads; i++)
        c.spawn_scheduler(t_init, &g, TaskType::TASK);

    c.wait_for_all();
    return 0;
}
