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

// Trt support for using epoll()

#ifndef TRT_EPOLL_HH_
#define TRT_EPOLL_HH_

#include <unordered_map>
#include <tuple>
#include <sys/socket.h>
#include <sys/epoll.h>

extern "C" {
    #include "trt_util/misc.h"  // spinlock_t
}
#include "trt_util/deque_mt.hh"

// The design we follow is to have one epoll context per scheduler [1].  This
// decision has implications. See for example
// tests/trt_epoll_multi_threaded_server.cc on how we can implement a
// multi-threaded server listening on a single socket.
//
// [1] There are alternative designs where we use one global epoll context
// (maybe with an edge triggered approach). I don't think that they will work as
// well for multiple threads, though.

namespace trt {

enum class EpollSpawnPolicy {
    Local,      // only local
    Distribute, // both local and remote
};


// per-thread epoll state
class EpollState {
    enum class State {UNINITIALIZED, READY, DRAINING, DONE};
    State ep_state_;
    int   ep_fd_; // epoll file descriptor
    int   ep_wait_timeout_;

    // registered fds (one for each type of event, for now: IN, OUT)
    //    ->  null => nothing to notify
    //    -> !null => the async object to notify
    struct FdInfo {
        uint32_t event_mask;
        int      old_flags;

        LocalSingleAsyncObj *ao_in_, *ao_out_;

        FdInfo(uint32_t mask, int fl)
            : event_mask(mask)
            , old_flags(fl)
            , ao_in_(nullptr)
            , ao_out_(nullptr) {}
    };
    std::unordered_map<int, FdInfo> fds_;
    size_t pending_waits_;

    // queue for remotely spawning tasks
    // (this could be in the scheduler, but here is more contained and we only
    // spawn in schedulers that have an epoll spawner)
    //
    // This allows tasks calling accept_ll() to enqueue tasks to other
    // schedulers. The queue is checked by the poller.
    struct EpollSpawn {
        int       reg_fd;     // register fd
        uint32_t  reg_mask;   // register mask
        TaskFn    spawn_fn;
        TaskFnArg spawn_arg;
        /* no parent */
        /* detached */
        /* trt::TaskType::Task */

        EpollSpawn(int fd, uint32_t m, TaskFn fn, TaskFnArg arg)
        : reg_fd(fd)
        , reg_mask(m)
        , spawn_fn(fn)
        , spawn_arg(arg)
        {}

        EpollSpawn() : EpollSpawn(-1, 0, nullptr, nullptr) {}
    };
    deque_mt<EpollSpawn> epoll_spawn_deque_;

public:
    EpollState();

    EpollState(EpollState const &) = delete;
    void operator=(EpollState const &) = delete;

    // register a file descriptor to be handled by the poller
    // (adds it to the epoll fds and sets it as non-blocking)
    void register_fd(int fd, uint32_t event_mask /* EPOLLIN or EPOLLOUT */);
    //
    // deregister a file descriptor from the poller
    //  returns != 0 if there was an error (e.g., fd not found)
    //
    // fcntl mask will return to its previous value (i.e., before register_fd
    // sets O_NONBLOCK)
    int deregister_fd(int fd);

    void init(void);
    void stop(void);

    void *poller_task(void *arg);

    void set_timeout_param(int t) { ep_wait_timeout_ = t; }
    int get_timeout_param() { return ep_wait_timeout_; }
    int get_timeout() {
        return T::io_npending_get() == 0 ? get_timeout_param() : 0;
    }


private:
    enum class OpType {IN, OUT};

    void notify_maybe(int fd, OpType ty);
    void shutdown_all(void);

    // wrapper for socket operations that goes over the trt mehanics (i.e,.
    // creates an async object, a waitset and a future for the operation).
    template<typename F, typename... Args>
    typename std::result_of<F(int, Args...)>::type
    op_wrapper(F &&f, OpType ty, int fd, Args &&... a);

    EpollState *es_get_rr();

public:
    int listen(int sockfd, int backlog);

    int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
    int accept_ll(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

    ssize_t recv(int fd, void *buff, size_t len, int flags);
    ssize_t send(int fd, const void *buff, size_t len, int flags);

    ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
    ssize_t recvmsg(int fd, struct msghdr *msg, int flags);

    // register an fd a spawn a task. Will call setnonblocking()
    // policy:
    //  Local: locally
    //  Distribute: distribute across multiple schedulers (including local)
    void register_and_spawn(int fd, uint32_t m, TaskFn fn, TaskFnArg arg,
                            EpollSpawnPolicy p);
};

// global epoll state
class EpollGlobalState {
    static const size_t    STATES_NR_ = 128;
    EpollState     *states_[STATES_NR_];
    size_t          states_next_;
    spinlock_t      lock_;

public:
    EpollGlobalState() : states_next_(0) {
        // not sure if the default constructor will do it and too lazy to look
        for (size_t i=0; i<STATES_NR_; i++)
            states_[i] = nullptr;
        spinlock_init(&lock_);
    }

    inline void lock()   { spin_lock(&lock_); }
    inline void unlock() { spin_unlock(&lock_); }

    void register_epoll_state(EpollState *st) {
        lock();
        assert(st != nullptr);
        if (states_next_ == STATES_NR_) {
            fprintf(stderr, "More states than max=%zd. Dying ungracefully.", STATES_NR_);
            abort();
        }
        states_[states_next_++] = st;
        unlock();
    }

    void deregister_epoll_state(EpollState *st) {
        lock();
        for (size_t i=0; i < STATES_NR_; i++) {
            if (states_[i] == st) {
                // shift array
                for (;;) {
                    states_[i] = (i < STATES_NR_ - 1) ? states_[i+1] : nullptr;
                    if (states_[i] == nullptr)
                        break;
                    i++;
                }
                // decrease last index
                states_next_--;
                // we are done
                unlock();
                return;
            }
        }
        // state was not found. Let's consider this a bug and die
        unlock();
        fprintf(stderr, "%s:%d: state %p not found", __PRETTY_FUNCTION__, __LINE__, st);
        abort();
    }

    EpollState *es_get_mod(size_t idx) {
        EpollState *ret;
        lock();
        ret = states_[idx % states_next_];
        unlock();
        return ret;
    }
};

// users of epoll
#if !defined(TRT_EPOLL_SELF)
extern thread_local EpollState EpollState__;

// User API
struct Epoll {

    // Initialization:
    // Typically, there are two things that need to be done on every scheduler
    // that wants to use epoll: call init() to initialize thread-local state and
    // spawn a poller task to check the epoll queue

    // initialize scheduler-local state
    static inline void init(void) { EpollState__.init(); }
    // stop scheduler-local epoll
    // TODO: explain
    static inline void stop(void) { EpollState__.stop(); }

    // poller task for checking queues
    static void *poller_task(void *arg) { return EpollState__.poller_task(arg); };

    // set socket to listen (NB: blocks does not go over trt) and register it to
    // the local epoll context.
    static int listen(int fd, int backlog) { return EpollState__.listen(fd, backlog); }

    // accept() and accept_ll():
    //
    // wait for new connections on a listen socket
    //  - if no new connection is available, the callign task will defer
    //    execution until it is woken by the poller.
    //
    // Once a new connection is available, accept() will also register the file
    // descriptor (which sets it to be non-blocking)
    static int accept_ll(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
        return EpollState__.accept_ll(sockfd, addr, addrlen);
    }
    static int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
        return EpollState__.accept(sockfd, addr, addrlen);
    }

    // send / recv operations
    //  if they return EWOULDBLOCK their execution will be defered until
    //  the poller wakes them up.
    static ssize_t recv(int fd, void *buf, size_t len, int flags) {
        return EpollState__.recv(fd, buf, len, flags);
    }
    static ssize_t send(int fd, const void *buf, size_t len, int flags) {
        return EpollState__.send(fd, buf, len, flags);
    }

    // close a file descriptor and deregester it from the epoll loop.
    static int close(int fd) {
        int err = EpollState__.deregister_fd(fd);
        if (err) {
            fprintf(stderr, "%s:%d: deregister_fd returned error\n", __PRETTY_FUNCTION__, __LINE__);
        }
        return ::close(fd);
    }

    // register_and_spawn() is a simple helper for building multi-threaded
    // servers that use only a single listen socket.
    // The idea is that the user calls accept_ll() and then uses this function
    // to:
    //  regsiter the accept fd
    //  spawn a new task to handle it (either local or remotely based on a
    //  policy)
    using SpawnPolicy = EpollSpawnPolicy;
    static void register_and_spawn(int fd, uint32_t m, TaskFn fn, TaskFnArg arg,
                                   EpollSpawnPolicy p) {
        return EpollState__.register_and_spawn(fd, m, fn, arg, p);
    }

    static ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
        return EpollState__.sendmsg(sockfd, msg, flags);
    }

    static ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
        return EpollState__.recvmsg(sockfd, msg, flags);
    }

    // default: 0 -- se EpollState__.get_timeout() on how is used.
    static void set_wait_timeout(int x) { EpollState__.set_timeout_param(x); }
    static int get_wait_timeout() { return EpollState__.get_timeout_param(); }

    // for debugging
    static void *epoll_handle() { return static_cast<void *>(&EpollState__); }

};
#endif

}

#endif /* TRT_EPOLL_HH_ */
