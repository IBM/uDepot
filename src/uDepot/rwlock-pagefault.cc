/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
 *           Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

#include "uDepot/rwlock-pagefault.hh"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <assert.h>
#include <setjmp.h>
#include <signal.h>
#include <pthread.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <limits.h>
#include <errno.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <uDepot/thread-id.hh>

#include "util/debug.h"

namespace udepot {

/**
 * rwlock_pagefault
 *
 * SIGSEGV handler, and futex sleeping.
 */

// Wrapper for the futex system call.
// We use this to put threads to sleep when a resize operation occurs.
static inline long
sys_futex(void *addr1, int op, int v1, struct timespec *t, void *addr2, int v3)
{
	return syscall(SYS_futex, addr1, op, v1, t, addr2, v3);
}

// Each thread needs to have its own rollback buffer (@rb_jmp). We also maintain
// a flag (rb_set) to indicate whether the rollback is set.
thread_local rwlpf_rb rwlpf_rb__ = {0};
struct sigaction oldact_g;

rwlock_pagefault::rwlock_pagefault() : rwlock_pagefault_base() {}

rwlock_pagefault::~rwlock_pagefault() {}

// the read_lock() part
void
rwlock_pagefault::rd_enter()
{
	for (;;) {
		bool ret = rwlock_pagefault_base::rd_try_lock();
		if (ret)
			return;
		// failed: there is a resize in-progress: sleep
		reader_wait_for_writer();
	}
}

void
rwlock_pagefault::rd_exit()
{
	rwlock_pagefault_base::rd_unlock();
}


// helper function to wait in a futex.
// We use the ->resize_in_progress as the futex condition.
void
rwlock_pagefault::reader_wait_for_writer()
{
	int ret = sys_futex(&write_in_progress_, FUTEX_WAIT, 1, NULL, NULL, 0);
	if (ret == 0)
		return;

	/* something different that wake-up happend */
	perror("futex_wait");
	switch (errno) {
		case EACCES:
		UDEPOT_ERR("futex_wait: No read access to futex memory\n");
		abort();

		case EFAULT:
		case EINVAL:
		case ENFILE:
		case ENOSYS:
		case ETIMEDOUT:
		UDEPOT_ERR("futex_wait: Error:%d", ret);
		abort();
		break;

		case EAGAIN:
		//case EWOULDBLOCK:
		case EINTR:
		//case FUTEX_CMP_REQUEUE:
		UDEPOT_ERR("futex_wait: Retrying...\n");
		break;

		default:
		UDEPOT_ERR("futex_wait: Unknown ret value=%d\n", ret);
		abort();
	}
}

void
rwlock_pagefault::write_enter()
{
	rwlock_pagefault_base::wr_enter();
}

void rwlock_pagefault::write_wait_readers()
{
	while (!rwlock_pagefault_base::wr_ready())
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

void
rwlock_pagefault::write_exit()
{
	rwlock_pagefault_base::wr_exit();
	// notif all threads.
	sys_futex(&write_in_progress_, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}


static void
sigsegv_handler(int sig, siginfo_t *siginfo, void *uctx)
{
	UDEPOT_DBG("SIGSEGV!\n");
	if (rwlpf_rb__.rb_set != 1) {
		// UDEPOT_ERR("SIGSEGV without rollback set. Calling old handler\n");
		// abort();
		return (oldact_g.sa_sigaction(sig, siginfo, uctx));
	}
	UDEPOT_DBG("SIGSEGV: roll back\n");
	// TODO: check address (we need a global variable)
	siglongjmp(rwlpf_rb__.rb_jmp, 1);
}

int
rwlock_pagefault::init()
{
	struct sigaction sa = {0};
	sa.sa_sigaction = sigsegv_handler;
	sa.sa_flags = SA_SIGINFO;
	if (sigaction(SIGSEGV, &sa, &oldact_g) == -1) {
		perror("sigaction");
		return errno;
	}
	return 0;
}


} // namespace udepot
