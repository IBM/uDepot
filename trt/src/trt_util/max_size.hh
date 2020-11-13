/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */
#ifndef MAX_SIZE_HH_
#define MAX_SIZE_HH_

template <typename H>
constexpr size_t max_size() { return sizeof(H); }

template <typename H, typename N, typename... T>
constexpr size_t max_size() {
    return max_size<H>() > max_size<N, T...>()
           ? max_size<H>()
           : max_size<N, T...>();
}

#endif /* ifndef MAX_SIZE_HH_ */
