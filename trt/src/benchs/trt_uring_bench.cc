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
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_uring.hh"

extern "C" {
    #include "trt_util/timer.h"
}

using namespace trt;

#define FILE_SIZE (128*1024*1024)
#define FILE_FLAGS (O_DIRECT)
#define BUFF_SIZE (4096)

#define NOPS_TOTAL (1024*1024)
#define NOPS_BATCH 64

static int
make_tempfile(char *tmpname, size_t file_size, size_t buff_size) {
    int fd;
    char *buff;

    printf("Creating random file\n");
    fd = mkostemp(tmpname, FILE_FLAGS);
    if (fd < 0) {
        perror("mkostemp");
        exit(1);
    }

    int ret = ftruncate(fd, file_size);
    if (ret < 0) {
        perror("ftruncate");
        exit(1);
    }

    if (posix_memalign((void **)&buff, 4096, buff_size) != 0) {
        perror("posix_memalign");
        exit(1);
    }

    for (size_t off=0; off < file_size; off+= buff_size) {
        for (size_t i=0; i<buff_size; i++)
            buff[i] = 'a' + ((off / buff_size) % ('z' + 1));

        ret = pwrite(fd, buff, buff_size, off);
        if (ret != (int)buff_size) {
            perror("pwrite");
            exit(1);
        }
    }

    free(buff);
    return fd;
}

struct targ {
    int fd;
    bool verify;
    size_t file_size;
    size_t buff_size;
};

void *t_io(void *arg__) {
    struct targ *arg = (struct targ *)arg__;
    const size_t nchunks = arg->file_size / arg->buff_size;
    size_t chunk = rand() % nchunks;
    int ret;

    char *buff;
    #if 0
    if (posix_memalign((void **)&buff, 4096, arg->buff_size) != 0) {
        perror("posix_memalign");
        exit(1);
    }
    #else
    char buff__[arg->buff_size] __attribute__((aligned(4096)));
    buff = buff__;
    #endif

    //trt_dmsg("PREAD initiated\n");
    ret = IOU::pread(arg->fd, buff, arg->buff_size, chunk*arg->buff_size);
    if (ret < 0) {
        fprintf(stderr, "err=%d\n", ret);
        perror("iou_pread");
        exit(1);
    }
    assert(ret == (int)arg->buff_size);
    //trt_dmsg("PREAD returned\n");

    if (arg->verify) {
        const char c = 'a' + (chunk % ('z' + 1));
        for (size_t i=0; i<arg->buff_size; i++) {
            if (buff[i] != c) {
                fprintf(stderr, "Error!");
                exit(1);
            }
        }
    }

    #if 0
    free(buff);
    #endif
    //trt_dmsg("DONE\n");
    return nullptr;
}

void *t_main(void *arg__) {
    struct targ *arg = (struct targ *)arg__;
    const size_t nops_total = NOPS_TOTAL;
    const size_t nops_batch = NOPS_BATCH;
    size_t nops_submitted = 0;
    size_t nops_completed = 0;

    for (;;) {
        // keep up to @nops_batch in flight
        while (nops_submitted < nops_total &&
               nops_submitted - nops_completed < nops_batch) {
            //trt_dmsg("%s: spawn io task (submitted:%zd)\n", __FUNCTION__, nops_submitted + 1);
            T::spawn(t_io, arg);
            nops_submitted++;
        }

        if (nops_submitted > nops_completed) {
            //trt_dmsg("WAITING\n");
            T::task_wait();
            nops_completed++;
        }

        if (nops_completed == nops_total)
            break;
    }

    trt_dmsg("%s: YIELDING\n", __FUNCTION__);
    T::yield(); // allow all submitters to write
    IOU::stop(); // stop uring
    trt_dmsg("%s: DONE\n", __FUNCTION__);

    return nullptr;
}

void *t_main_detached(void *arg__) {
    struct targ *arg = (struct targ *)arg__;
    const size_t nops_total = NOPS_TOTAL;
    const size_t nops_batch = NOPS_BATCH;

    size_t nops_submitted = 0;
    while (nops_submitted < nops_total) {

        if (get_tls_IouState__()->iou_pending_ + nops_batch > UringState::iou_maxio_) {
            T::yield();
            continue;
        }

        //printf("spawning batch\n");
        for (size_t i=0; i<nops_batch; i++) {
            T::spawn(t_io, arg, nullptr, true);
            nops_submitted++;
        }
    }

    //trt_dmsg("%s: YIELDING\n", __FUNCTION__);
    T::yield(); // allow all submitters to write
    IOU::stop(); // stop aio
    trt_dmsg("%s: DONE\n", __FUNCTION__);

    return nullptr;
}

void *t_init(void *arg__)
{
    IOU::init();
    struct targ *arg_ = (struct targ *)arg__;
    static struct targ arg = *arg_; // copy (main's stack is going to go away)
    trt_dmsg("%s\n", __FUNCTION__);

    trt_dmsg("spawing iou_poller\n");
    T::spawn(IOU::poller_task, nullptr, nullptr, true, TaskType::TASK);
    trt_dmsg("spawing t_main_detached\n");
    T::spawn(t_main_detached, &arg, nullptr, false, TaskType::POLL);
    trt_dmsg("waiting main\n");
    T::task_wait(); // wait before releasing targ

    trt_dmsg("%s: DONE\n", __FUNCTION__);
    return nullptr;
}

int main(int argc, char *argv[])
{
    struct targ arg;
    char *fname;

    if (argc < 2) {
        char tempname[] = "/tmp/trt-iou-bench-test-XXXXXX";
        fname = tempname;
        int fd = make_tempfile(fname, FILE_SIZE, BUFF_SIZE);

        arg.fd = fd;
        arg.file_size = FILE_SIZE;
        arg.verify = true;
    } else {
        fname = argv[1];
        int fd = open(fname, O_RDONLY | FILE_FLAGS);
        if (fd == -1) {
            perror(fname);
            exit(1);
        }

        size_t fsize = lseek64(fd, 0, SEEK_END);
        if (fsize == (size_t)-1) {
            perror("lseek64");
            exit(1);
        }

        lseek(fd, 0, SEEK_SET); // shouldn't matter really

        arg.fd = fd;
        arg.file_size = fsize;
        arg.verify = false;
    }

    arg.buff_size = BUFF_SIZE;
    printf("f:%s s:%zd verify:%u\n", fname, arg.file_size, arg.verify);

    xtimer_t t; timer_init(&t); timer_start(&t);

    Controller c;
    c.spawn_scheduler(t_init, &arg, TaskType::TASK, 2);
    c.set_exit_all();
    c.wait_for_all();

    timer_pause(&t);
    double s = timer_secs(&t);
    printf("time=%lfs Kops/sec=%lf MiB/sec=%lf\n", s, NOPS_TOTAL/(s*1000), (1.0*NOPS_TOTAL*BUFF_SIZE)/(s*1024*1024));

    return 0;
}
