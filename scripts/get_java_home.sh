#!/bin/sh
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
#           Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
#
readlink -f $(dirname $(readlink -f $(which javac)))/../
