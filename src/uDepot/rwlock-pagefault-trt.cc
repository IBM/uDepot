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

#include <stdio.h>
#include <signal.h>

#include "util/debug.h"
#include "uDepot/rwlock-pagefault-trt.hh"
#include "trt/uapi/trt.hh"

namespace udepot {

static struct sigaction oldact_g;

static void
sigsegv_handler(int sig, siginfo_t *siginfo, void *uctx)
{
	UDEPOT_DBG("Got SIGSEGV!\n");
	trt::Task *t = &trt::T::self();
	if (t->rwpf_rb_set != 1) {
		//trt_err("SIGSEGV without rollback set. Calling old handler\n");
		return (oldact_g.sa_sigaction(sig, siginfo, uctx));
	}

	/* MAYBE TODO: register and check address for faults */
	//siglongjmp(t->rwpf_rb_jmp, 1);
	longjmp(t->rwpf_rb_jmp, 1);
}

rwlock_pagefault_trt::rwlock_pagefault_trt() : rwpf_trt_ao_(nullptr) {
	pthread_spin_init(&rwpf_trt_lock_, 0);
}

rwlock_pagefault_trt::~rwlock_pagefault_trt() {}

int
rwlock_pagefault_trt::init()
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

// the read_lock() part
void
rwlock_pagefault_trt::rd_enter()
{
	for (;;) {
		bool ok = rwlock_pagefault_base::rd_try_lock();
		if (ok) {
			return;
		}

		// failed: there is a resize in-progress: sleep
		trt::Waitset ws;
		trt::Future  future;
		// use a move constructor under the lock for getting the async object
		// reference. If there no reference exists, retry. The future will
		// subscribe to the async objet under the lock, so we are sure that it's
		// not getting away.
		bool retry = false;
		pthread_spin_lock(&rwpf_trt_lock_);
		if (rwpf_trt_ao_ == nullptr)
			retry = true;
		else
			future = trt::Future(rwpf_trt_ao_, &ws, nullptr);
		pthread_spin_unlock(&rwpf_trt_lock_);
		if (retry)
			continue;

		// wait
		trt::Future *f__ __attribute__((unused)) = ws.wait_();
		assert(f__ == &future);
		assert(future.get_val() == 0xbeef);
		future.drop_ref();
	}
}

void
rwlock_pagefault_trt::rd_exit()
{
	rwlock_pagefault_base::rd_unlock();
}

void
rwlock_pagefault_trt::write_enter()
{
	// before we start draining readers, we need a few things first.

	// first, we should be the only writer -- other writers are assumed to be
	// excluded externally.
	assert(rwpf_trt_ao_ == nullptr);

	// Allocate an async object. It will be deallocated when it's reference
	// count reaches zero.
	//
	// Since readers are not blocked yet, the lock is probably not needed.
	// But, I take it anyway since we are already on the slow path.
	pthread_spin_lock(&rwpf_trt_lock_);
	rwpf_trt_ao_ = trt::AsyncObj::allocate();
	if (rwpf_trt_ao_ == nullptr)  {
		perror("malloc");
		abort();
	}
	pthread_spin_unlock(&rwpf_trt_lock_);

	// start draining readers
	rwlock_pagefault_base::wr_enter();
}

void
rwlock_pagefault_trt::write_wait_readers()
{
	while (!rwlock_pagefault_base::wr_ready())
			trt::T::yield();
}

void
rwlock_pagefault_trt::write_exit()
{
	trt::AsyncObj *old_ao = nullptr;

	// remove the AsyncObj reference under the lock
	pthread_spin_lock(&rwpf_trt_lock_);
	std::swap(old_ao, rwpf_trt_ao_);
	pthread_spin_unlock(&rwpf_trt_lock_);

	// let readers pass
	rwlock_pagefault_base::wr_exit();

	// notify readers sleeping
	assert(old_ao != nullptr);
	trt::T::notify(old_ao, 0xbeef, trt::NotifyPolicy::LastTaskScheduler);
}

} // end namespace udepot
