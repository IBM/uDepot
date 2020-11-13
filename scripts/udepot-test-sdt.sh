#!/bin/bash
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
#           Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
#

# Simple wrapper to run udepot-test with static tracepoint (sdt) enabled

BINFILE=bin/udepot-test

set -e
set -x

EVENTS_UDEPOT="-e sdt_udepot:*"
#EVENTS_TRT="-e sdt_trt:* -e sdt_trt_spdk:* -e sdt_trt_epoll:*"
EVENTS="$EVENTS_UDEPOT"

#make $BINFILE

PERF_FILE="${PERF_FILE:-/tmp/perf.data}"

sudo READELF=$(which readelf) PERF=$(which perf) ./trt/scripts/sdt_reset.sh  $BINFILE
#sudo $(which perf) record -o ${PERF_FILE} -F 1000 -e 'sdt_udepot:*' $BINFILE $@
sudo $(which perf) record -F 100 -T -m 4096 -o ${PERF_FILE} $EVENTS $BINFILE $@
