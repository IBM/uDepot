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

// Simple multithreaded dequeue.
// Meant for producers / consumers.

#ifndef DEQUE_MT_HH_
#define DEQUE_MT_HH_

#include <deque>
#include <atomic>

extern "C" {
    #include "trt_util/misc.h"  // spinlock_t
}

template<typename T>
class deque_mt {
private:
    std::deque<T>       deque_;
    std::atomic<size_t> deque_nr_;
    spinlock_t          lock_;

public:
    deque_mt() : deque_nr_(0) {
        spinlock_init(&lock_);
    }

    deque_mt(deque_mt const&) = delete;
    void operator=(deque_mt const &) = delete;
    virtual ~deque_mt() {}

    template<typename... Args>
    inline void emplace_back(Args &&...args) {
        spin_lock(&lock_);
        deque_.emplace_back(std::forward<Args>(args)...);
        spin_unlock(&lock_);
        deque_nr_++;
    }

    // returns:
    //  false:           if deque was empty
    //  true, sets item: if an element was found
    //
    // NB: can i has std::optional now?
    inline bool pop_front(T& item) {
        bool ret = false;
        if (deque_nr_.load(std::memory_order_relaxed) > 0) {
            spin_lock(&lock_);
            if (deque_.size() > 0) {
                ret = true;
                item = deque_.front();
                deque_.pop_front();
                deque_nr_--;
            }
            spin_unlock(&lock_);
        }
        return ret;
    }
};


#endif /* ifndef DEQUE_MT_HH_ */
