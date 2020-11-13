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

#include "KV.hh"
#include "util/debug.h"
#include "util/types.h"
#include "uDepot.hh"
#include "uDepotSalsa.hh"
#include "uDepot/uDepotDirectoryMap.hh"
#include "uDepot/uDepotSalsaMD.hh"
#include "uDepotIO.hh"
#include "udepot-utests.hh"
#include "frontends/usalsa++/Scm.hh"

namespace udepot {

uDepotDirectoryMap *map_g;
uDepotIO *udpt_io_g;

static int setup(const char *fname, uDepotIO::Ty io_type)
{
	udpt_io_g = uDepotIO::getUDepotIO(io_type);
	int rc = udpt_io_g->open(fname, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	assert(0 == rc);
	dev_size = udepot_io_m->get_size();
	return 0;
}

int uDepotDirectoryMapTest(const int argc, char *argv[])
{
	int rc = 0;
	rc = setup_test("/dev/shm/_udepot_dirmap_test", uDepotIO::Ty::FileIO);
	assert(0 == rc);
}
};
