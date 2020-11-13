/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4:

#include <sched.h>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

// place cpu mask to given buffer in a hex form
// returns: 0 or errno in case of error
int cpumask_to_str(cpu_set_t *cpuset,
                   size_t     cpuset_size, /* bytes */
                   char      *buff,
                   size_t     buff_len) {

    // we need at least:
    //  two for the prefix: '0x'
    //  one for the mask
    //  one for the suffix: '\0'
    if (buff_len < 4) {
        //fprintf(stderr, "Too small buffer\n");
        return EINVAL;
    }
    buff[0] = '0';
    buff[1] = 'x';

    // maximum cpu we can hold (each byte can hold 8 bits)
    const size_t max_cpu = cpuset_size * 8;
    size_t cpu = max_cpu;
    char *ptr = NULL;
    do {
        // iterations
        // 0: max_cpu-4, max_cpu-3, max_cpu-2, max_cpu-1
        // 1: max_cpu-8, max_cpu-7, max_cpu-6, max_cpu-5
        //    ...        ...        ...        ...
        // n: 0,         1,         2,         3
        cpu -= 4;

        char val = 0;
        if (CPU_ISSET_S(cpu + 0, cpuset_size, cpuset))
            val |= 1;
        if (CPU_ISSET_S(cpu + 1, cpuset_size, cpuset))
            val |= 2;
        if (CPU_ISSET_S(cpu + 2, cpuset_size, cpuset))
            val |= 4;
        if (CPU_ISSET_S(cpu + 3, cpuset_size, cpuset))
            val |= 8;

        if (!ptr) {
            if (val == 0) continue;           // nothing to write yet, skip
            ptr = buff + 2;                   // initialize ptr
        } else if (++ptr > buff + buff_len - 2) { // progress ptr and check boundaries
            return EINVAL;
        }

        if (val >= 0 && val < 10) {
            *ptr = '0' + val;
        } else if (val >= 10 && val < 16) {
            *ptr = ('a' - 10) + val;
        } else {
            //fprintf(stderr, "Unexpected val=%c\n", val);
            return EINVAL;
        }

    } while (cpu != 0);

    if (!ptr) {
        ptr = buff + 2;
        *ptr = '0';
    }
    ptr[1] = '\0';
    return 0;
}


#if defined(CPUMASK_UTILS_TEST)

#if defined(NDEBUG)
#warning "Tests use assert but NDEBUG is defined"
#endif

int main(int argc, char *argv[])
{
    cpu_set_t cpuset[4];
    size_t setsize = sizeof(cpuset);
    int err;

    {
        char buff[1024] = "XXXXXXXXXX";
        CPU_ZERO_S(setsize, cpuset);
        err = cpumask_to_str(cpuset, setsize, buff, 4);
        assert(!err);
        assert(!strcmp("0x0", buff));
    }


    {
        char buff[1024] = "XXXXXXXXXX";
        CPU_ZERO_S(setsize, cpuset);
        CPU_SET_S(0, setsize, cpuset);
        err = cpumask_to_str(cpuset, setsize, buff, 4);
        assert(!err);
        assert(!strcmp("0x1", buff));
    }

    {
        char buff[1024] = "XXXXXXXXXX";
        CPU_ZERO_S(setsize, cpuset);
        CPU_SET_S(1, setsize, cpuset);
        err = cpumask_to_str(cpuset, setsize, buff, 4);
        assert(!err);
        assert(!strcmp("0x2", buff));
    }

    {
        char buff[1024] = "XXXXXXXXXX";
        CPU_ZERO_S(setsize, cpuset);
        CPU_SET_S(2, setsize, cpuset);
        err = cpumask_to_str(cpuset, setsize, buff, 4);
        assert(!err);
        assert(!strcmp("0x4", buff));
    }

    {
        char buff[1024] = "XXXXXXXXXX";
        CPU_ZERO_S(setsize, cpuset);
        CPU_SET_S(3, setsize, cpuset);
        err = cpumask_to_str(cpuset, setsize, buff, 4);
        assert(!err);
        assert(!strcmp("0x8", buff));
    }

    {
        char buff[1024] = "XXXXXXXXXX";
        CPU_ZERO_S(setsize, cpuset);
        CPU_SET_S(0, setsize, cpuset);
        CPU_SET_S(1, setsize, cpuset);
        CPU_SET_S(2, setsize, cpuset);
        CPU_SET_S(3, setsize, cpuset);
        err = cpumask_to_str(cpuset, setsize, buff, 4);
        assert(!err);
        assert(!strcmp("0xf", buff));
    }

    {
        char buff[1024] = "XXXXXXXXXX";
        CPU_ZERO_S(setsize, cpuset);
        CPU_SET_S(4, setsize, cpuset);
        err = cpumask_to_str(cpuset, setsize, buff, 4);
        assert(err);
    }

    {
        char buff[1024] = "XXXXXXXXXX";
        CPU_ZERO_S(setsize, cpuset);
        CPU_SET_S(4, setsize, cpuset);
        err = cpumask_to_str(cpuset, setsize, buff, 5);
        assert(!err);
        assert(!strcmp("0x10", buff));
    }

    return 0;
}
#endif
