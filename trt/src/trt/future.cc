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

#include "trt/future.hh"
#include "trt/waitset.hh"
#include "trt/async_obj.hh"

namespace trt {

Future::Future(AsyncObj *aobj, Waitset *ws, void *caller_ctx)
 : f_aobj_(aobj)
 , f_waitset_(ws)
 , f_caller_ctx_(caller_ctx)
 , f_registered_(false) {
    // invalid future: ... nothing else matters
    if (f_aobj_ == nullptr)
        return;
    // Register this future to the asyncronous object, so that it will not
    // go away until we get its value.
    f_aobj_->subscribe_();
    // Add this future to the specified waitset
    if (f_waitset_ != nullptr) {
        f_waitset_->add_future(*this);
    } else {
        fprintf(stderr, "Error: Future constructor with a NULL waitset\n");
        fprintf(stderr, "For now, all futures should be a part of a waitset\n");
        abort();
    }
}

TaskBase *
Future::set_ready(void) {
    assert(f_registered_);
    f_registered_ = false;
    return f_waitset_->set_ready();
}

} // end namespace trt
