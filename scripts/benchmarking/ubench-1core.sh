#!/bin/bash
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
#

echo "device:$DEVNAME SN:$DEV_SN NUMA_NODE:$DEV_NODE CPUS:${DEV_CPUS[@]}"                  > $dname/device
sudo nvme list 2>&1 > $dname/nvme-devices
grep -H . /proc/sys/net/core/busy_poll /proc/sys/net/core/busy_read > $dname/net_busy_polling

function run_trt_aio {

	for trt_tasks in $QD; do
		rdir=$(printf "$dname/trt-aio-ntasks-%03g\n" $trt_tasks)
		mkdir $rdir

		cmd="sudo $PERF_RECORD_TRT_AIO -o $rdir/perf.data $TASKSET $UDEPOT_TEST -f $DEVNAME -u 5 -t 1 --trt-ntasks $trt_tasks $COMMON_OPTS"
		echo $cmd | tee $rdir/perf-cmd
		$cmd 2>&1 | tee $rdir/perf-log

		# NB: There seems to be a significant change in the measured throughput
		# even with when doing sampling (-F 1000). Do a run without profiling.
		cmd="sudo $TASKSET $UDEPOT_TEST -f $DEVNAME -u 5 -t 1 --trt-ntasks $trt_tasks $COMMON_OPTS"
		echo $cmd | tee $rdir/cmd
		$cmd 2>&1 | tee $rdir/log
	done

}


function run_linux_directio {

	for threads in $QD; do
		rdir=$(printf "$dname/linux-directio-nthreads-%03g\n" $threads)
		mkdir $rdir
		cmd="sudo $PERF_RECORD_POSIX_ODIRECT -o $rdir/perf.data $TASKSET $UDEPOT_TEST -f $DEVNAME -u 3 -t $threads $COMMON_OPTS"
		echo $cmd | tee $rdir/perf-cmd
		$cmd 2>&1 | tee $rdir/perf-log

		# NB: There seems to be a significant change in the measured throughput
		# even with when doing sampling (-F 1000). Do a run without profiling.
		cmd="sudo $TASKSET $UDEPOT_TEST -f $DEVNAME -u 3 -t $threads $COMMON_OPTS"
		echo $cmd | tee $rdir/cmd
		$cmd 2>&1 | tee $rdir/log
	done
}


function run_linux_buffered {

	for threads in $QD; do
		rdir=$(printf "$dname/linux-buffered-nthreads-%03g\n" $threads)
		mkdir $rdir
		cmd="sudo $PERF_RECORD_POSIX -o $rdir/perf.data $TASKSET $UDEPOT_TEST -f $DEVNAME -u 2 -t $threads $COMMON_OPTS"
		echo $cmd | tee $rdir/perf-cmd
		$cmd 2>&1 | tee $rdir/perf-log

		# NB: There seems to be a significant change in the measured throughput
		# even with when doing sampling (-F 1000). Do a run without profiling.
		cmd="sudo $TASKSET $UDEPOT_TEST -f $DEVNAME -u 2 -t $threads $COMMON_OPTS"
		echo $cmd | tee $rdir/cmd
		$cmd 2>&1 | tee $rdir/log
	done
}

#

function run_trt_spdk {
	for trt_tasks in $QD; do
		rdir=$(printf "$dname/trt-spdk-ntasks-%03g\n" $trt_tasks)
		mkdir $rdir
		cmd="sudo $PERF_RECORD_TRT_SPDK -o $rdir/perf.data $TASKSET_SPDK $UDEPOT_TEST -u 6 -f $DEV_SN -t 1 --trt-ntasks $trt_tasks $COMMON_OPTS"
		echo $cmd | tee $rdir/perf-cmd
		$cmd 2>&1 | tee $rdir/perf-log

		# NB: There seems to be a significant change in the measured throughput
		# even with when doing sampling (-F 1000). Do a run without profiling.
		cmd="sudo $TASKSET_SPDK $UDEPOT_TEST -u 6 -f $DEV_SN -t 1 --trt-ntasks $trt_tasks $COMMON_OPTS"
		echo $cmd | tee $rdir/cmd
		$cmd 2>&1 | tee $rdir/log
	done
}


function run_spdk_perf {

	SPDK_MASK=$(python -c "print hex(1<<${DEV_CPUS[0]})")
	for qd in $QD; do
		 for wload in randread write randwrite; do
			cmd="sudo ./trt/external/spdk/examples/nvme/perf/perf -c $SPDK_MASK -ll -L -s 4096 -q $qd  -t $((5*60)) -w $wload -r '$SPDK_DEV'"
			echo $cmd
			eval $cmd
		done
	done > $dname/spdk-perf-1core-latencies

	for qd in $QD; do
		 for wload in randread write randwrite; do
			cmd="sudo ./trt/external/spdk/examples/nvme/perf/perf -c $SPDK_MASK -s 4096 -q $qd  -t $((5*60)) -w $wload -r '$SPDK_DEV'"
			echo $cmd
			eval $cmd
		done
	done > $dname/spdk-perf-1core
}

COMMON_SRV_OPTS="-w 0 -r 0 --force-destroy --grain-size 32 --server-conf *:5555"
COMMON_CLI_OPTS="--grain-size 32 -u 2 -w $NWRS -r $NRDS --val-size 4000 -f /dev/shm/deleteme --force-destroy --thin --zero-copy -m $SRV_ADDR:5555"

#nix-copy-closure $CLI_ADDR $PERF_BINARY

function run_trt_aio_net {
	for qd in $QD; do
		rdir=$(printf "$dname/trt-aio-net-qd-%03g\n" $qd)
		mkdir $rdir

		echo $SRV_ADDR >$rdir/srv-addr
		echo $CLI_ADDR >$rdir/cli-addr

		for perf in 1 0; do
				if [ $perf == "1" ]; then
					cli_fname="$rdir/perf-cli"
					srv_fname="$rdir/perf-srv"
				else
					cli_fname="$rdir/cli"
					srv_fname="$rdir/srv"
				fi

				srv_cmd="sudo"
				#srv_cmd="$srv_cmd $PERF -o $rdir/perf-srv.data"
				srv_cmd="$srv_cmd $TASKSET"
				srv_cmd="$srv_cmd $UDEPOT_TEST -f $DEVNAME"
				srv_cmd="$srv_cmd -u 5"
				srv_cmd="$srv_cmd -t 1"
				srv_cmd="$srv_cmd $COMMON_SRV_OPTS"
				echo $srv_cmd  >     ${srv_fname}-cmd
				($srv_cmd 2>&1 | tee ${srv_fname}-log) &
				SRV_PID=$!
				sleep 1s

				cli_cmd="sudo"
				if [ $perf == "1" ]; then
					cli_cmd="$cli_cmd $PERF_RECORD_TRT_AIO -o $rdir/perf-cli.data"
				fi
				cli_cmd="$cli_cmd $UDEPOT_TEST"
				cli_cmd="$cli_cmd $COMMON_CLI_OPTS"
				cli_cmd="$cli_cmd -t $qd"
				echo $cli_cmd  >${cli_fname}-cmd

				ssh $CLI_ADDR "cd ~/wibm/src/udepot.git; $cli_cmd" 2>&1 1>${cli_fname}-log

				sudo pkill udepot-test
		done
	done
}

function run_trt_spdk_net {
	for qd in $QD; do
		rdir=$(printf "$dname/trt-spdk-net-qd-%03g\n" $qd)
		mkdir $rdir

		echo $SRV_ADDR >$rdir/srv-addr
		echo $CLI_ADDR >$rdir/cli-addr

		for perf in 1 0; do
				if [ $perf == "1" ]; then
					cli_fname="$rdir/perf-cli"
					srv_fname="$rdir/perf-srv"
				else
					cli_fname="$rdir/cli"
					srv_fname="$rdir/srv"
				fi

				srv_cmd="sudo"
				#srv_cmd="$srv_cmd $PERF -o $rdir/perf-srv.data"
				srv_cmd="$srv_cmd $TASKSET_SPDK"
				srv_cmd="$srv_cmd $UDEPOT_TEST -f $DEV_SN"
				srv_cmd="$srv_cmd -u 6"
				srv_cmd="$srv_cmd -t 1"
				srv_cmd="$srv_cmd $COMMON_SRV_OPTS"
				echo $srv_cmd  >     ${srv_fname}-cmd
				($srv_cmd 2>&1 | tee ${srv_fname}-log) &
				SRV_PID=$!
				sleep 1s

				cli_cmd="sudo"
				if [ $perf == "1" ]; then
					cli_cmd="$cli_cmd $PERF_RECORD_TRT_SPDK -o $rdir/perf-cli.data"
				fi
				cli_cmd="$cli_cmd $UDEPOT_TEST"
				cli_cmd="$cli_cmd $COMMON_CLI_OPTS"
				cli_cmd="$cli_cmd -t $qd"
				echo $cli_cmd  >${cli_fname}-cmd

				ssh $CLI_ADDR "cd ~/wibm/src/udepot.git; $cli_cmd" 2>&1 1>${cli_fname}-log

				sudo pkill udepot-test
		done
	done
}

run_linux_directio
run_trt_aio
#run_linux_buffered
#run_trt_aio_net

sudo NRHUGE=1024 trt/external/spdk/scripts/setup.sh

run_trt_spdk
run_spdk_perf

#run_trt_spdk_net
