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


#ifndef	_UDEPOT_RWLOCK_PAGEFAULT_
#define	_UDEPOT_RWLOCK_PAGEFAULT_

/**
 * Let's say we have a big array of memory (e.g., a hash table) and that at rare
 * points we want to grow/shrink it.
 *
 * One solution would be to have a (traditional) RW lock, where all normal
 * access threads get it as readers, and the resizer gets it as a writer for as
 * long as it operates.  When the resizer requests the RW lock, all threads
 * trying to get the lock should fail (to avoid starving the resizer), and the
 * resizer should wait until normal access threads that already got the (read)
 * lock terminate.
 *
 * It would be good if we could serve requests during the copy:
 *   (1) RD requests we can serve from the old version, since nothing changes
 *   (2) WR requests are more tricky, and we would need some kind of log for the
 *     changes that we could replay on the new version.
 *
 * The code here implements (1) by mapping the table RO during the copy, and
 * installing SIGSEGV handlers that will rollback operations in case of a page
 * fault.
 *
 * see test/rwlock-pagefault/resizable_table for an example
 */

#include <atomic>
#include <functional>
#include <setjmp.h>
#include <assert.h>

#include "util/types.h"
#include "uDepot/brlock.hh"

namespace udepot {

/**
 * rwlock_pagefault_base
 *
 * Basic interface, and simple rw spinlock.
 */

class rwlock_pagefault_base {
protected:
	BRLock brlock_;
	// This is not strictly needed, but it simplifies the futex code.
	std::atomic<u32> write_in_progress_;

public:
	rwlock_pagefault_base(rwlock_pagefault_base const&) = delete;
	rwlock_pagefault_base &operator=(rwlock_pagefault_base const&) = delete;

	rwlock_pagefault_base() : brlock_(), write_in_progress_(false) {};
	virtual ~rwlock_pagefault_base() {};

protected:
	bool rd_try_lock() {
		int ok = brlock_.rd_try_lock();
		assert(ok || write_in_progress_);
		return ok;
	}

	void rd_unlock() {
		brlock_.rd_unlock();
	}

	void wr_enter() {
		assert(!write_in_progress_);
		write_in_progress_ = 1;
		brlock_.wr_enter();
	}

	bool wr_ready() {
		assert(write_in_progress_);
		return brlock_.wr_ready();
	}

	void wr_exit() {
		brlock_.wr_exit();
		write_in_progress_ = 0;
	}
};

class rwlock_pagefault : public rwlock_pagefault_base {
public:
	rwlock_pagefault();
	~rwlock_pagefault();

	int init();

	void rd_enter();
	void rd_exit();

	void write_enter();
	void write_wait_readers();
	void write_exit();

	// prepare_f:
	//  prepare for executing the given operation This function should
	//  probalby call .rd_enter();
	//
	// rollback_f:
	//  we got a pagefault and we need to rollback execution.  This function
	//  should probably call .rd_exit()
	//
	// finalize_f:
	//  operation_f completed without a rollback.  This function should
	//  probably call .rd_exit()
	//
	// operation_f: do the operation
	//
	// Why this is needed? In the implmenetation we take a snapshot so that
	// we can rollback in case of a pagefault. After we take a snapshot and
	// until we are done, we can only push into the stack, never pop,
	// because our checkpoint (that includes a point in the stack) might
	// become invalid.
	template<typename F, typename... Args>
	typename std::result_of<F(Args...)>::type
	rd_execute__(std::function<void(void)> prepare_f,
	             std::function<void(void)> rollback_f,
	             std::function<void(void)> finalize_f,
	             F &&operation_f, Args &&... a);

	// convience method where only the ->rd_enter/->rd_exit() functions are
	// called.
	// TODO: make template as above
	long
	rd_execute(std::function<long(void)> operation_f) {
		return rd_execute__(
			[this] { this->rd_enter(); },
			[this] { this->rd_exit();  },
			[this] { this->rd_exit();  },
			operation_f);
	}

private:
	void reader_wait_for_writer();
};


struct rwlpf_rb {
	int        rb_set; // rollback buffer validity
	sigjmp_buf rb_jmp;
};

extern thread_local rwlpf_rb rwlpf_rb__;

template<typename F, typename... Args>
typename std::result_of<F(Args...)>::type
rwlock_pagefault::rd_execute__(std::function<void(void)> prepare_f,
                               std::function<void(void)> rollback_f,
                               std::function<void(void)> finalize_f,
                               F &&operation_f, Args &&... a) {
	for (;;) {
		prepare_f();
		assert(rwlpf_rb__.rb_set == 0);
		if (sigsetjmp(rwlpf_rb__.rb_jmp, 1) == 0)
			break;
		/* rollback happend, retry */
		assert(rwlpf_rb__.rb_set == 1);
		rwlpf_rb__.rb_set = 0;
		rollback_f();
	}

	rwlpf_rb__.rb_set = 1;
	auto ret = operation_f(std::forward<Args>(a)...);
	finalize_f();
	assert(rwlpf_rb__.rb_set == 1);
	rwlpf_rb__.rb_set = 0;
	return ret;
}

}; // udepot

#endif	// _UDEPOT_RWLOCK_PAGEFAULT_
