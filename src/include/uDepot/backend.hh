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
#ifndef _UDEPOT_BACKEND_HH_
#define _UDEPOT_BACKEND_HH_

#include "uDepot/sync.hh"
#include "uDepot/sched.hh"
#include "uDepot/io.hh"
#include "uDepot/io/file-direct.hh"
#include "uDepot/io/trt-aio.hh"
#include "uDepot/io/trt-uring.hh"
#include "uDepot/net/socket.hh"
#include "uDepot/net/trt-epoll.hh"
#include "uDepot/net/memcache.hh"
#include "uDepot/net/trt-memcache.hh"
#include "uDepot/rwlock-pagefault.hh"
#include "uDepot/rwlock-pagefault-trt.hh"

#if defined(UDEPOT_TRT_SPDK)
#include "uDepot/io/trt-spdk.hh"
#include "uDepot/io/trt-spdk-array.hh"
#include "uDepot/io/spdk.hh"
#endif // UDEPOT_TRT_SPDK

namespace udepot {

class RuntimePosix {
public:
	typedef rwlock_pagefault RwpfTy;
	typedef PthreadLock      LockTy;
	typedef FileIO           IO;
	typedef PthreadSched     Sched;
	typedef SocketNet        Net;
};

class RuntimePosixODirect {
public:
	typedef rwlock_pagefault     RwpfTy;
	typedef PthreadLock          LockTy;
	typedef FileIODirect         IO;
	typedef PthreadSched         Sched;
	typedef SocketNet            Net;
};

class RuntimeTrt {
public:
	typedef rwlock_pagefault_trt RwpfTy;
	typedef TrtLock              LockTy;
	typedef TrtFileIO            IO;
	typedef TrtSched             Sched;
	typedef TrtEpollNet          Net;
};

class RuntimeTrtUring {
public:
	typedef rwlock_pagefault_trt RwpfTy;
	typedef TrtLock              LockTy;
	typedef TrtFileIOUring       IO;
	typedef TrtSched             Sched;
	typedef TrtEpollNet          Net;
};

#if defined(UDEPOT_TRT_SPDK)
class RuntimeTrtSpdk {
public:
	typedef rwlock_pagefault_trt RwpfTy;
	typedef TrtLock              LockTy;
	typedef TrtSpdkIO            IO;
	typedef TrtSched             Sched;
	typedef TrtEpollNet          Net;
};

class RuntimeTrtSpdkArray {
public:
	typedef rwlock_pagefault_trt   RwpfTy;
	typedef TrtLock                LockTy;
	typedef TrtSpdkArrayIO         IO;
	typedef TrtSched               Sched;
	typedef TrtEpollNet            Net;
};
class RuntimePosixSpdk {
public:
	typedef rwlock_pagefault     RwpfTy;
	typedef PthreadLock          LockTy;
	typedef SpdkIO               IO;
	typedef PthreadSched         Sched;
	typedef SocketNet            Net;
};

// memcache backends

class RuntimeTrtSpdkArrayMC {
public:
	typedef rwlock_pagefault_trt   RwpfTy;
	typedef TrtLock                LockTy;
	typedef TrtSpdkArrayIO         IO;
	typedef TrtSched               Sched;
	typedef MemcacheTrtNet        Net;
};
#endif // UDEPOT_TRT_SPDK

class RuntimePosixODirectMC {
public:
	typedef rwlock_pagefault     RwpfTy;
	typedef PthreadLock          LockTy;
	typedef FileIODirect         IO;
	typedef PthreadSched         Sched;
	typedef MemcacheNet         Net;
};

class RuntimeTrtMC {
public:
	typedef rwlock_pagefault_trt RwpfTy;
	typedef TrtLock              LockTy;
	typedef TrtFileIO            IO;
	typedef TrtSched             Sched;
	typedef MemcacheTrtNet       Net;
};

class RuntimeTrtUringMC {
public:
	typedef rwlock_pagefault_trt RwpfTy;
	typedef TrtLock              LockTy;
	typedef TrtFileIOUring       IO;
	typedef TrtSched             Sched;
	typedef MemcacheTrtNet       Net;
};

} // end udepot namespace

#endif //_UDEPOT_BACKEND_HH_
