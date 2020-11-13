/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

    for (ssize_t i=0; i<total_size; i++)
        buff[i] = 'a' + (i % ('z' - 'a' + 1));

    trt_dmsg("%s: issuing pwrite()\n", __PRETTY_FUNCTION__);
    if (total_size != trt::SPDK::pwrite(qp, buff, total_size, 0)) {
        perror("pwrite");
        abort();
    }

    trt_dmsg("%s: issuing preadv()\n", __PRETTY_FUNCTION__);
    if (total_size != trt::SPDK::preadv(qp, iovecs, nvecs, 0)) {
        perror("preadv");
        abort();
    }

    test_buff_and_vecs(buff, vec_buf, nvecs, vec_size);
    printf("PREADV OK\n");

    /**
     * Test preadv: pwrite zeroes + pwritev + pread
     */

    for (ssize_t i=0; i<total_size; i++)
        buff[i] = 0;

    if (total_size != trt::SPDK::pwrite(qp, buff, total_size, 0)) {
        perror("pwrite");
        abort();
    }

    if (total_size != trt::SPDK::pwritev(qp, iovecs, nvecs, 0)) {
        perror("preadv");
        abort();
    }

    if (total_size != trt::SPDK::pread(qp, buff, total_size, 0)) {
        perror("pread");
        abort();
    }

    test_buff_and_vecs(buff, vec_buf, nvecs, vec_size);
    printf("PWRITEV OK\n");

    for (size_t i=0; i<nvecs; i++) {
        free(vec_buf[i]);
    }
    free(buff);

    trt_dmsg("IO task END\n");
    return nullptr;
}


void *
t_init(void *arg__) {

    trt_dmsg("%s: enter\n", __PRETTY_FUNCTION__);
    GlobArg *arg = static_cast<GlobArg *>(arg__);

    trt::SPDK::init(arg->g_spdk);
    std::shared_ptr<SpdkQpair> qp = trt::SPDK::getQpair(arg->g_namepace);

    trt_dmsg("Spawning SPDK poller task\n");
    trt::T::spawn(trt::SPDK::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);

    trt_dmsg("Spawning IO task\n");
    trt::T::spawn(t_io, qp.get());
    trt_dmsg("Waiting IO task\n");
    trt::T::task_wait();

    trt_dmsg("IO task DONE: Stopping SPDK queues\n");
    trt::SPDK::stop();

    trt_dmsg("Notify scheduler to exit\n");
    trt::T::set_exit_all();
    return nullptr;
}

int main(int argc, char *argv[])
{
    GlobArg g("");
    g.init();
    trt::Controller c;

    c.spawn_scheduler(t_init, &g, trt::TaskType::TASK);
    c.wait_for_all();

    return 0;
}
