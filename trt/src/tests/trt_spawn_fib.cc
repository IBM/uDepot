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
#include "trt/uapi/trt.hh"

using namespace trt;

void *fib(void *arg) {
    uintptr_t x = (uintptr_t)arg;
    trt::RetT ret;

    if (x == 1 || x == 2) return (void *)1;

    T::spawn(fib, (void *)(x - 1));
    T::spawn(fib, (void *)(x - 2));

    ret  = std::get<0>(T::task_wait());
    ret += std::get<0>(T::task_wait());

    return (void *)ret;
}

void *start(void *arg) {
    std::tuple<trt::RetT, void *> ret;

    printf("%s: %lu\n", __FUNCTION__, (uintptr_t)arg);
    T::spawn(fib, arg, (void *)0xf11f11);
    ret = T::task_wait();
    assert(std::get<1>(ret) == (void *)0xf11f11);
    printf("%s: ret  : %llu\n", __FUNCTION__, (unsigned long long)std::get<0>(ret));

    return nullptr;
}

int main(int argc, char *argv[]) {
    Controller c;
    c.spawn_scheduler(start, (void *)10, TaskType::TASK);

    return 0;
}
