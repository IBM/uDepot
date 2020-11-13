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

#include <stdio.h>
#include <inttypes.h>

#include <rte_config.h>
//#include <rte_cycles.h>
//#include <rte_mempool.h>
//#include <rte_malloc.h>
#include <rte_lcore.h>

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_spdk.hh"

extern "C" {
    #include "trt_util/timer.h"
}
#include "trt_util/arg_pool.hh"



// benchmark configuration
struct Conf {
    unsigned nthreads;
    unsigned ndevices;
    unsigned op_size;
    unsigned ops_per_task;
    unsigned ops_executing_max;
    uint64_t nops;

    Conf() : nthreads(1),
             ndevices(1),
             op_size(4096),
             ops_per_task(1),
             ops_executing_max(32),
             nops(1000000) {}
};

struct thread_part {
    uint64_t start, len;
    thread_part() : start(-1), len(-1) {}

    thread_part(uint64_t total, uint32_t nthreads, uint32_t thread_id) {
        len = total / nthreads;
        start = thread_id*len;
        if (thread_id == nthreads - 1)
            len += total % nthreads;
    }
};

struct ThreadArg {
    Conf            &conf;
    unsigned        tid;
    thread_part     partition;
    SpdkGlobalState &spdk_gs;

    ThreadArg(Conf &conf_, unsigned tid_, SpdkGlobalState &spdk_gs_) :
        conf(conf_),
        tid(tid_),
        partition(conf.nops, conf.nthreads, tid),
        spdk_gs(spdk_gs_)
        {}
};

struct TaskArg {
    size_t           op_nlbas;
    SpdkPtr          op_buff;

    SpdkQpair        *spdk_qp;
    size_t           nr_ops;
    ArgPool<TaskArg> *ap;
    size_t           *tasks_completed;

    bool is_initialized(void) { return op_buff.ptr_m != nullptr;}

    void init(size_t op_size, size_t sector_size) {
        op_nlbas = (op_size + sector_size - 1) / sector_size;
        op_buff = std::move(SpdkAlloc::alloc_iobuff(op_nlbas*sector_size));
        if (op_buff.ptr_m == nullptr) {
            fprintf(stderr, "RtePtr::malloc() failed\n");
            abort();
        }
    }

};

struct SpawnerArg {
    ThreadArg                &thr_arg;
    size_t                   tasks_executing_max;
    ArgPool<TaskArg>         task_args;
    std::vector<SpdkQpair *> qpairs;
private:
    size_t                   qp_idx;

public:
    SpawnerArg(ThreadArg &thr_arg_, std::vector<SpdkQpair *> qpairs_):
        thr_arg(thr_arg_),
        tasks_executing_max(thr_arg.conf.ops_executing_max / thr_arg.conf.ops_per_task),
        task_args(tasks_executing_max),
        qpairs(qpairs_),
        qp_idx(0)
        {}

    SpdkQpair *get_next_qp_rr() {
        SpdkQpair *ret = qpairs[qp_idx];
        if (++qp_idx == qpairs.size())
            qp_idx = 0;
        return ret;
    }
};

static void usage(FILE *f, char *progname) {
    fprintf(f, "Usage: %s: [-h] [-t nthreads] [-d ndevices] [-n ops] [-s op_size] [-o ops_per_task] [-q ops_execting_max]\n", progname);
}

Conf parse_args(int argc, char *argv[]) {

    Conf cnf;
    bool help = false;
    int opt;

    while ((opt = getopt(argc, argv, "ht:d:n:s:o:q:")) != -1) {
        switch (opt) {

            case 'h':
            help = true;
            break;

            case 't':
            cnf.nthreads = atol(optarg);
            break;

            case 'd':
            cnf.ndevices = atol(optarg);
            break;

            case 'n':
            cnf.nops = atol(optarg);
            break;

            case 's':
            cnf.op_size = atol(optarg);
            break;

            case 'o':
            cnf.ops_per_task = atol(optarg);
            break;

            case 'q':
            cnf.ops_executing_max = atol(optarg);
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

    return cnf;
}

static void *
t_worker(void *arg_) {
    TaskArg *arg = static_cast<TaskArg *>(arg_);
    size_t dev_lbas = arg->spdk_qp->get_nsectors();
    //trt_dmsg("nops=%zd qp=%p dev_lbas=%zd\n", arg->nr_ops, arg->spdk_qp, dev_lbas);
    trt::T::rand();

    for (size_t i=0; i < arg->nr_ops; i++) {
        size_t lba0 = (trt::T::rand() % (dev_lbas - arg->op_nlbas));
        trt::SPDK::read(arg->spdk_qp, arg->op_buff, lba0, arg->op_nlbas);
    }

    arg->ap->put_arg(arg);
    *(arg->tasks_completed) += 1;
    //trt_dmsg("DONE\n");
    return nullptr;
}

static void *
t_spawner(void *arg_) {
    //trt_dmsg("spawner task: enter\n");
    SpawnerArg *arg = static_cast<SpawnerArg *>(arg_);
    assert(arg != nullptr);

    const size_t ops_total    = arg->thr_arg.partition.len;
    const size_t ops_per_task = arg->thr_arg.conf.ops_per_task;
    const size_t tasks_total  = (ops_total + ops_per_task - 1) / ops_per_task;

    size_t tasks_spawned = 0, tasks_completed = 0, ops_spawned = 0;
    while (tasks_spawned < tasks_total) {
        //trt_dmsg("tasks_completed: %zd tasks_spawned: %zd\n", tasks_completed, tasks_spawned);
        assert(tasks_completed <= tasks_spawned);
        size_t tasks_executing = tasks_spawned - tasks_completed;
        assert(tasks_executing <= arg->tasks_executing_max);
        size_t spawn_tasks_nr = arg->tasks_executing_max - tasks_executing;
        if (spawn_tasks_nr == 0) {
            trt::T::yield();
            continue;
        }

        //trt_dmsg("Going to spawn: %zd tasks (completed:%zd)\n", spawn_tasks_nr, tasks_completed);
        trt::Task::List tl;
        for (size_t i=0; i<spawn_tasks_nr; i++) {
            size_t task_ops = std::min(ops_per_task, ops_total - ops_spawned);
            if (task_ops == 0)
                break;
            TaskArg *targ = arg->task_args.get_arg();
            assert(targ != nullptr);
            targ->ap              = &arg->task_args;
            targ->nr_ops          = task_ops;
            targ->tasks_completed = &tasks_completed;
            targ->spdk_qp         = arg->get_next_qp_rr();

            ops_spawned += task_ops;

            trt::Task *t = trt::T::alloc_task(t_worker, targ, nullptr, true);
            tl.push_back(*t);
        }

        //trt_dmsg("Going to spawn: %zd tasks\n", tl.size());
        tasks_spawned += tl.size();
        trt::T::spawn_many(tl);
        trt::T::yield();
    }
    assert(ops_spawned == ops_total);

    while (tasks_completed != tasks_spawned)
        trt::T::yield();

    return nullptr;
}

static void *
t_main(void *arg) {
    ThreadArg *targ = static_cast<ThreadArg *>(arg);
    trt_dmsg("tid=%u\n", targ->tid);

    trt_dmsg("Initializing SPDK\n");
    trt::SPDK::init(targ->spdk_gs);
    trt_dmsg("Spawining SPDK poller\n");
    trt::T::spawn(trt::SPDK::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);
    trt_dmsg("init done\n");

    std::vector<SpdkQpair *> qpairs;
    auto ns_names = trt::SPDK::getNamespaceNames();
    size_t ndevices = std::min(ns_names.size(), static_cast<size_t>(targ->conf.ndevices));
    trt_dmsg("%s: using %zd devices\n", __PRETTY_FUNCTION__, ndevices);
    for (size_t i=0; i<ndevices; i++) {
        qpairs.emplace_back(trt::SPDK::getQpair(ns_names[i]).get());
    }

    SpawnerArg spawner_arg(*targ, qpairs);

    size_t i = 0;
    for (;;) {
        // since we now that it's a FIFO...
        TaskArg *a = spawner_arg.task_args.get_arg();
        assert(a != nullptr);
        if (a->is_initialized()) {
            spawner_arg.task_args.put_arg(a);
            break;
        }

        size_t op_size     = targ->conf.op_size;
        // This assumes that all queues have the same block size, which
        // in practice should be true.
        size_t sector_size = qpairs[0]->get_sector_size();
        a->init(op_size, sector_size);
        spawner_arg.task_args.put_arg(a);
        i++;
    }
    printf("Initialized %zd task arguments (free:%zd)\n", i, spawner_arg.task_args.free_nr());

    xtimer_t t; timer_init(&t); timer_start(&t);
    trt::T::spawn(t_spawner, static_cast<void *>(&spawner_arg), nullptr, false, trt::TaskType::POLL);
    trt::T::task_wait();
    timer_pause(&t);
    double s = timer_secs(&t);
    printf("===> time:%lfsecs tput:%lf Mops/sec bw:%lf MiB/sec ops:%lu\n",
        s,
        (double)targ->partition.len / (s*1000*1000),
        (double)targ->partition.len * targ->conf.op_size / (s*1024*1024),
        targ->partition.len);

    trt::SPDK::stop();
    if (targ->tid == 0) {
        trt_dmsg("Time to exit\n");
        trt::T::set_exit_all();
    }

    return nullptr;
}

int main(int argc, char *argv[])
{
    Conf cnf;
    SpdkGlobalState spdk_gs;
    trt::RteController c;

    cnf = parse_args(argc, argv);

    spdk_gs.init(); // this will initilize rte

    unsigned lcores_nr = rte_lcore_count();
    printf("lcores_nr=%u\n", lcores_nr);
    if (lcores_nr == 0) {
        fprintf(stderr, "Did you initialize RTE?\n");
        abort();
    }
    if (cnf.nthreads > lcores_nr - 1) {
        cnf.nthreads = lcores_nr - 1;
        printf("Note: truncated  #threads to #available slave cores: %u\n", cnf.nthreads);
    }
    printf("Conf: nthreads:%u ndevices:%u op_size:%u ops_per_task:%u nops:%" PRIu64 " ops_executing_max:%u\n",
            cnf.nthreads, cnf.ndevices, cnf.op_size, cnf.ops_per_task, cnf.nops, cnf.ops_executing_max);

    std::deque<ThreadArg> thread_confs;

    // spawn trt schedulers in all slave rte lcores. master core remains idle
    // (makes things simpler).
    unsigned lcore, i=0;
    RTE_LCORE_FOREACH_SLAVE(lcore) {
        printf("spawning scheduler on lcore=%u\n", lcore);
        thread_confs.emplace_back(cnf, i, spdk_gs);
        ThreadArg *t_arg = &thread_confs.back();
        c.spawn_scheduler(t_main, static_cast<void *>(t_arg), trt::TaskType::TASK, lcore);
        if (++i >= cnf.nthreads)
            break;
    }

    c.wait_for_all();
    return 0;
}
