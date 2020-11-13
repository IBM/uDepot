#!/bin/bash
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
#

# Simple wrapper to run udepot-test with static tracepoint (sdt) enabled

BINFILE=build/tests/net_echo_client
#make $BINFILE

sudo READELF=$(which readelf) PERF=$(which perf) ./scripts/sdt_reset.sh  $BINFILE
sudo $(which perf) record -e 'sdt_trt:*' $BINFILE $@
