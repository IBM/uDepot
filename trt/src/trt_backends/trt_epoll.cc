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


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <assert.h>

#include "trt/uapi/trt.hh"

// Only trt_epoll.cc should define TRT_EPOLL_SELF
#define TRT_EPOLL_SELF
#include "trt_backends/trt_epoll.hh"

#define MAX_NEVENTS 128

namespace trt {

thread_local EpollState EpollState__;
EpollGlobalState        EpollGlobalState__;

EpollState::EpollState(void)
    : ep_state_(State::UNINITIALIZED)
    , ep_fd_(-1)
    , ep_wait_timeout_(0) { }

void EpollState::init(void) {
    if (ep_state_ != State::UNINITIALIZED) {
        fprintf(stderr, "%s: Invalid state\n", __PRETTY_FUNCTION__);
        abort();
    }

    ep_fd_ = epoll_create(MAX_NEVENTS);
    if (ep_fd_ == -1) {
        perror("epoll_create");
        exit(1);
    }

    // NB: we currently never deregister from the global state...
    EpollGlobalState__.register_epoll_state(this);
    ep_state_ = State::READY;
}

void EpollState::stop(void) {
    if (ep_state_ != State::READY) {
        fprintf(stderr, "%s: Invalid state\n", __PRETTY_FUNCTION__);
        abort();
    }

    // remove ourselves from the global list
    EpollGlobalState__.deregister_epoll_state(this);

    // The idea in other backends (e.g., SPDK) is that when stop() is called, we
    // enter draining mode.  That is, we stop adding new requests, we wait for
    // all requests to complete, and when we are done we finalize the shutdown
    // (DRAINING -> DONE). With epoll, we can wait on some events (e.g.,
    // replies) but we need to interrupt others (e.g., client connections).
    // Currently, there is no way to distinguish between them, so we interupt
    // all file descriptors
    ep_state_ = State::DRAINING;
    shutdown_all();

    // close epoll fd
    close(ep_fd_);
    ep_fd_ = -1;

    ep_state_ = State::DONE;
}

static int
setnonblocking(int fd, int &flags) {
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int
setnonblocking(int fd) {
    int unused;
    return setnonblocking(fd, unused);
}

// returns != 0 if the file descriptor was not found
int EpollState::deregister_fd(int fd)
{
    auto iter = fds_.find(fd);
    if (iter == fds_.end()) {
        // if stop() was called, we already deregistered everything, but we
        // still expect user to call close()
        return ep_state_ == State::DONE ? 0 : 1;
    }

    if (iter->second.ao_in_) {
        // TODO: register a falure with the async object
        fprintf(stderr, "%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
        abort();
    }

    if (iter->second.ao_out_) {
        // TODO: register a falure with the async object
        fprintf(stderr, "%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
        abort();
    }

    fds_.erase(iter);

    int ret = epoll_ctl(ep_fd_, EPOLL_CTL_DEL, fd, NULL);
    if (ret == -1) {
        trt_dmsg("epoll_ctl: EPOLL_CTL_DEL returned %d (%s)\n", errno, strerror(errno));
    }

    return 0;
}

void EpollState::register_fd(int fd, uint32_t event_mask) {
    int old_flags,ret;
    struct epoll_event ev;

    setnonblocking(fd, old_flags);
    assert(fds_.find(fd) == fds_.end());
    fds_.emplace(std::piecewise_construct,
                 std::make_tuple(fd),
                 std::make_tuple(event_mask, old_flags));
    ev.events = event_mask;
    ev.data.fd = fd;
    ret = epoll_ctl(ep_fd_, EPOLL_CTL_ADD, fd, &ev);
    if (ret < 0) {
        // TODO proper error handling
        perror("epoll_ctl");
        abort();
    }
}

int
EpollState::listen(int sockfd, int backlog) {
    int ret;

    ret = ::listen(sockfd, backlog);
    if (ret < 0)
        return ret;

    // a listen socket is only for input
    setnonblocking(sockfd);
    register_fd(sockfd, EPOLLIN);

    return 0;
}

void
EpollState::shutdown_all(void)
{
    auto notify = [] (LocalSingleAsyncObj *lsao, int ret) {
        bool x = T::local_single_notify_add(lsao, ret);
        if (!x) {
            T::local_single_notify_submit();
            T::local_single_notify_init();
            // XXX: At this point we switch to the scheduler, which means that
            // we might get rescheduled. Need to ensure that nothing will break
            // if this happens.
            x = T::local_single_notify_add(lsao, ret);
            assert(x);
        }
    };

    T::local_single_notify_init();
    for (auto x = fds_.begin(); x != fds_.end(); ) {
        int fd       = x->first;
        FdInfo *fd_i = &x->second;

        if (fd_i->ao_in_)
            notify(fd_i->ao_in_, ESHUTDOWN);

        if (fd_i->ao_out_)
            notify(fd_i->ao_out_, ESHUTDOWN);

        int ret = epoll_ctl(ep_fd_, EPOLL_CTL_DEL, fd, NULL);
        if (ret == -1) {
            trt_dmsg("epoll_ctl: EPOLL_CTL_DEL returned %d (%s)\n", errno, strerror(errno));
        }

        x = fds_.erase(x);
    }
    T::local_single_notify_submit();
}

void
EpollState::notify_maybe(int fd, OpType ty)
{
    auto entry = fds_.find(fd);
    assert(entry != fds_.end());
    assert(entry->first == fd);
    LocalSingleAsyncObj **lsao_pptr;
    switch (ty) {
        case OpType::OUT:
        lsao_pptr = &entry->second.ao_out_;
        break;

        case OpType::IN:
        lsao_pptr = &entry->second.ao_in_;
        break;

        default: /* convince GCC that ao_pptr has a value */
        abort();

    }

    LocalSingleAsyncObj *lsao = *lsao_pptr;
    if (lsao == nullptr)
        return;
    *lsao_pptr = nullptr;

    bool ret = T::local_single_notify_add(lsao, 0);
    //trt_dmsg("notify add returned: %u\n", ret);
    if (!ret) { // flush notifications and retry
        T::local_single_notify_submit();
        T::local_single_notify_init();
        ret = T::local_single_notify_add(lsao, 0);
        assert(ret);
    }
}

void *
EpollState::poller_task(void *unused) {
    struct epoll_event *ep_events;
    size_t max_nevents = MAX_NEVENTS;

    trt_dmsg("Starting %s\n", __PRETTY_FUNCTION__);
    assert(ep_state_ == State::READY);
    size_t nbytes = sizeof(epoll_event)*max_nevents;
    ep_events = static_cast<struct epoll_event *>(malloc(nbytes));
    if (!ep_events) {
        perror("malloc");
        exit(1);
    }

    for (;;) {
        int nevents = 0;

        if (ep_fd_ == -1) {
            trt_msg("TRT Epoll poller exiting\n");
            break;
        }


        if (pending_waits_ > 0) {
            int timeout = get_timeout();
            nevents = epoll_wait(ep_fd_, ep_events, max_nevents, timeout);
            if (nevents < 0) {
                perror("epoll_wait");
                exit(1);
            }
        }
        //trt_dmsg("nevents=%d\n", nevents);

        // check to see if any fd's were queued for registration
        // just one for now, todo: pop_many()
        EpollSpawn s;
        if (epoll_spawn_deque_.pop_front(s)) {
            // assume that fd is already set to be non-blocking
            register_fd(s.reg_fd, s.reg_mask);
            T::spawn(s.spawn_fn, s.spawn_arg, nullptr, true, trt::TaskType::TASK);
        }

        if (nevents == 0) {
            T::yield();
            continue;
        }

        // perform notifcations in batches
        T::local_single_notify_init();
        for (int i=0; i < nevents; i++) {
            struct epoll_event *ev = &ep_events[i];
            int fd = ev->data.fd;
            if (ev->events & EPOLLIN) {
                //trt_dmsg("Notify IN\n");
                notify_maybe(fd, OpType::IN);
            }
            if (ev->events & EPOLLOUT) {
                //trt_dmsg("Notify OUT\n");
                notify_maybe(fd, OpType::OUT);
            }
        }
        T::local_single_notify_submit();
    }

    free(ep_events);
    return nullptr;
}

template<typename F, typename... Args>
typename std::result_of<F(int, Args...)>::type
EpollState::op_wrapper(F &&op, OpType ty, int sockfd, Args &&... a) {

    assert(ep_state_ == State::READY);
    typename std::result_of<F(int, Args...)>::type ret;

    while (true) {
        // perform the operation
        ret = op(sockfd, std::forward<Args>(a)...);
        // success
        if (ret != -1)
            return ret;
        // failure that was not caused by the O_NONBLOCK flag
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;

        // we need to wait and get woken up from the poller
        LocalSingleAsyncObj lsao;

        // add fd -> async object mapping
        auto entry = fds_.find(sockfd);
        assert(entry != fds_.end());
        LocalSingleAsyncObj **lsao_pptr;
        switch (ty) {
            case OpType::OUT:
            lsao_pptr = &entry->second.ao_out_;
            break;

            case OpType::IN:
            lsao_pptr = &entry->second.ao_in_;
            break;

            default: /* convince GCC that ao_pptr has a value */
            abort();
        }
        assert(*lsao_pptr == nullptr);
        *lsao_pptr = &lsao;
        pending_waits_++;
        ret = T::local_single_wait(&lsao);
        pending_waits_--;
        if (ret != 0) {
            // error: this should be the only case where we get an error here:
            // the system is shutting down
            assert(ret == ESHUTDOWN);
            errno = ESHUTDOWN;
            return -1;
        }
    }
}


EpollState *
EpollState::es_get_rr() {
    static size_t idx = 0;
    return EpollGlobalState__.es_get_mod(idx++);
}

void EpollState::register_and_spawn(int fd, uint32_t m, TaskFn fn,
                                    TaskFnArg arg, EpollSpawnPolicy p) {
    EpollState *es;
    // if local or we got ourself, spawn locally
    if (p == EpollSpawnPolicy::Local || (es = es_get_rr()) == &EpollState__) {
        register_fd(fd, m);
        trt::T::spawn(fn, arg, nullptr, true, trt::TaskType::TASK);
    } else {
        // otherwise enqueue to remote epoll
        es->epoll_spawn_deque_.emplace_back(fd, m, fn, arg);
    }
}

int
EpollState::accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int ret = accept_ll(sockfd, addr, addrlen);
    if (ret > 0)
        register_fd(ret, EPOLLIN /* | EPOLLOUT */);
    return ret;
}

int
EpollState::accept_ll(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    if (ep_state_ == State::DRAINING) {
        errno = ESHUTDOWN;
        return -1;
    }
    return op_wrapper(::accept, OpType::IN, sockfd, addr, addrlen);
}


ssize_t
EpollState::recv(int fd, void *buff, size_t len, int flags) {
    if (ep_state_ == State::DRAINING) {
        errno = ESHUTDOWN;
        return -1;
    }
    return op_wrapper(::recv, OpType::IN, fd, buff, len, flags);
}

ssize_t
EpollState::send(int fd, const void *buff, size_t len, int flags) {
    if (ep_state_ == State::DRAINING) {
        errno = ESHUTDOWN;
        return -1;
    }
    return op_wrapper(::send, OpType::OUT, fd, buff, len, flags);
}

ssize_t
EpollState::sendmsg(int fd, const struct msghdr *msg, int flags) {
    if (ep_state_ == State::DRAINING) {
        errno = ESHUTDOWN;
        return -1;
    }
    return op_wrapper(::sendmsg, OpType::OUT, fd, msg, flags);
}

ssize_t
EpollState::recvmsg(int fd, struct msghdr *msg, int flags) {
    if (ep_state_ == State::DRAINING) {
        errno = ESHUTDOWN;
        return -1;
    }
    return op_wrapper(::recvmsg, OpType::IN, fd, msg, flags);
}

} // end namespace trt
