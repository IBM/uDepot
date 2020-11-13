#!/bin/bash
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
#

if [ -z "$1" ]; then
	echo "Usage: $0 <perf.data file>"
	exit 1
fi

for fname in $*; do
	echo $fname
	for event in $(sudo $(which perf) script -i $fname | awk '{print $5}' | cut -d':' -f2 | sort -u); do
		echo -n " $event:"
		sudo $(which perf) script -i $fname | awk "match(\$0,/$event.*arg1=(\S+)$/,ticks) { print ticks[1] }" | qtree-percentiles -p 0.5 -p 0.99 -p 0.999 --qtree_compr_limit 18 | sed -e 's/^/  /'
	done
done
