#!/bin/bash
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
#           Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
#

PERF="${PERF:-perf}"

$PERF record -dT \
	-e syscalls:sys_enter_io_submit     \
	-e syscalls:sys_exit_io_submit      \
	-e syscalls:sys_enter_io_getevents  \
	-e syscalls:sys_exit_io_getevents   \
	$*
