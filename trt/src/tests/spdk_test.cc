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

// A test for "test_util/spdk.hh"
//
// write a bunch of data into a device
// read them, and make sure they have the same checksum

#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "trt_util/spdk.hh"


#define TOTAL_BYTES (1024*4096)

struct XorCsum {
    unsigned long csum;
    uint64_t len;

    XorCsum() : csum(0), len(0) {}

    void add(char *buff, size_t buff_size) {
        for (size_t i = 0; i < buff_size; i++) {
            csum ^= buff[i];
        }

        len += buff_size;
    }

    bool operator!=(XorCsum &c) { return csum != c.csum || len != c.len; }
};

struct SpdkOp {
    SpdkQpair *qp_;
    const size_t dev_sectors_;
    const size_t sector_size_;

    SpdkOp(SpdkQpair *qp)
        : qp_(qp),
          dev_sectors_(qp->sqp_namespace->get_nsectors()),
          sector_size_(qp->sqp_namespace->get_sector_size()) {}

    virtual bool finished(void) = 0;
    virtual void submit_op(void) = 0;
    virtual void op_done(void)   = 0;

    static void callback_(void *arg, const struct spdk_nvme_cpl *);

    virtual void run_op() {
        submit_op();
        while (!finished()) {
            qp_->execute_completions();
        }
    }
};


void SpdkOp::callback_(void *arg, const struct spdk_nvme_cpl *)
{
    SpdkOp *op = static_cast<SpdkOp *>(arg);
    op->qp_->npending_dec(1);
    op->op_done();
    if (!op->finished())
        op->submit_op();
}


struct SpdkWrite : SpdkOp {
    size_t  total_bytes_, bytes_written_;
    SpdkPtr buff_;
    size_t  buff_size_;
    size_t  last_write_size_;
    size_t  nwrites_;
    XorCsum csum_;

    SpdkWrite(SpdkQpair *qp, size_t nbytes)
        : SpdkOp(qp),
          total_bytes_(nbytes),
          bytes_written_(0),
          buff_size_(4096),
          last_write_size_(0),
          nwrites_(0) {
        size_t buff_nlbas = buff_size_ / qp->get_sector_size();
        assert(buff_size_ % qp->get_sector_size() == 0);
        buff_ = std::move(qp->alloc_buffer(buff_nlbas));
        if (!buff_.ptr_m) {
            fprintf(stderr, "malloc failed");
            exit(1);
        }
        srand(time(NULL));
    }

    ~SpdkWrite() {
        if (buff_.ptr_m)
            qp_->free_buffer(std::move(buff_));
    }

    bool finished(void) override {
        return total_bytes_ <= bytes_written_;
    }

    void op_done() override {
        bytes_written_ += last_write_size_;
    }

    void submit_op() override {
        for (unsigned i=0; i<buff_size_; i++) {
            char *p = (char *)buff_.ptr_m;
            p[i] = rand();
        }

        uint64_t lba = bytes_written_ / sector_size_;
        uint64_t bytes_remaining = (lba - dev_sectors_) * sector_size_;
        uint64_t op_bytes = std::min(buff_size_, bytes_remaining);
        assert(op_bytes % sector_size_ == 0);
        last_write_size_ = op_bytes;
        csum_.add((char *)buff_.ptr_m, op_bytes);
        qp_->submit_write(buff_, lba, op_bytes / sector_size_, callback_, this);
    }
};

struct SpdkCsum : SpdkOp {
    size_t bytes_read_;
    XorCsum csum_;

    SpdkPtr spdk_buff;
    size_t  spdk_buff_size;

    size_t last_read_size;

    SpdkCsum(SpdkQpair *qp)
        : SpdkOp(qp),
          bytes_read_(0),
          spdk_buff_size(4096),
          last_read_size(0) {
        size_t buff_nlbas = spdk_buff_size / qp->get_sector_size();
        assert(spdk_buff_size % qp->get_sector_size() == 0);
        spdk_buff = std::move(qp->alloc_buffer(buff_nlbas));
        if (spdk_buff.ptr_m == nullptr) {
            fprintf(stderr, "rte_malloc failed\n");
            exit(1);
        }
        assert(spdk_buff_size % sector_size_ == 0);
    }

    ~SpdkCsum(void) {
        if (spdk_buff.ptr_m)
            qp_->free_buffer(std::move(spdk_buff));
    }

    bool finished(void) override {
        //return bytes_read_ == sector_size_ * dev_sectors_;
        return bytes_read_ == TOTAL_BYTES;
    }

    void op_done(void) override {
        csum_.add((char *)spdk_buff.ptr_m, last_read_size);
        bytes_read_ += last_read_size;
    }

    void submit_op() override {
        uint64_t lba = bytes_read_ / sector_size_;
        uint64_t bytes_remaining = (lba - dev_sectors_) * sector_size_;
        uint64_t op_bytes = std::min(spdk_buff_size, bytes_remaining);
        assert(op_bytes % sector_size_ == 0);
        //printf("submitting (read:%lu)\n", bytes_read_);
        last_read_size = op_bytes;
        qp_->submit_read(spdk_buff, lba, op_bytes / sector_size_, callback_, this);
    }
};

XorCsum spdk_xor_checksum() {
    SpdkGlobalState sg;
    sg.init();

    SpdkState spdk(sg);

    auto qp = spdk.getQpair(std::string());
    if (qp == nullptr) {
        fprintf(stderr, "did not find queue pair\n");
        abort();
    }

    printf("dev_sectors: %zd\n", qp->sqp_namespace->get_nsectors());
    printf("sector_size: %u\n", qp->sqp_namespace->get_sector_size());

    // execute write operations
    SpdkWrite wr(qp.get(), TOTAL_BYTES);
    wr.run_op();

    // execute read operations
    SpdkCsum rd(qp.get());
    rd.run_op();

    if (wr.csum_ != rd.csum_) {
        fprintf(stderr, "%s: checksums differ!\n", __PRETTY_FUNCTION__);
        fprintf(stderr, "Write: len=%lu csum=%lu\n", wr.csum_.len, wr.csum_.csum);
        fprintf(stderr, "Read : len=%lu csum=%lu\n", rd.csum_.len, rd.csum_.csum);
    }

    spdk.stop();
    return rd.csum_;
}

XorCsum file_xor_checksum(const char *device) {
    uint64_t bytes_read;
    char buff[4096];
    XorCsum csum;

    int fd = open(device, O_RDONLY /* | O_DIRECT */);
    if (fd == -1) {
        perror(device);
        exit(1);
    }

    bytes_read = 0;
    while (bytes_read != TOTAL_BYTES) {
        int r = read(fd, buff, sizeof(buff));

        if (r == 0) {
            break;
        } else if (r < 0) {
            perror("read");
            abort();
        }


        csum.add(buff, r);
        bytes_read += r;
    }

    return csum;
}

void usage(FILE *f) {
    fprintf(f, "-f filename: filename to use\n");
    //fprintf(f, "-l length of file to use\n");
    fprintf(f, "-s Use spdk\n");
}

int main(int argc, char *argv[]) {
    int opt;
    XorCsum x;
    char *filename = (char *)(uintptr_t)-1;
    //size_t len = TOTAL_BYTES;

    while ((opt = getopt(argc, argv, "f:l:s")) != -1) {
        switch (opt) {
            case 'f':
            filename = optarg;
            break;

            /*
            case 'l':
            len = atol(optarg);
            break;
            */

            case 's':
            filename = NULL;
            break;

            default:
            usage(stderr);
            exit(1);
        }
    }


    if (filename == (char *)(uintptr_t)-1) {
        usage(stdout);
        exit(0);
    } else if (filename) {
        x = file_xor_checksum(filename);
    } else {
        x = spdk_xor_checksum();
    }

    printf("csum:%lu len:%lu\nTerminating normally.\n", x.csum, x.len);
    return 0;
}
