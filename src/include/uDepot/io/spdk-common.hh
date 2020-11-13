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

#ifndef _UDEPOT_SPDK_IO_COMMON_HH_
#define _UDEPOT_SPDK_IO_COMMON_HH_


#include "uDepot/io.hh"
#include "util/debug.h"

#include "trt_util/rte.hh"

namespace udepot {

struct SpdkNativeNYI : public RteAlloc<512> {
	using Ptr = RteAlloc<512>::Ptr;

	ssize_t pread_native(Ptr buff, size_t len, off_t off) {
		UDEPOT_ERR("%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
		abort();
	}

	ssize_t pwrite_native(Ptr buff, size_t len, off_t off) {
		UDEPOT_ERR("%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
		abort();
	}

	// error is returned as negative number
	ssize_t preadv_native(IoVec<Ptr>  iov, off_t off) {
		UDEPOT_ERR("%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
		abort();
	}

	ssize_t pwritev_native(IoVec<Ptr> iov, off_t off) {
		UDEPOT_ERR("%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
		abort();
	}
};

} // end namespace udepot

#endif /* ifndef _UDEPOT_SPDK_IO_COMMON_HH_ */
