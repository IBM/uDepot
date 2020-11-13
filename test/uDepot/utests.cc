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
#include "util/debug.h"
#include "util/types.h"

extern int uDepotDirectoryMapTest();

int main(const int argc, char *argv[])
{
	int rc __attribute__((unused)) = 0;

	rc = uDepotDirectoryMapTest(argc, argv);
	assert(0 == rc);

	return 0;
}
