#!/bin/bash
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
#           Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
#

rm -f /tmp/udepot-mc-test
rm -f /tmp/udepot-mc-client-test
udpt_mc_server=$1
udpt_mc_test=$2
$udpt_mc_server -u 10 -t 1 -s $((1024*1024*1024 + 4096)) --segment-size 1024 --grain-size 4096 --force-destroy --server-conf localhost,$((0)) -f /tmp/udepot-mc-test &
serverpid=$!
$udpt_mc_test  -m localhost -f /tmp/udepot-mc-client-test --noexpire
kill -INT $serverpid
wait
rm -f /tmp/udepot-mc-test
rm -f /tmp/udepot-mc-client-test
