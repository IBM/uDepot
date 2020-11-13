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

// Simple trt+epoll multi-threaded server
//
// We currently use one epoll context per thread. This means that if we have one
// listening socket, we need a way to distribute the load. For that, we use teh
// register_and_spawn() helper function.

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <signal.h>

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_epoll.hh"

extern "C" {
    #include "trt_util/net_helpers.h"
}

trt::ControllerBase *Controller_g_ = nullptr;

static void *
serve_task(void *arg)
{
    int fd = (int)(uintptr_t)arg;
    ssize_t r_ret, s_ret;
    char indata[1];
    trt_msg("receiving ....\n");
    r_ret = trt::Epoll::recv(fd, indata, sizeof(indata), 0);
    if (r_ret < 0) {
        perror("recv");
        abort();
    }

    uint64_t handle = (uint64_t)trt::Epoll::epoll_handle();
    trt_msg("sending: %" PRIx64 "\n", handle);
    s_ret = trt::Epoll::send(fd, &handle, sizeof(handle), 0);
    if (s_ret < 0) {
        perror("send");
        abort();
    } else if (s_ret != sizeof(handle)) {
        fprintf(stderr, "Partial write: FIXME!\n");
        abort();
    }

    if (indata[0] == 'x') {
        trt_dmsg("Received last connection\n");
    }

    trt::Epoll::close(fd);
    return nullptr;
}

static void *
srv_listen(void *arg)
{
    int err, fd;
    struct url url;
    const char *url_str = (const char *)arg;
    struct addrinfo *ai_list, *ai;
    const int optval = 1;

    // initialize epoll and start poller
    trt::Epoll::init();
    trt::T::spawn(trt::Epoll::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);

    // bind and listen to URL given by the user
    err = url_parse(&url, url_str);
    if (err) {
        fprintf(stderr, "url_parse failed\n");
        exit(1);
    }

    bool bind_ok = false;
    ai_list = url_getaddrinfo(&url, true);
    for (ai = ai_list; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == -1) {
            perror("socket call failed\n");
            continue;
        }

        err = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
        if (err) {
            perror("setsockopt");
            close(fd);
            continue;
        }

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            perror("bind");
            close(fd);
            continue;
        }

        bind_ok = true;
        break;
    }

    freeaddrinfo(ai_list);
    if (!bind_ok) {
        fprintf(stderr, "Could not bind to an address\n");
        abort();
    }

    trt_dmsg("listen\n");
    err = trt::Epoll::listen(fd, 128);
    if (err == -1) {
        perror("listen failed");
        exit(1);
    }

    struct sockaddr cli_addr;
    socklen_t cli_addr_len;
    while (true) {
        trt_msg("accept\n");
        int afd = trt::Epoll::accept_ll(fd, &cli_addr, &cli_addr_len);
        trt_msg("accept returned: %d\n", afd);
        if (afd == -1) {
            perror("accept");
        }

        trt::TaskFn fn = serve_task;
        trt::TaskFnArg targ = (void *)(uintptr_t)afd;
        trt_msg("registering and spawining task...\n");
        trt::Epoll::register_and_spawn(afd, EPOLLIN | EPOLLOUT, fn, targ,
                                       trt::Epoll::SpawnPolicy::Distribute);
    }

    return nullptr;
}

// server without the listen socket
static void *
srv(void *)
{
    // initialize epoll and start poller
    trt::Epoll::init();
    trt::T::spawn(trt::Epoll::poller_task, nullptr, nullptr, true, trt::TaskType::TASK);

    return nullptr;
}

void client(const char url_str[])
{
    int err, fd;
    struct url url;
    struct addrinfo *ai_list;
    char b[1] = {0};
    const size_t NLOOPS = 5;

    // bind and listen to URL given by the user
    err = url_parse(&url, url_str);
    if (err) {
        fprintf(stderr, "url_parse failed\n");
        exit(1);
    }
    ai_list = url_getaddrinfo(&url, true);

    for (size_t i=0; i<NLOOPS; i++) {
        printf("%s: connecting...\n", __PRETTY_FUNCTION__);
        fd = ai_connect(ai_list, NULL);
        if (fd < 0) {
            fprintf(stderr, "ai_connect failed\n");
            exit(1);
        }

        if (i == NLOOPS - 1)
            b[0] = 'x'; // mark that we are DONE

        printf("%s: sending...\n", __PRETTY_FUNCTION__);
        ssize_t ret;
        ret = send(fd, b, sizeof(b), 0);
        if (ret != sizeof(b)) {
            fprintf(stderr, "send returned: %zd errno: %d (%s)\n", ret, errno, strerror(errno));
            abort();
        }

        uint64_t indata;
        ret = recv(fd, &indata, sizeof(indata), 0);
        if (ret != sizeof(indata)) {
            fprintf(stderr, "recv returned: %zd errno: %d (%s)\n", ret, errno, strerror(errno));
            abort();
        }
        printf("%s received: %" PRIx64 "\n", __PRETTY_FUNCTION__, indata);
        close(fd);
    }

    return;
}

// fills set with all signals, and blocks them
static void
disable_signals(sigset_t *sigset)
{
    int err;
    err = sigfillset(sigset);
    if (err)
        abort();
    err = pthread_sigmask(SIG_BLOCK, sigset, NULL);
    if (err)
        abort();
}


static void
enable_signals(sigset_t *sigset)
{
    int err;
    err = pthread_sigmask(SIG_UNBLOCK, sigset, NULL);
    if (err)
        abort();
}


static void *task_exit(void *arg)
{
    trt_msg("The begining of the end ...\n");
    trt::Epoll::stop();
    return nullptr;
}

static void
spawn_exit_task(trt::Scheduler &s) {

    trt::Task *t = trt::T::alloc_task(task_exit, nullptr /* arg */);
    if (!t) {
        printf("Unable to allocate task. Cannot exit\n");
        return;
    }
    printf("Allocated task: %p\n", t);

    bool ok = s.remote_push_task_front(*t);
    if (!ok) {
        printf("Unable to push task. Cannot exit\n");
        trt::T::free_task(t);
    }
    printf("Pushed task %p to scheduler %p\n", t, &s);
}

static void signal_exit_handler(int signum)
{
    static bool called_once = false;

    fprintf(stderr, "received signal %s %d\n", strsignal(signum), signum);
    if (called_once)
        exit(1);
    called_once = true;

    if (!Controller_g_) {
        exit(1);
    }

    Controller_g_->on_each_scheduler(spawn_exit_task);
}

static int install_exit_signal_handler()
{
    int err = 0;
    struct sigaction saction;
    saction.sa_handler = signal_exit_handler;
    sigemptyset (&saction.sa_mask);
    saction.sa_flags = 0;
    err |= sigaction(SIGINT, &saction, NULL);
    err |= sigaction(SIGQUIT, &saction, NULL);
    err |= sigaction(SIGABRT, &saction, NULL);
    err |= sigaction(SIGUSR1, &saction, NULL);
    err |= sigaction(SIGUSR2, &saction, NULL);
    err |= sigaction(SIGTSTP, &saction, NULL);
    err |= sigaction(SIGSEGV, &saction, NULL);
    #if 0
    // ignore SIGCHLD
    saction.sa_handler = SIG_IGN;
    saction.sa_flags = SA_NOCLDSTOP;
    err |= sigaction(SIGCHLD, &saction, NULL);
    #endif
    return err;
}


int main(int argc, char *argv[])
{
    trt::Controller c;
    Controller_g_ = &c;

    sigset_t sigset;
    const char server_url[] = "tcp://*:8000";
    const char connect_url[] = "tcp://localhost:8000";

    // disable all sginals so that the new threads will inherit a signal mask
    // that blocks everything.
    disable_signals(&sigset);
    // spawn two schedulers
    c.spawn_scheduler(srv_listen, (void *)server_url, trt::TaskType::TASK);
    c.spawn_scheduler(srv, nullptr, trt::TaskType::TASK);
    enable_signals(&sigset);
    usleep(1000000);

    pid_t p;
    if ((p = fork()) == 0) {
        /* client */
        client(connect_url);
        exit(0);
    } else if (p == -1) {
        fprintf(stderr, "fork() failed: %d (%s)", errno, strerror(errno));
        abort();
    }

    install_exit_signal_handler();

    /* server */
    printf("Waiting for client %d\n", p);
    p = waitpid(p, NULL, 0);
    printf("Client DONE. Press Ctlr-C to exit\n");

    c.set_exit_all();
    c.wait_for_all();
    return 0;
}
