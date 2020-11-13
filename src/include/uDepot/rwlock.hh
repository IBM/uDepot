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

// A read-write spinlock

#ifndef _UDEPOT_RWLOCK_
#define _UDEPOT_RWLOCK_

#include <atomic>

namespace udepot {

class RWLock {
protected:
	static constexpr s32 RWLOCK_BIAS = 0x01000000;
	// rwlock_:
	// > 0   -> readers
	// == 0  -> writer, no readers
	// < 0   -> writer and readers
	std::atomic<s32> rwlock_;

public:
	RWLock(RWLock const&) = delete;
	RWLock &operator=(RWLock const&) = delete;
	RWLock() : rwlock_(RWLOCK_BIAS) {};

	// Try to get a read lock. Returns true if successful.
	bool rd_try_lock(void) {
		// writer already here
		if (rwlock_ == 0)
			return false;

		const s32 old_val = rwlock_.fetch_sub(1);
		if (old_val > 0) {
			return true;
		} else {
			rwlock_.fetch_add(1);
			return false;
		}
	}

	void rd_unlock(void) {
		rwlock_.fetch_add(1);
	}

	void wr_enter(void) {
		rwlock_.fetch_sub(RWLOCK_BIAS);
	}

	bool wr_ready(void) {
		return rwlock_ == 0;
	}

	void wr_exit(void) {
		rwlock_.fetch_add(RWLOCK_BIAS);
	}

	virtual ~RWLock() {};
};


} // end namespace udepot


#endif // _UDEPOT_RWLOCK_

