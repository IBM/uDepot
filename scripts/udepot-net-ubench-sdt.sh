#!/bin/bash
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
#           Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
#
#!/bin/bash

# Simple wrapper to run udepot-net-ubench with static tracepoints (sdt) enabled

BINFILE=test/uDepot/udepot-net-ubench
make $BINFILE

sudo READELF=$(which readelf) PERF=$(which perf) ./trt/scripts/sdt_reset.sh  $BINFILE
sudo $(which perf) record -dT -e 'sdt_udepot:*' $BINFILE $@
