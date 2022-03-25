/*
 *  Copyright (c) 2020,2022 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *           Nikolas Ioannou (nio@zurich.ibm.com, nicioan@gmail.com)
 */

#include <inttypes.h>
#include <string.h>
#include <sys/uio.h>

#include "trt_util/iouring.hh"

// IOCB_PRWITEV/IOCB_PREADV is not clearly documented, so after reading fs/aio.c
// this is how I think it should work

static int
make_tempfile(char *tmpname, size_t file_size) {
    int fd;

    printf("Creating random file\n");
    fd = mkostemp(tmpname, 0);
    if (fd < 0) {
        perror("mkostemp");
        exit(1);
    }

    int ret = ftruncate(fd, file_size);
    if (ret < 0) {
        perror("ftruncate");
        exit(1);
    }

    return fd;
}

void
test_buff_and_vecs(char *b_, char **vecs, size_t nvecs, size_t vec_size)
{
    char *b = b_;
    for (size_t v=0; v < nvecs; v++) {
        for (size_t i=0; i < vec_size; i++) {
            if (vecs[v][i] != *b) {
                fprintf(stderr, "error %c =/= %c (%zd)\n", vecs[v][i], *b, b - b_);
                abort();
            }
            b++;
        }
    }
}

int main(int argc, char *argv[])
{
    const size_t file_size_ = 1024;
    const size_t nvecs      = 4;
    const size_t vec_size   = file_size_ / nvecs;
    const size_t file_size  = nvecs * vec_size;

    int fd;
    struct iovec iovecs[nvecs];
    char *vec_buf[nvecs];
    char buff[file_size];
    ssize_t ret;

    UringState uring_state;
    uring_state.init();
    assert(uring_state.is_initialized());
    for (size_t i=0; i<nvecs; i++) {
        iovecs[i].iov_base = vec_buf[i] = static_cast<char *>(malloc(vec_size));
        iovecs[i].iov_len  = vec_size;
        if (!vec_buf[i]) {
            perror("malloc");
            exit(1);
        }
    }

    char fname[] = "/tmp/iou_preadv.XXXXXX";
    fd = make_tempfile(fname, file_size);

    for (size_t i=0; i<file_size; i++)
        buff[i] = 'a' + (i % ('z' - 'a' + 1));

    if (file_size != pwrite(fd, buff, file_size, 0)) {
      fprintf(stderr,"pwrite errno=%s ret=%ld", strerror(errno), file_size);
        abort();
    }

    struct io_uring_sqe *sqe = uring_state.get_sqe();
    assert(sqe != nullptr);
    io_uring_prep_readv(sqe, fd, iovecs, nvecs, 0);
    io_uring_sqe_set_data(sqe, iovecs);

    ret = uring_state.submit(sqe);
    if (ret != 0) {
        perror("getevents");
        abort();
    }
    uring_state.flush_ioq_();

    struct io_uring_cqe *cqes[1];

    ret = uring_state.wait_cqe_nr(1, cqes);
    assert(ret == 0);
    struct io_uring_cqe *cqe = cqes[0];

    uring_state.cqe_seen(cqe);
    assert(cqe->res == file_size);

    test_buff_and_vecs(buff, vec_buf, nvecs, vec_size);
    printf("PREADV OK\n");

    for (size_t i=0; i<file_size; i++)
        buff[i] = 0;

    if (file_size != pwrite(fd, buff, file_size, 0)) {
      fprintf(stderr,"pwrite errno=%s", strerror(errno));
        abort();
    }

    sqe = uring_state.get_sqe();
    assert(sqe != nullptr);
    io_uring_prep_writev(sqe, fd, iovecs, nvecs, 0);
    io_uring_sqe_set_data(sqe, iovecs);

    ret = uring_state.submit(sqe);
    if (ret != 0) {
        perror("getevents");
        abort();
    }
    uring_state.flush_ioq_();

    ret = uring_state.wait_cqe_nr(1, cqes);
    assert(ret == 0);
    cqe = cqes[0];

    uring_state.cqe_seen(cqe);
    assert(cqe->res == file_size);


    if (file_size != pread(fd, buff, file_size, 0)) {
        perror("pread");
        abort();
    }

    test_buff_and_vecs(buff, vec_buf, nvecs, vec_size);
    printf("PWRITEV OK\n");

    for (size_t i=0; i<nvecs; i++) {
        free(vec_buf[i]);
    }

    uring_state.stop();

    return 0;
}
