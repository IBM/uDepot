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

#ifndef TRT_TASK_MEM_HH_
#define TRT_TASK_MEM_HH_

#include <stdio.h> // fprintf

namespace trt {

// This is a simple implementation of a task-local memory structure. Objects in
// this heap go away when the object goes away. We merge it to the stack and
// have them grow in different directions (what a novel idea!).
template<size_t HeapN, size_t StackN>
class TaskMem {
    static const size_t ull_nr_heap  = (1 +  (HeapN-1)) / sizeof(unsigned long long);
    static const size_t ull_nr_stack = (1 + (StackN-1)) / sizeof(unsigned long long);
    static const size_t ull_nr = ull_nr_heap + ull_nr_stack;

    // we use unsigned long long to get better alligment
    unsigned long long __attribute__((aligned(128))) mem__[ull_nr];
    size_t heap_idx_;
    char *heap__;
    char *stack__;

public:
 TaskMem()
     : heap_idx_(0),
       heap__((char *)&mem__[0]),
       stack__((char *)&mem__[ull_nr_heap]) {}

    void *get_stack_bottom() { return static_cast<void *>(stack__); }
    size_t get_stack_size()  { return StackN; }

    // not very sophisticated, but here you go
    void *alloc(size_t size) {
        char *p;
        if (heap_idx_ + size > HeapN)
            return nullptr;
        p = heap__ + heap_idx_;
        heap_idx_ += size;
        return static_cast<void *>(p);
    }

    void free(size_t size) {
        fprintf(stderr, "%s: NYI!", __PRETTY_FUNCTION__);
        abort();
    }
};

} // end namespace trt

#endif /* ifndef TRT_TASK_MEM_HH_ */
