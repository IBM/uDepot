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

#ifndef TIMER_H
#define TIMER_H

#include <sys/time.h>

struct timer {
	struct timeval tv_start;

	unsigned long usecs_total;
	unsigned long usecs_last;
	unsigned long pauses;
};
typedef struct timer xtimer_t;

static inline void timer_init(xtimer_t *t)
{
	t->usecs_total = 0;
	t->usecs_last = 0;
	t->pauses = 0;
}

static inline void timer_start(xtimer_t *t)
{
	gettimeofday(&t->tv_start, NULL);
}

static inline void timer_pause(xtimer_t *t)
{
	struct timeval tv_stop;
	unsigned long usecs;

	gettimeofday(&tv_stop, NULL);
	usecs = (tv_stop.tv_sec - t->tv_start.tv_sec)*1000000;
	usecs += tv_stop.tv_usec;
	usecs -= t->tv_start.tv_usec;

	t->usecs_last = usecs;
	t->usecs_total += usecs;
	t->pauses++;
}

static double inline timer_secs(xtimer_t *t)
{
	return (double)t->usecs_total/(double)1000000;
}

#endif
