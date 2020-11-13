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
#include <cassert>
#include <cstdlib>
#include <algorithm>

#include "frontends/usalsa++/SalsaCtlr.hh"
#include "frontends/usalsa++/Scm.hh"

int main(int argc, char *argv[])
{
	salsa::Scm scm;
	salsa::SalsaCtlr sc1, sc2;
	int err = scm.init(argc - 1, argv + 1);
	if (0 != err)
		return err;
	err = scm.init_threads();
	if (0 != err)
		return err;
	err = sc1.init(&scm, 0);
	if (0 != err)
		return err;
	err = sc2.init(&scm, 0);
	if (0 != err)
		return err;
	u64 size = sc1.get_size() / 3;
	u64 seg_size = sc1.get_seg_size();
	u64 grain_size = sc1.get_grain_size();
	size /= grain_size;
	assert(size);
	int i = 0;
	do {
		const u64 len = std::min(rand() % seg_size, size);
		salsa::SalsaCtlr *const sc = i++ % 2 ? &sc1 : &sc2;
		u64 start;
		int rc;
		do { rc = sc->allocate_grains(len, &start); } while (EAGAIN == rc);
		assert(0 == rc && SALSA_INVAL_GRAIN != start);
		sc->release_grains(start, len);
		size -= len;
	} while (size);
	assert(0 == scm.exit_threads());
	sc1.shutdown();
	sc2.shutdown();
	return 0;
}
