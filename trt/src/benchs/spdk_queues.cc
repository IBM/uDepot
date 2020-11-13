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

#include <vector>
#include <memory>
#include <cstdlib>
#include <cinttypes>
#include <random>
#include <chrono>
#include <string>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <typeinfo>
#include <cxxabi.h>
#include <unistd.h>
#include <algorithm>

#include "trt_util/spdk.hh"
#include "trt_util/ticks.hh"
#include "trt_util/arg_pool.hh"
#include "trt_util/sdt.h"


// What is the effect of using seperate queues for IO streams?

struct Class;
struct Task;

using Clock = std::chrono::steady_clock;

struct TaskArg {
    uint64_t  ta_ticks_start;
    Task      *ta_task;
};

struct Task {
    static size_t              glbl_task_next_id;

    size_t                     t_id;
    unsigned                   t_queue_depth;
    uint64_t                   t_ticks_end;
    std::shared_ptr<SpdkQpair> t_qp;
    bool                       t_finished, t_done;
    Class                      *t_class;
    ArgPool<TaskArg>           t_args;
    size_t                     t_ops;
    uint64_t                   t_ops_ticks_total;
    uint64_t                   t_ops_ticks_max;
    Clock::time_point          t_tp_start;

    Task(unsigned qd)
        : t_id(glbl_task_next_id++)
        , t_queue_depth(qd)
        , t_finished(false)
        , t_done(false)
        , t_args(qd)
        , t_ops(0)
        , t_ops_ticks_total(0)
        , t_ops_ticks_max(0) {}

    virtual std::string info(void) {
        int status;
        const char *name_ = typeid(*this).name();
        const char *name = abi::__cxa_demangle(name_, 0, 0, &status);
        std::stringstream ss;
        ss << "task: " << std::setw(3) << t_id << " "
           << "qd: "   << std::setw(3) << t_queue_depth << " "
           << "type: " << std::setw(10) << name << " ";
        return ss.str();
    }

    // Factory method
    static std::shared_ptr<Task> from_string(std::string cnf);

    virtual void init(Class *c, std::shared_ptr<SpdkQpair> qp, uint64_t t) {
        t_class = c;
        t_qp = qp;
        t_ticks_end = t;
        t_tp_start = Clock::now();
    }

    virtual ~Task() {
        if (t_finished) {
            std::chrono::duration<double> secs = (Clock::now() - t_tp_start);
            printf("task %zd : %8.1lf kIOPS. Average Kticks/op: %7" PRIu64 ".  Max Kticks/op: %" PRIu64 "7. (%zd operations on %.2lf secs.)\n",
                t_id,
                (double)(t_ops) / (secs.count()*1000.0),
                t_ops_ticks_total / (t_ops*1000),
                t_ops_ticks_max   / 1000 ,
                t_ops,
                secs.count());
        }
    }

    virtual void submit_op() = 0;
    virtual void complete_op(TaskArg *ta) = 0;

    TaskArg *base_submit_op();
    void base_complete_op(TaskArg *ta);
};

struct BenchConf {
    std::vector<std::vector<std::shared_ptr<Task>>> b_task_classes;
    bool                                            b_dry_run;
    uint64_t                                        b_ticks;
    std::string                                     b_scheduler;
    unsigned                                        b_poll_limit;

    BenchConf()
        : b_dry_run(false)
        , b_ticks(40000000000)
        , b_scheduler("rr")
        , b_poll_limit(16) {}

    void print() {
        std::cout << "ticks: "      << b_ticks << std::endl;
        std::cout << "scheduler: "  << b_scheduler << std::endl;
        std::cout << "poll_limit: " << b_poll_limit << std::endl;
        for (unsigned i=0; i<b_task_classes.size(); i++) {
            auto &c = b_task_classes[i];
            std::cout << "class: " << i << std::endl;
            for (auto t: c) {
                std::cout << "\t" << t->info() << std::endl;
            }
        }
    }

    int add_task_class(std::string);
};

size_t Task::glbl_task_next_id = 0;

void task_callback(void *arg, const struct spdk_nvme_cpl *cpl) {
    TaskArg *ta = static_cast<TaskArg *>(arg);
    Task *t = ta->ta_task;

    if (spdk_nvme_cpl_is_error(cpl)) {
        fprintf(stderr, "*********** Error in completion\n");
    }

    //printf("cb: t=%p ta\n", t);
    t->complete_op(ta);
    //printf("finished? %d\n", t->t_finished);
}

struct TaskRandRD : Task {
    std::default_random_engine            t_rand_eng_;
    std::uniform_int_distribution<size_t> t_rand_dist_;

    SpdkPtr                               t_buff;
    size_t                                t_qp_nblocks;

    TaskRandRD(unsigned qd)
        : Task(qd), t_rand_eng_(time(NULL)) {}

    virtual void init(Class *c, std::shared_ptr<SpdkQpair> qp, uint64_t t) override {
        Task::init(c, qp, t);
        #if 0
        t_qp_nblocks = qp->get_nsectors();
        size_t bsize = qp->get_sector_size();
        #else
        size_t bsize = 4096;
        t_qp_nblocks = qp->get_nsectors() / (4096 / qp->get_sector_size());
        #endif
        t_buff = std::move(SpdkAlloc::alloc_iobuff(bsize));
    }

    virtual ~TaskRandRD() {
        SpdkAlloc::free_iobuff(std::move(t_buff));
    }

    virtual void submit_op() override {
        TaskArg *ta = Task::base_submit_op();
        size_t lba = t_rand_dist_(t_rand_eng_) % t_qp_nblocks;
        t_qp->submit_read(t_buff, lba, 1, task_callback, ta);
    }

    virtual void complete_op(TaskArg *ta) override {
        t_qp->npending_dec(1);
        Task::base_complete_op(ta);
    }
};

struct ClassStats {
    uint64_t total_ticks;             // how many ticks in total
    uint64_t total_polls;             // how many ->polls() in total
    uint64_t total_cbs;               // how many callbacks where executed in total
    uint64_t total_unnecessary_polls; // how many ->polls() did not execute any callbacks
    uint64_t total_unnecessary_ticks; // ticks for the unnecessary polls
    uint64_t total_full_polls;        // how many ->polls() executed callbacks equal to the limit

    uint64_t last_ticks;  // how many ticks in the last ->poll()
    uint64_t last_cbs;    // how many callbacks where executed in the last ->poll()

    ClassStats()
        : total_ticks(0)
        , total_polls(0)
        , total_cbs(0)
        , total_unnecessary_polls(0)
        , total_unnecessary_ticks(0)
        , last_ticks(0)
        , last_cbs(0) {}

    // wrapper for ->poll() that also updates the statistics
    inline size_t poll(Class &c, unsigned n);

    void print(void) {
        printf("total_ticks:             %15" PRIu64 "\n", total_ticks);
        printf("total_polls:             %15" PRIu64 "\n", total_polls);
        printf("total_cbs:               %15" PRIu64 "\n", total_cbs);
        printf("total_unnecessary_polls: %15" PRIu64 "\n", total_unnecessary_polls);
        printf("total_unnecessary_ticks: %15" PRIu64 "\n", total_unnecessary_ticks);
        printf("total_full_polls:        %15" PRIu64 "\n", total_full_polls);
    }
};


struct Class {
    SpdkState                          cl_spdk;
    std::vector<std::shared_ptr<Task>> cl_tasks;
    std::shared_ptr<SpdkQpair>         cl_qp;
    size_t                             cl_active_tasks;
    ClassStats                         cl_stats;

    Class(SpdkGlobalState &gs, std::vector<std::shared_ptr<Task>> &&tasks)
        : cl_spdk(gs),
          cl_tasks(tasks),
          cl_qp(cl_spdk.getQpair(std::string())),
          cl_active_tasks(0) {}

    Class(Class &&c) {
        cl_spdk         = std::move(c.cl_spdk);
        cl_tasks        = std::move(c.cl_tasks);
        cl_qp           = std::move(c.cl_qp);
        cl_active_tasks = std::move(c.cl_active_tasks);
        cl_stats        = std::move(c.cl_stats);
    }

    Class() = delete;
    Class(Class const&) = delete;
    void operator=(Class const&) = delete;


    // ~Class does not work for whatever reason
    void stop(void) {
        cl_spdk.stop();
        // by the time we call this, queues should have be drained
        while (!cl_spdk.is_done()) {
            //printf("Polling until queues are drained\n");
            poll(0);
        }
        // and now, we can drop the reference on the queue now
        cl_qp.reset();
    }

    size_t poll(unsigned n) {
       return cl_qp->execute_completions(n);
    }

    size_t poll_and_update_stats(unsigned n) {
        return cl_stats.poll(*this, n);
    }

    void init_ops(uint64_t ticks_end);
    void task_done();
};

size_t ClassStats::poll(Class &c, unsigned n) {
    size_t t = ticks::get_ticks();
    size_t ret_n = c.poll(n);
    t = ticks::get_ticks() - t;

    last_ticks   = t;
    last_cbs     = ret_n;

    total_ticks += t;
    total_polls += 1;
    total_cbs   += ret_n;


    if (ret_n == 0) {
        total_unnecessary_polls++;
        total_unnecessary_ticks += t;
    } else if (ret_n == n) {
        total_full_polls++;
    }

    return ret_n;
}

TaskArg *
Task::base_submit_op() {
    //printf("--> %s\n", __PRETTY_FUNCTION__);
    //std::cout << "SUBMIT task id:" << t_id << std::endl;
    TaskArg *arg = t_args.get_arg();
    assert(arg != nullptr);
    arg->ta_task = this;
    arg->ta_ticks_start = ticks::get_ticks();
    DTRACE_PROBE2(spdk_queues, submit_op, t_id, arg->ta_ticks_start);
    return arg;
}

void Task::base_complete_op(TaskArg *ta) {
    //printf("<-- %s\n", __PRETTY_FUNCTION__);
    uint64_t now = ticks::get_ticks();
    uint64_t op_ticks = now - ta->ta_ticks_start;
    DTRACE_PROBE3(spdk_queues, complete_op, t_id, now, op_ticks);

    // printf("task %zd completed op in %" PRIu64 " ticks\n", t_id, op_ticks);
    t_ops++;
    t_ops_ticks_total += op_ticks;
    if (op_ticks > t_ops_ticks_max)
        t_ops_ticks_max = op_ticks;

    if (!t_finished)
        t_finished = now >= t_ticks_end;

    t_args.put_arg(ta);
    if (t_finished) {
        if (!t_done) { // notify class only once
            t_class->task_done();
            t_done = true;
        }
    } else {
        submit_op();
    }
}

void Class::init_ops(uint64_t ticks_end) {
    for (auto &task: cl_tasks) {
        task->init(this, cl_qp, ticks_end);
        cl_active_tasks++;
        for (unsigned i=0; i<task->t_queue_depth; i++) {
            task->submit_op();
        }
    }
}

void Class::task_done(void) {
    //printf("%s\n", __PRETTY_FUNCTION__);
    assert(cl_active_tasks > 0);
    cl_active_tasks--;
}

struct Scheduler {
    std::vector<Class> &s_classes;
    unsigned            s_poll_limit;

    Scheduler(std::vector<Class> &classes, unsigned poll_limit)
        : s_classes(classes)
        , s_poll_limit(poll_limit) {}

    virtual ~Scheduler() {}

    Scheduler() = delete;
    Scheduler(Scheduler const&) = delete;
    void operator=(Scheduler const&) = delete;

    // returns true if there are still active tasks to schedule
    virtual void schedule() = 0;

    void init(uint64_t ticks) {
        uint64_t t0 = ticks::get_ticks();
        for (auto &c: s_classes)
            c.init_ops(ticks + t0);
    }

    void stop() {
        std::cout << "Stopping classes" << std::endl;
        for (auto &c: s_classes)
            c.stop();

        std::cout << "Classes stats" << std::endl;
        for (size_t i=0; i < s_classes.size(); i++) {
            std::cout << "class: " << i << std::endl;
            s_classes[i].cl_stats.print();
        }
    }

    static std::unique_ptr<Scheduler>
    from_string(std::string s, std::vector<Class> &classes, unsigned poll_limit);
};

struct RoundRobinSched : Scheduler  {

    RoundRobinSched(std::vector<Class> &classes, unsigned poll_limit)
        : Scheduler(classes, poll_limit) {}

    void schedule(void) override {
        bool done;
        do {
            done = true;
            for (auto &c: s_classes) {
                //printf("c: %p active_tasks: %zd\n", &c, c.cl_active_tasks);
                if (c.cl_active_tasks > 0) {
                    c.poll_and_update_stats(s_poll_limit);
                    done = false;
                }
            }
        } while (!done);
    }
};

struct WeightedRRSched : Scheduler {

    WeightedRRSched(std::vector<Class> &classes, unsigned poll_limit)
        : Scheduler(classes, poll_limit) {}

    void schedule(void) override {
        for (;;) {
            size_t ncbs = 0;
            size_t nactive = 0;
            for (auto &c: s_classes) {
                if (c.cl_active_tasks == 0)
                    continue;
                nactive++;
                ncbs += c.cl_stats.last_cbs;
            }

            if (nactive == 0)
                break;

            size_t limit = ncbs / nactive;
            //printf("ncbs: %zd nactive: %zd limit: %zd\n", ncbs, nactive, limit);

            for (auto &c: s_classes) {
                // printf("c: %p active_tasks: %zd\n", &c, c.cl_active_tasks);
                if (c.cl_active_tasks > 0) {
                    c.poll_and_update_stats(limit);
                }
            }
        }
    }
};

// At its current form, this is not a great policy because if you have a fast
// and a slow submitter, the slow will never be able to catch up, so a lot of
// time will be (needlesly) spend in polling the slow one.
//
// Keeping it for reference (at least for now).
struct FairSched : Scheduler {
    struct ClassStats {
        uint64_t total_ticks;      // total time this class was executed
        uint64_t ticks_per_task;   // prediction of how many ticks a task takes

        ClassStats() : total_ticks(0), ticks_per_task(0) {}
    };

    std::vector<ClassStats> fs_stats;

    FairSched(std::vector<Class> &classes, unsigned poll_limit)
        : Scheduler(classes, poll_limit), fs_stats(classes.size()) {}

    void schedule(void) override {

        // C++ functions build a max-heap
        // The compare function should returns true if the first argument is
        // less than the second.
        // We want the maximum element (top of heap) to be the one with the
        // minimum value of total_ticks
        auto cmp_fn = [this] (size_t i1, size_t i2) -> bool {
            return fs_stats[i1].total_ticks > fs_stats[i2].total_ticks;
        };

        std::vector<size_t> heap(s_classes.size());
        for (size_t i=0; i < s_classes.size(); i++)
            heap[i] = i;
        std::make_heap(heap.begin(), heap.end(), cmp_fn);

        while (true) {
            for (auto &c_idx: heap) {
                std::cout << "class: "       << c_idx
                          << " total_ticks: " << fs_stats[c_idx].total_ticks
                          << " qd:" << s_classes[c_idx].cl_qp->sqp_npending
                          << std::endl;
            }

            if (heap.size() == 0)
                break;

            // select the next class to poll and its poll limit
            size_t next_idx;
            unsigned poll_limit;
            if (heap.size() == 1) {
                next_idx = heap.back();
                heap.pop_back();
                poll_limit = s_poll_limit;
            } else {
                // pop class from heap
                std::pop_heap(heap.begin(), heap.end(), cmp_fn);
                next_idx = heap.back();
                auto &next_stats = fs_stats[next_idx];
                heap.pop_back();
                // figure out poll limit by looking at the next class to be
                // scheduled
                size_t next_ticks = next_stats.total_ticks;
                size_t next_next_ticks = fs_stats[heap[0]].total_ticks;
                assert(next_ticks <= next_next_ticks);
                poll_limit = next_stats.ticks_per_task == 0
                    ? s_poll_limit
                    : std::min(1UL, (next_next_ticks - next_ticks) / next_stats.ticks_per_task);
            }
            // poll the class
            std::cout << "********** selected class: " << next_idx
                      << " poll_limit: " << poll_limit
                      << std::endl;
            auto &next_class = s_classes[next_idx];
            auto &next_stats = fs_stats[next_idx];
            uint64_t t = ticks::get_ticks();
            size_t ntasks = next_class.poll(poll_limit);
            t = ticks::get_ticks() - t;
            // update class stats
            next_stats.ticks_per_task = t / (ntasks == 0 ? 1 : ntasks);
            next_stats.total_ticks += t;
            // if class has more active tasks, add it to the queue again
            printf("active tasks: %zd\n", next_class.cl_active_tasks);
            if (next_class.cl_active_tasks > 0) {
                heap.push_back(next_idx);
                std::push_heap(heap.begin(), heap.end(), cmp_fn);
            }
        }
    }
};


std::unique_ptr<Scheduler>
Scheduler::from_string(std::string s,
                       std::vector<Class> &classes,
                       unsigned poll_limit) {
    // NB: use std::make_unique() when we move into c++14
    if (s == "rr") {
        return std::unique_ptr<RoundRobinSched>(new RoundRobinSched(classes, poll_limit));
    } else if (s == "fair") {
        return std::unique_ptr<FairSched>(new FairSched(classes, poll_limit));
    } else if (s == "wrr") {
        return std::unique_ptr<WeightedRRSched>(new WeightedRRSched(classes, poll_limit));
    }

    std::cerr << "Unknown scheduler: %s" << s << std::endl;
    return nullptr;
}

int bench(BenchConf &&conf) {

    conf.print();
    if (conf.b_dry_run)
        return 0;

    // initialize
    std::vector<Class> classes;
    SpdkGlobalState gs;
    gs.init();
    for (auto &tasks: conf.b_task_classes) {
        classes.emplace_back(gs, std::move(tasks));
    }

    Clock::time_point t = Clock::now();

    std::unique_ptr<Scheduler> sched = Scheduler::from_string(conf.b_scheduler, classes, conf.b_poll_limit);
    if (!sched)
        return EINVAL;

    printf("starting: initializing classses and tasks...\n");
    uint64_t ticks = conf.b_ticks;
    sched->init(ticks);
    printf("Scheduling (polling)...\n");
    sched->schedule();
    printf("Exiting...\n");
    sched->stop();

    std::chrono::duration<double> secs = (Clock::now() - t);
    printf("Duration: %.1f secs\n", secs.count());
    return 0;
}

std::shared_ptr<Task>
Task::from_string(std::string cnf) {
    long qd = std::atol(cnf.c_str());
    if (qd <= 0) {
        fprintf(stderr, "cannot create a Task from %s\n", cnf.c_str());
        return nullptr;
    } else {
        return std::make_shared<TaskRandRD>((unsigned)qd);
    }
}

int
BenchConf::add_task_class(std::string class_conf) {
    std::vector<std::shared_ptr<Task>> tasks;
    std::istringstream is;
    is.str(class_conf);

    while (is.good()) {
        std::string task_conf;
        std::getline(is, task_conf, ',');
        std::shared_ptr<Task> t = Task::from_string(task_conf);
        if (t == nullptr)
            return EINVAL;
        tasks.push_back(std::move(t));
    }

    b_task_classes.push_back(tasks);
    return 0;
}

void usage(const char *progname) {
    printf("%s: [-c task_class] [-h] [-n]\n", progname);
    printf("  -n  Do a dry run\n");
    printf("  -c  Add a task class\n");
    printf("      A task_class is parsed as a comma-separated list of tasks\n");
    printf("      Each task is (for now) just a number designating its queue depth\n");
    printf("  -s scheduler (rr: round robin, fair: fair scheduler)\n");
    printf("  -t ticks (how long to run)\n");
    printf("  -l default poll limit\n");
}

int main(int argc, char *argv[])
{
    int err;
    char opt;
    BenchConf conf;

    while ((opt = getopt(argc, argv, "c:hnt:s:l:")) != -1) {
        switch (opt) {
            case 'c': {
                err = conf.add_task_class(std::string(optarg));
                if (err) {
                    std::cerr << "Failed to add task class: " << optarg << std::endl;
                    usage(argv[0]);
                    exit(1);
                }
            } break;

            case 'h':
                usage(argv[0]);
                exit(0);

            case 't':
                conf.b_ticks = atol(optarg);
                if (conf.b_ticks <= 0) {
                    std::cerr << "Invalid ticks value: " << optarg << std::endl;
                    usage(argv[0]);
                    exit(1);
                }
                break;

            case 'l':
                conf.b_poll_limit = atol(optarg);
                break;

            case 's':
                conf.b_scheduler = std::string(optarg);
                break;

            case 'n':
                conf.b_dry_run = true;
                break;

            case '?':
            default:
                usage(argv[0]);
                exit(1);
        }
    }

    err = bench(std::move(conf));
    if (err) {
        usage(argv[0]);
        exit(1);
    }
}
