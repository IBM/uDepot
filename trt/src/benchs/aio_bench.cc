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

// use kernel-based libaio, rather than libc's aio(7) which is implemented in
// user-space using threads. We use the kernel ABI directly.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/aio_abi.h>
#include <time.h>

#include <vector>

#define OP_SIZE 4096

#include <algorithm>
#include <boost/intrusive/list.hpp>
namespace bi = boost::intrusive;

// syscall wrappers
int io_setup(unsigned maxevents, aio_context_t *ctx) {
    return syscall(SYS_io_setup, maxevents, ctx);
}

int io_submit(aio_context_t ctx, long nr, struct iocb **iocbpp) {
    return syscall(SYS_io_submit, ctx, nr, iocbpp);
}

int io_getevents(aio_context_t ctx, long min_nr, long nr,
         struct io_event *events, struct timespec *timeout) {
    return syscall(SYS_io_getevents, ctx, min_nr, nr, events, timeout);
}

int io_destroy(aio_context_t ctx) {
    return syscall(SYS_io_destroy, ctx);
}

struct Op {
    bi::list_member_hook<> op_lnode_;
    struct iocb            op_iocb_;
    char                   op_buff_[OP_SIZE + 4096]; // add 4k for alignment

    Op(uint64_t opcode, int fd, off_t offset, size_t nbytes = OP_SIZE) {
        memset(&op_iocb_, 0, sizeof(op_iocb_));
        op_iocb_.aio_fildes     = fd;
        op_iocb_.aio_lio_opcode = opcode;
        op_iocb_.aio_reqprio    = 0;
        op_iocb_.aio_buf        = 4096 * ((4095 + (uintptr_t)op_buff_) / 4096);
        op_iocb_.aio_nbytes     = nbytes;
        op_iocb_.aio_offset     = offset;
        op_iocb_.aio_data       = (uintptr_t)this;
    }

    Op() { };
};

struct OpList {
    using List = bi::list<Op, bi::member_hook<Op, bi::list_member_hook<>, &Op::op_lnode_>>;

    std::vector<Op> ops_;
    List            ops_free_;

    OpList(size_t nops) {
        ops_.resize(nops);
        for (Op &op: ops_) {
            ops_free_.push_front(op);
        }
    }

    Op *alloc(void) {
        if (ops_free_.size() == 0) {
            return nullptr;
        }

        Op *o = &ops_free_.front();
        ops_free_.pop_front();
        return o;
    }

    void free(Op *op) {
        ops_free_.push_front(*op);
    }

    size_t nfree(void) { return ops_free_.size(); }
};

struct arg {
    int fd;
    size_t fsize;
    size_t max_batch;
    size_t max_inflight;
    size_t ops;
};

static void
do_bench(struct arg *arg) {

    size_t completed = 0;
    size_t submitted = 0;
    OpList ops(arg->max_inflight);
    const size_t nchunks = arg->fsize / OP_SIZE;
    aio_context_t aio_ctx = 0;
    struct io_event *events = (struct io_event *)malloc(sizeof(io_event)*arg->max_batch);

    if (io_setup(arg->max_inflight, &aio_ctx) < 0) {
        perror("io_queue_init failed\n");
        exit(1);
    }

    while (completed < arg->ops) {
        size_t inflight = submitted - completed;

        if (inflight < arg->max_inflight) {
            size_t b = std::min(arg->max_batch,
                                std::min(arg->max_inflight - inflight,
                                         ops.nfree()));

            struct iocb *iocbs[b];
            for (size_t i=0; i<b; i++) {
                Op *op__ = ops.alloc();
                off_t rand_off = (rand() % nchunks)*OP_SIZE;
                Op *op = new(op__) Op(IOCB_CMD_PREAD, arg->fd, rand_off);
                iocbs[i] = &op->op_iocb_;
                //printf("iocbs[%zd]=%p\n", i, iocbs[i]);
            }

            int ret = io_submit(aio_ctx, b, iocbs);
            if (ret != (int)b) {
                perror("io_submit");
                exit(1);
            }

            submitted += b;
            //printf("submitted: %zd (total: %zd)\n", b, submitted);
        }

        struct timespec ts; ts.tv_nsec = 1; ts.tv_sec = 0;
        int nevents = io_getevents(aio_ctx, 0, arg->max_batch, events, &ts);
        if (nevents < 0) {
            perror("io_getevents");
            exit(1);
        }

        for (int i=0; i<nevents; i++) {
            struct io_event *ev = &events[i];
            Op *op = (Op *)ev->data;
            struct iocb *iocb_ptr = (struct iocb *)ev->obj;
            assert(&op->op_iocb_ == iocb_ptr);
            if (ev->res < 0) {
                fprintf(stderr, "aio event error: %s for %p\n", strerror(-ev->res), iocb_ptr);
                abort();
            }
            if (ev->res2 < 0) {
                fprintf(stderr, "aio event error (res2): %s\n", strerror(-ev->res2));
                exit(1);
            }
            ops.free(op);
        }

        completed += nevents;
        //printf("completed: %d (total: %zd)\n", nevents, completed);
    }

    free(events);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s filename\n", argv[0]);
        exit(1);
    }

    const size_t ops = 1024*1024*1024;
    const size_t max_batch = 8;
    const size_t max_inflight = 128;

    srand(time(NULL));

    int fd = open(argv[1], O_RDONLY | O_DIRECT);
    if (fd == -1) {
        perror(argv[1]);
        exit(1);
    }

    size_t fsize = lseek64(fd, 0, SEEK_END);
    if (fsize == (size_t)-1) {
        perror("lseek64");
        exit(1);
    }

    struct arg arg;
    arg.fd           = fd;
    arg.fsize        = fsize;
    arg.ops          = ops;
    arg.max_batch    = max_batch;
    arg.max_inflight = max_inflight;

    do_bench(&arg);

    return 0;
}
