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

#ifndef	_UDEPOT_SYNC_H_
#define	_UDEPOT_SYNC_H_

#include <pthread.h>

#include "trt/uapi/trt.hh"
#include "util/debug.h"

/**
 * Synchronization backend for uDepot
 *  - locks
 *  - lock tables
 *  - read-write pagefault locks
 */

namespace udepot {

// Abstract base class:
class uDepotLock {
public:
	virtual void lock() = 0;
	virtual void unlock() = 0;

        u32 ref_cnt() const { return ref_cnt_m.load(); }
        u32 ref_cnt_dec_ret(u32 val) { return ref_cnt_m.fetch_sub(val); }

        uDepotLock(u32 ref_cnt):ref_cnt_m(ref_cnt) {}
	virtual ~uDepotLock() {}
private:
        char pad_[64];
        std::atomic<u32> ref_cnt_m;
};

class PthreadLock : public uDepotLock {
protected:
	pthread_mutex_t lock_m;
public:
	PthreadLock():uDepotLock(1) { pthread_mutex_init(&lock_m, NULL); }
	~PthreadLock() { pthread_mutex_destroy(&lock_m); }
	PthreadLock(PthreadLock const&)   = delete;
	void operator=(PthreadLock const&) = delete;

	void lock() override {
		int ret = pthread_mutex_lock(&lock_m);
		if (ret != 0) {
			UDEPOT_ERR("Error taking lock");
			abort();
		}
	}

	void unlock() override {
		int ret = pthread_mutex_unlock(&lock_m);
		if (ret != 0) {
			UDEPOT_ERR("Error releasing lock");
			abort();
		}
	}
};

class PthreadSpinLock : public uDepotLock {
public:
	PthreadSpinLock():uDepotLock(1) { pthread_spin_init(&lock_m, PTHREAD_PROCESS_SHARED); }
	~PthreadSpinLock() { pthread_spin_destroy(&lock_m); }
	PthreadSpinLock(PthreadSpinLock const&)   = delete;
	void operator=(PthreadSpinLock const&) = delete;

	void lock() override {
		for (;;) {
			const int ret = pthread_spin_trylock(&lock_m);
			switch (ret) {
			case 0:
				return;
			case EBUSY:
				pthread_yield();
				break;
			default:
				UDEPOT_ERR("Error taking lock");
				abort();
			}
		}
	}

	void unlock() override {
		const int ret = pthread_spin_unlock(&lock_m);
		if (ret != 0) {
			UDEPOT_ERR("Error releasing lock");
			abort();
		}
	}
protected:
	pthread_spinlock_t lock_m;
};

// NB: we can probably do better than the following implementation, but this
// should work for now.
class TrtLock : public uDepotLock {
	pthread_mutex_t lock_m;
public:
	TrtLock():uDepotLock(1) { pthread_mutex_init(&lock_m, NULL); }
	TrtLock(TrtLock const&)    = delete;
	void operator=(TrtLock const&) = delete;

	void lock() override {
		for (;;) {
			int ret = pthread_mutex_trylock(&lock_m);
			switch (ret) {
				case 0:
				return;

				case EBUSY:
				trt::T::yield();
				break;

				default:
				UDEPOT_ERR("Error taking lock");
				abort();
			}
		}
	}

	void unlock() override {
		int ret = pthread_mutex_unlock(&lock_m);
		if (ret != 0) {
			UDEPOT_ERR("Error releasing lock");
			abort();
		}
	}
};

} // end udepot namespace

#endif /* _UDEPOT_SYNC_H_ */
