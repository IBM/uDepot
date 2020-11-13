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

#ifndef TRT_SYNC_ABSTRACT_H__
#define TRT_SYNC_ABSTRACT_H__

// Some thoughts on synchronization
//
// When scheduling, there is a tradeoff between synchronization across different
// cores and load-balancing.
//
// Cilk-5, for example, uses work stealing and the THE protocol. If a core does
// not have work, it will try to steal from other schedulers. The task queue
// uses an algorithm that does not require a lock in the fast path, i.e.,
// pushing/popping tasks out of the local queue. It does require locking when
// stealing though (to protect against other potential stealers). Cilk's
// implementation is based on the assumption that there is ample parallel
// slackness so stealing is infrequent.
//
// Accept-affinity, arrakis, and other works have shown that you need to
// minimize synchronization when dealing with network servers. If possible, the
// path of request processing should never change cores.  Unfortunately, this is
// not possible to do for a single (standard) TCP socket because of accept().
//
// The initial thought for trt was to use something closer to Cilk's work
// stealing, but since we target network servers, parallel slackness is not
// really a property that we can assume. I think it actually makes more sense to
// target something like the second approach.
//
// Two designs come to mind as a first approach
// (we can probably do better with something like DPDK):
//  1. single queue for all schedulers. Scheduler that has accept puts tasks
//  into this queue.
//  2. one queue per scheduler that does not do accept, accept task places new
//  tasks into this queue in a round-robin (or whatever) fashion.
//
// (2) makes more sense to me.
//
// The discussion above is concerned mostly with task balancing. However, there
// are other tradeoffs, such as synchronization for tasks that wait on one
// another (e.g., wait_() and friends). Initially, I assumed that a task on one
// core might have to wait for a  task on another core. We might, however, want
// to make the argument that all processing of a single network request happens
// on a single core so there is no need to synchronize for these things. Since,
// the atomic operations used in these function start to appear on profiling,
// this might not be a bad idea.
//
// Is there a compromise where we could support both (i.e., waiting from
// different threads and waiting on the same thread)? There are other (special)
// cases where we can have more efficient implementations: e.g., waitsets with
// one waiter.

// Can we build a common interface for:
//  - AsyncObj
//  - Future
//  - Waitset
//  - Task
//  that the scheduler can use?

// The primitives above are used inside tasks, so we will need an abstract task
// as well.

#include <cstdio>

#include <boost/intrusive/list.hpp>
namespace bi = boost::intrusive;

#include "trt/common.hh"


namespace trt {

//
// Base types for synchronization
//

class TaskBase;
class AsyncObjBase;
class FutureBase;
class WaitsetBase;

// TODO: explain


class WaitsetBase {
public:

    // called when one of the async objects pointed by the futures of this wait
    // set becomes available.
    //
    // NB: called in Scheduler::notify_
    virtual TaskBase *set_ready() = 0;

    // used by the user. it will either return directly or try to wait in the
    // scheduler
    virtual FutureBase *wait() = 0;

    // used by scheduler to make the transition to the waiting state
    // return value:
    //  true:  transition succesfull, do nothing
    //  false: transition failed, please reschedule task
    //
    // NB: used in scheduler when handing Cmd::WAIT
    virtual bool try_set_state_to_waiting(void) = 0;
};

class FutureBase {
protected:
    // A future is kept into two lists:
    //  - The async object keeps the futures it needs to wakeup
    //  - The waitlist keeps the futures it waits on
    bi::list_member_hook<> f_lnode_ao_;
    bi::list_member_hook<> f_lnode_ws_;

    // Atomically checks if future is ready and it schedules it for notification
    // if it is not
    //
    // Used from within waitsets.
    virtual bool is_ready_or_register() = 0;

    // notify the future it's wait has been completed (i.e., no longer a member
    // of a waitset).
    //
    // Used from within waitsets.
    virtual void wait_completed() = 0;

public:
    // Is the value of this Future ready?
    virtual bool is_ready() = 0;

    // set the future
    virtual TaskBase *set_ready() = 0;

    virtual RetT get_val()  = 0; // should be called only if is_ready() == true
    virtual void *get_ctx() = 0; // get user context
    virtual void drop_ref() = 0; // drop reference to asynchronous object

    // list type for AsyncObj
    using AoList =
        bi::list<FutureBase,
                 bi::member_hook<FutureBase,
                                 bi::list_member_hook<>,
                                 &FutureBase::f_lnode_ao_>>;

    // list type for Waitset
    using WsList =
        bi::list<FutureBase,
                 bi::member_hook<FutureBase,
                                 bi::list_member_hook<>,
                                 &FutureBase::f_lnode_ws_>>;

    virtual ~FutureBase() {
        if (f_lnode_ao_.is_linked())
            fprintf(stderr, "%s: %p ->f_lnode_ao_ is still linked\n", __PRETTY_FUNCTION__, this);
        if (f_lnode_ws_.is_linked())
            fprintf(stderr, "%s: %p ->f_lnode_ws_ is still linked\n", __PRETTY_FUNCTION__, this);
    }
};

class AsyncObjBase {
public:
    // move the state to ready, and return the list of futures that need to be
    // set to ready (and notified).
    virtual FutureBase::AoList set_ready(RetT val) = 0;
};


} // end trt namespace

#endif /* ifndef TRT_SYNC_ABSTRACT_H__ */
