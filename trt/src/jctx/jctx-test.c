/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <pthread.h>
#include <semaphore.h>

#include "jctx.h"

/*
 * N+1 contexts:
 *  - N: one for each thread
 *  - 1: loop
 */

__thread jctx_t thr_jctx; // thread-local jctx;

struct thr_arg {
	sem_t *wait;
	sem_t *post;
	jctx_t *ctx;
	unsigned i;
};

void *fn(void *arg)
{
	jctx_t *myctx = arg;
	printf("hello from %s [myctx=%p]\n", __FUNCTION__, myctx);

	unsigned nloops = 100;
	for (unsigned i=0; i<nloops; i++) {
		printf("thread:%lu i=%u\n", (unsigned long)pthread_self(), i);
		if (jctx_setjmp(myctx) == 0) {
			jctx_longjmp(&thr_jctx);
			fprintf(stderr, "SHOULD NOT REACH HERE\n");
			abort();
		}
	}

	return (void *)(0xbeef);
}

void *
thread_fn(void *arg)
{
	struct thr_arg *targ = (struct thr_arg *)arg;
	for (;;) {
		sem_wait(targ->wait);
		if (jctx_setjmp(&thr_jctx) == 0) {
			jctx_longjmp(targ->ctx);
			fprintf(stderr, "SHOULD NOT REACH HERE\n");
			abort();
		}
		printf("thread:%lu Continuing\n", (unsigned long)pthread_self());
		sem_post(targ->post);
	};
	return NULL;
}

int main(int argc, const char *argv[])
{
	const unsigned int nthreads = 10;

	pthread_t       tids[nthreads];
	struct thr_arg  targs[nthreads];
	sem_t           semas[nthreads];
	jctx_t          ctx;

	// initilalize ctx
	jctx_init(&ctx, 4096, (void *)fn, &ctx);

	// initialize all semaphores
	for (unsigned int i=0; i<nthreads; i++)
		sem_init(&semas[i], 0, 0);

	// initialize thread structure
	for (unsigned int i=0; i<nthreads; i++) {
		targs[i] = (struct thr_arg){
			.wait = &semas[i],
			.post = &semas[(i+1) % nthreads],
			.ctx  = &ctx,
			.i    = i,
		};
	}

	for (unsigned int i=0; i<nthreads; i++) {
		pthread_create(&tids[i], NULL, thread_fn, &targs[i]);
	}

	sem_post(&semas[0]);

	for (unsigned int i=0; i<nthreads; i++) {
		pthread_join(tids[i], NULL);
	}

	return 0;
}

// vim:noexpandtab:tabstop=8:softtabstop=0:shiftwidth=8:copyindent
