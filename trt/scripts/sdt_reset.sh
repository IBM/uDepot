#!/bin/bash
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
#
#!/bin/bash

PERF="${PERF:-perf}"
READELF="${READELF:-readelf}"

#set -x

if [ -z "$1" ]; then
    echo "Usage: $0 <binary_file>"
    exit 0
fi

fname=$(realpath "$1")
xdir=$(realpath $(dirname $0))

$PERF buildid-cache --remove $fname
$PERF buildid-cache --add $fname

# NB: In some cases (e.g., for probes inside templates), I cannot find a way to
# avoid having multiple events with the same name. Fortunately, perf probe is
# smart enough to create multiple events for each name. --kkourt
for probe in $(READELF=$READELF ${xdir}/sdt_get.pl $fname | sort -u); do
    $PERF probe -d "sdt_$probe"
    # remove all events with the same name
    for p in $($PERF probe -l 2>/dev/null | grep "sdt_$probe"  | awk '{ print $1 }'); do
        $PERF probe -d $p
    done
    $PERF probe "sdt_$probe"
done
