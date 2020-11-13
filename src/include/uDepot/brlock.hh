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

// A big reader's lock, using RWLocks

#ifndef _UDEPOT_BRLOCK_
#define _UDEPOT_BRLOCK_

#include <array>
#include "uDepot/rwlock.hh"
#include "uDepot/thread-id.hh"

namespace udepot {

class BRLock {
	static const size_t LOCKS_NR = 128;
	static const size_t CACHELINE_SIZE = 128;
	struct Lock {
		RWLock rwlock;
		unsigned char padding[CACHELINE_SIZE - sizeof(RWLock)];

		Lock() : rwlock() {}
	};
	std::array<Lock, LOCKS_NR> locks_;

public:
	BRLock(BRLock const&) = delete;
	BRLock &operator=(const BRLock&) = delete;

	BRLock() : locks_() {};
	virtual ~BRLock() {};

	bool rd_try_lock(void) {
		unsigned tid = ThreadId::get() % LOCKS_NR;
		return locks_[tid].rwlock.rd_try_lock();
	}

	void rd_unlock(void) {
		size_t tid = ThreadId::get() % LOCKS_NR;
		locks_[tid].rwlock.rd_unlock();
	}

	void wr_enter(void) {
		for (size_t i=0; i < LOCKS_NR; i++) {
			locks_[i].rwlock.wr_enter();
		}
	}

	bool wr_ready(void) {
		for (size_t i=0; i < LOCKS_NR; i++) {
			if (!locks_[i].rwlock.wr_ready())
				return false;
		}
		return true;
	}

	void wr_exit(void) {
		for (size_t i=0; i < LOCKS_NR; i++) {
			locks_[i].rwlock.wr_exit();
		}
	}
};

} // end namespace udepot

#endif //  _UDEPOT_BRLOCK_
