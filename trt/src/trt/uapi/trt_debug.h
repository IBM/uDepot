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
#ifndef TRT_DEBUG_H_
#define TRT_DEBUG_H_

/*
 * Allow trt debug prints without having a dependency to trt by using weak
 * aliases
 */

uint64_t trt_dbg_get_tid(void) __attribute__((weak)) ;
uint64_t trt_dbg_get_sid(void) __attribute__((weak)) ;

#define trt_debug_print_str__ "S%-4ld:T%-4ld %20s()"
#define trt_debug_print_arg__ trt_dbg_get_sid(), trt_dbg_get_tid(), __FUNCTION__
#define trt_debug_print(msg, fmt, args...) \
    printf(trt_debug_print_str__ " " msg fmt , trt_debug_print_arg__ , ##args)

#if !defined(NDEBUG)
    #define trt_debug(fmt,args...) trt_debug_print("", fmt, ##args)
#else
    #define trt_debug(fmt,args...) do { } while (0)
#endif


uint64_t trt_dbg_get_tid(void) {
    return (uint64_t)-1;
}

uint64_t trt_dbg_get_sid(void) {
    return (uint64_t)-1;
}

#endif /* TRT_DEBUG_H_ */

