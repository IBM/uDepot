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

#ifndef	_UDEPOT_RWLOCK_PAGEFAULT_TRT_
#define	_UDEPOT_RWLOCK_PAGEFAULT_TRT_

#include "uDepot/rwlock-pagefault.hh"
#include "trt/uapi/trt.hh"

namespace udepot {

class rwlock_pagefault_trt : public rwlock_pagefault_base {
public:
	rwlock_pagefault_trt();
	~rwlock_pagefault_trt();

	int init();

	void rd_enter();
	void rd_exit();

	void write_enter();
	void write_wait_readers();
	void write_exit();

	// See rwlock_pagefault for a description of these methods.
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
	// We use the lock to protect the AsyncObj reference
	pthread_spinlock_t  rwpf_trt_lock_;
	trt::AsyncObj      *rwpf_trt_ao_;
};

template<typename F, typename... Args>
typename std::result_of<F(Args...)>::type
rwlock_pagefault_trt::rd_execute__(std::function<void(void)> prepare_f,
                                   std::function<void(void)> rollback_f,
                                   std::function<void(void)> finalize_f,
                                   F &&operation_f, Args &&... a) {

	trt::Task *t = &trt::T::self();
	for (;;) {
		prepare_f();
		assert(t->rwpf_rb_set == 0);
		//if (sigsetjmp(t->rwpf_rb_jmp, 1) == 0)
		if (setjmp(t->rwpf_rb_jmp) == 0)
			break;
		/* rollback happend, retry */
		//trt_dmsg("ROLLBACK!\n");
		#if !defined(NDEBUG)
		trt::T::getS()->rwpf_rb_count++;
		#endif
		assert(t->rwpf_rb_set == 1);
		t->rwpf_rb_set = 0;
		//trt::T::yield();
		rollback_f();
	}

	t->rwpf_rb_set = 1;
	auto ret = operation_f(std::forward<Args>(a)...);
	finalize_f();
	assert(t->rwpf_rb_set == 1);
	t->rwpf_rb_set = 0;
	return ret;
}

 } // end udepot namespace

#endif //  _UDEPOT_RWLOCK_PAGEFAULT_TRT_
