/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop:4 shiftwidth:4:

#ifndef TRT_COMMON_HH_
#define TRT_COMMON_HH_

#include <cinttypes>

namespace trt {

using RetT      = uint64_t;
using TaskFnArg = void *;
using TaskFn    = void *(*)(TaskFnArg);

} // end namespace trt


#endif // TRT_COMMON_HH_
