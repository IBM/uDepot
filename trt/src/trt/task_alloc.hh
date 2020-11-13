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

#ifndef TRT_TASK_ALLOC_H__
#define TRT_TASK_ALLOC_H__

#include <typeinfo>
#include <typeindex>
#include <unordered_map>

#include "trt/task_base.hh"
#include "trt/task.hh"

namespace trt {

// Task allocation queues
// Expected to be per-thread, hence no synchronization used.
// Preety simple for now (one malloc per object, no freeing).
//
// The intitial implementation was using a hash table with std::type_index keys.
// However, the overhead had an impact on trt_ubench, so I opted for a single
// queue.  Most of the Task size comes from TaskMem, anyway, so we ever
// implement multiple task types, the size differences should be small.

#include "trt_util/max_size.hh"

// as template arguments, provide a list of tasks that this queue is going to be
// used for
class TaskAllocationQueue {
    static const size_t MAX_SIZE = max_size<Task>();
    TaskBase::List freeq_;

public:
    // This function, essentially, allows to allocate a task without a trt
    // context. The use-case is enqueuing remote tasks to a scheduler without a
    // trt context.
    //
    // Because we are using malloc for each invividual Task structure, tasks can
    // be allocated and freed in different queues.
    template<typename T, typename... Args>
    static T *task_alloc(Args &&...args) {
            void *p;
            p = std::malloc(MAX_SIZE);
            if (p == nullptr)
                return nullptr;
            T *t = new(p) T(std::forward<Args>(args)...);
            return t;
    }

    static void task_free(TaskBase *t) {
        std::free(t);
    }

    // Allocate a new task. Arguments will be passed to the task's constructor
    template<typename T, typename... Args>
    T *alloc(Args &&...args) {
        static_assert(sizeof(T) <= MAX_SIZE, "");
        void *p;
        if (freeq_.empty()) {
            return task_alloc<T, Args...>(std::forward<Args>(args)...);
        }

        p = &freeq_.front();
        freeq_.pop_front();
        T *t = new(p) T(std::forward<Args>(args)...);
        return t;
    }

    // free task (will call the destructor)
    void free(TaskBase *t) {
        t->~TaskBase();
        freeq_.push_front(*t);
    }

    ~TaskAllocationQueue() {
        while (!freeq_.empty()) {
            TaskBase *ptr = &freeq_.front();
            freeq_.pop_front();
            task_free(ptr);
        }
    }
};

} // end namespace trt


#endif /* ifndef TRT_TASK_ALLOC_H__ */
