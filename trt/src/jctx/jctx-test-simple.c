/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim:noexpandtab:tabstop=8:softtabstop=0:shiftwidth=8:copyindent

#include <stdio.h>
#include "jctx.h"

static void *fn(void *arg)
{
	printf("got arg: %p\n", arg);
	return (void *)(0xff);
}

int main(int argc, const char *argv[])
{
	jctx_t jctx;
	jctx_init(&jctx, 4096, fn, (void *)0xdead);
	printf("new jctx:%p buffer:%p stack:[%p,+%lu] fn:%p\n", &jctx, &jctx.ctx_, jctx.stk, jctx.stk_size, fn);

	//printf("FP=%p\n", (void *)jctx_fp(&jctx));
	//printf("IP=%p\n", (void *)jctx_ip(&jctx));
	//printf("SP=%p\n", (void *)jctx_sp(&jctx));

	#if 0
	if (jctx_setjmp(&jctx) == 0) {
		printf("Jump completed\n");
		return 0;
	}
	#endif

	printf("Jumping!\n");
	jctx_jump(&jctx);
	return 0;
}
