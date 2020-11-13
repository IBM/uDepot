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
#include "libos/os-thread.h"

#include <asm-generic/errno.h>

#include "libos/os-atomic.h"
#include "libos/os-debug.h"
#include "libos/os-malloc.h"

int os_init_thread(
	void                     *const private,
	os_thread_fun_return    (*const fn) (void *),
	os_thread_t              *const thread,
	os_atomic32_t            *const done,
	const u32                  tid,
	const char               *const name)
{
	int err = 0;
	struct os_thread_arg *arg = os_malloc(sizeof(*arg));
	ERR_CHK_SET_PRNT_GOTO((NULL == arg), err, ENOMEM, fail0,
			"arg malloc failed");
	arg->private = private;
	arg->id      = tid;
	os_atomic32_zero(done);
	err = os_thread_create(thread, fn, arg, name);
	ERR_CHK_PRNT_GOTO(unlikely(0 != err), fail1,
			"thread creation failed for name=%s tid=%u with %d",
			name, tid, err);
	assert(0 == err);
	return 0;
fail1:
	os_free(arg);
	os_atomic32_set(done, 1);
fail0:
	return err;
}
