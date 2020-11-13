#!/bin/bash
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
#           Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
#

FNAME="$(hostname).dat"

/udepot/udepot-memcache-server -u 10 -t 1 -s $((1024*1024*1024 + 4096)) --segment-size 1024 --grain-size 4096 --force-destroy --server-conf localhost -f /udepot-data/$FNAME
