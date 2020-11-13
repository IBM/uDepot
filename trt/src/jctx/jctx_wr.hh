/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */
#ifndef JCTX_WR_H__
#define JCTX_WR_H__

#include <functional>

extern "C" {
	#include "jctx.h"
}


class JctxWr {
	friend void *JctxWr_wrapper(JctxWr *, void *arg);

protected:
	std::function<void *(void *)> fn_;

public:
	JctxWr() = delete;
    JctxWr(JctxWr const&) = delete;
    void operator=(JctxWr const&) = delete;

    JctxWr(std::function<void *(void *)> fn)
    : fn_{ std::move(fn) } {}

};

#endif
