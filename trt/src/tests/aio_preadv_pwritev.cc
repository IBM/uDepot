/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

#include <inttypes.h>
#include <string.h>
#include <sys/uio.h>

#include "trt_util/aio.hh"

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
    struct io_event event;

    AioState aio;
    aio.init();

    for (size_t i=0; i<nvecs; i++) {
        iovecs[i].iov_base = vec_buf[i] = static_cast<char *>(malloc(vec_size));
        iovecs[i].iov_len  = vec_size;
        if (!vec_buf[i]) {
            perror("malloc");
            exit(1);
        }
    }

    char fname[] = "/tmp/aio_preadv.XXXXXX";
    fd = make_tempfile(fname, file_size);

    for (size_t i=0; i<file_size; i++)
        buff[i] = 'a' + (i % ('z' - 'a' + 1));

    if (file_size != pwrite(fd, buff, file_size, 0)) {
        perror("pwrite");
        abort();
    }

    struct iocb iocb_read;
    memset(&iocb_read, 0, sizeof(iocb_read));

    iocb_read.aio_fildes     = fd;
    iocb_read.aio_lio_opcode = IOCB_CMD_PREADV;
    iocb_read.aio_reqprio    = 0;
    iocb_read.aio_buf        = (uintptr_t)iovecs;
    iocb_read.aio_nbytes     = nvecs;
    iocb_read.aio_offset     = 0;
    iocb_read.aio_data       = 0xbeef;

    aio.submit(&iocb_read);
    ret = aio.getevents(1, 1, &event, NULL);
    if (ret != 1) {
        perror("getevents");
        abort();
    }

    test_buff_and_vecs(buff, vec_buf, nvecs, vec_size);
    printf("PREADV OK\n");

    for (size_t i=0; i<file_size; i++)
        buff[i] = 0;

    if (file_size != pwrite(fd, buff, file_size, 0)) {
        perror("pwrite");
        abort();
    }

    struct iocb iocb_write;
    memset(&iocb_read, 0, sizeof(iocb_write));
    iocb_write.aio_fildes     = fd;
    iocb_write.aio_lio_opcode = IOCB_CMD_PWRITEV;
    iocb_write.aio_reqprio    = 0;
    iocb_write.aio_buf        = (uintptr_t)iovecs;
    iocb_write.aio_nbytes     = nvecs;
    iocb_write.aio_offset     = 0;
    iocb_write.aio_data       = 0xbeef;

    aio.submit(&iocb_write);
    ret = aio.getevents(1, 1, &event, NULL);
    if (ret != 1) {
        perror("getevents");
        abort();
    }

    if (file_size != pread(fd, buff, file_size, 0)) {
        perror("pwrite");
        abort();
    }

    test_buff_and_vecs(buff, vec_buf, nvecs, vec_size);
    printf("PWRITEV OK\n");

    for (size_t i=0; i<nvecs; i++) {
        free(vec_buf[i]);
    }

    aio.stop();

    return 0;
}
