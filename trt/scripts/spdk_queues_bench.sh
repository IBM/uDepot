#!/bin/bash
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
#

RESULTS_DIR="/dev/shm/spdk_queue_runs"
BINFILE="build/benchs/spdk_queues"
declare -a OPTS=( "-c 1" "-c 256" "-c 1,256" "-c 1,256 -l 1" "-c 1 -c 256" "-c 1 -c 256 -l 1" "-c 1 -c 256 -s wrr")
#declare -a OPTS=("-c 256 -t 500000000000" "-c 256 -t 1000000000000")

make $BINFILE
sudo READELF=$(which readelf) PERF=$(which perf) ./scripts/sdt_reset.sh  $BINFILE

if [ -d $RESULTS_DIR ]; then
    mv $RESULTS_DIR ${RESULTS_DIR}-copy.$(date +%Y%m%d.%H%M%S)
fi

function run {
        if (($# != 2)); then
            echo "run: wrong number of arguments: $#"
            exit 1
        fi
        local opts=$1
        local outputdir=$2
        # parameters: $outputdir

        mkdir -p $outputdir
        echo "REPEAT: $r params: $opts"
        # -e sdt_spdk_queues:submit_op          \
        sudo $(which perf) record               \
            -F 1000                             \
            -e sdt_spdk_queues:complete_op      \
            -o $outputdir/perf.data             \
            $BINFILE $opts 1> $outputdir/stdout 2> $outputdir/stderr
}

mkdir -p $RESULTS_DIR
for r in $(seq 1 3); do
    def_params="-t 1000000000000"
   for qd in 1 16 64 256; do
        outputdir=$RESULTS_DIR/$(printf "qd%03g-alone.r%02g\n"  $qd $r)
        params="$def_params -c $qd"
        run "$params" "$outputdir"

        outputdir=$RESULTS_DIR/$(printf "qd%03g-shared.r%02g\n"  $qd $r)
        params="$def_params -c 1,$qd"
        run "$params" "$outputdir"

        outputdir=$RESULTS_DIR/$(printf "qd%03g-exclusive.r%02g\n"  $qd $r)
        params="$def_params -c 1 -c $qd"
        run "$params" "$outputdir"
   done;
done

