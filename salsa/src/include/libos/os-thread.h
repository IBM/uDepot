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
#ifndef _SALSA_THREAD_H_
#define _SALSA_THREAD_H_

#include "libos/os-atomic.h"

#include <pthread.h>
typedef pthread_t		os_thread_t;
typedef void *			os_thread_fun_return;
#define os_thread_create(_THREADP, _ROUTINE, _ARG, _NAME)	pthread_create((_THREADP), NULL, _ROUTINE, _ARG)
#define os_thread_cancel(_THREADP)				pthread_cancel(_THREADP)
#define os_thread_stop(_THREADP)				pthread_cancel(_THREADP)
#define os_thread_should_stop()					(0)

#include <utmpx.h>

#define os_cpu_get_id()	sched_getcpu()

struct os_thread_arg {
	void *private;
	u32   id;
};

int os_init_thread(
	void                     *const private,
	os_thread_fun_return          (*fn) (void *arg),
	os_thread_t              *const thread,
	os_atomic32_t            *const done,
	const u32                  tid,
	const char               *const name);

#endif	/* _SALSA_THREAD_H_ */
