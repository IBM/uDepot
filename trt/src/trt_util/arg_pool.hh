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

// This is an attempt to generalize a common pattern in trt benchmarking
//
// In benchmarks, we typically have a number of tasks in-flight and we maintain
// this number in a using a closed control loop, i.e., we start another task
// when one finishes. Typically, these tasks have arguments so we need a way
// allocate these arguments when we start another task. Obviously, tasks might
// be completed out of order.


#ifndef ARGPOOL_HH__
#define ARGPOOL_HH__

#include <deque>
#include <functional>

template<typename T>
struct ArgPool {
    size_t                   ap_nargs_;
    std::deque<T *>          ap_deque_;
    T                       *ap_array_;
    using RelFn = std::function<void(T *)>;
    RelFn                    ap_array_release_;

    // NB: by default, ts is deallocated with "delete[] ts"
    ArgPool(size_t nargs, T *ts, std::function<void(T *)> ts_release)
        : ap_nargs_(nargs),
          ap_deque_(nargs),
          ap_array_(ts),
          ap_array_release_(ts_release) {
        ap_deque_.clear();
        for (size_t i=0; i<nargs; i++) {
            ap_deque_.push_back(ap_array_ + i);
        }
    }

    ArgPool(size_t nargs) : ArgPool(nargs, new T[nargs], RelFn()) {}

    ~ArgPool() {
        // NB: default (empty) std::function evaluations to false:
        if (ap_array_release_)
            ap_array_release_(ap_array_);
        else
            delete[] ap_array_;
    }

    size_t free_nr(void) {
        return ap_deque_.size();
    }

    void put_arg(T *arg) {
        //printf("%s: arg:%p deque size: %zd\n", __PRETTY_FUNCTION__, arg, ap_deque_.size());
        ap_deque_.push_back(arg);
    }

    T *get_arg(void) {
        //printf("%s: deque size: %zd\n", __PRETTY_FUNCTION__, ap_deque_.size());
        if (ap_deque_.size() == 0)
            return nullptr;
        T *ret = ap_deque_.front();
        ap_deque_.pop_front();
        return ret;
    }
};

#endif /* ARGPOOL_HH__ */
