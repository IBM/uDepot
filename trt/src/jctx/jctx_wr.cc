/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

#include "jctx_wr.hh"

void *
JctxWr_wrapper(JctxWr *j, void *arg)
{
	return j->fn_(arg);
}

#if defined(JCTX_FN_TEST)
int main(int argc, char *argv[])
{
	jctx_t jctx;
	JctxWr jctxWr{ [] (void *arg) -> void * {
		printf(" got arg: %p\n", arg);
		return (void *)0xff; }
	};

	jctx_init2(&jctx, 4096, (void *)JctxWr_wrapper, &jctxWr, (void *)0xdead);

	jctx_longjmp(&jctx);
	return 0;
}
#endif /* JCTX_FN_TEST */
