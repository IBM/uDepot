# uDepot Key Value Store

[![Build Status](https://travis-ci.com/IBM/uDepot.svg?branch=main)](https://travis-ci.com/IBM/uDepot)

A multi-threaded, scalable, persistent store that is flash
optimized by using a log-structured space allocation and GC framework.

It uses a two-level directory map table as the main data structure
that grows together with the data and will utilize as much capacity as
possible before returning out of space. It currently persists both at
graceful shutdown but also on power loss (see Note at end for details
on crash recovery).

See our [FAST19 paper](https://www.usenix.org/system/files/fast19-kourtis.pdf) for more details on uDepot. The log-structured space
allocation and GC is described in our [MASCOTS18 paper](https://ieeexplore.ieee.org/document/8526893).

## Install

You should have a C++11 compatible compiler (any gcc version >=
4.8.1), and jdk installed. In Ubuntu, install with:
```
$ apt-get install build-essential default-jdk -y
```

Dependencies: boost, zlib, tcmalloc. In Ubuntu install with:
```
$ apt-get install libboost-dev zlib1g-dev libgoogle-perftools-dev -y
```


After git clone, you should execute the following commands:

```
$ git submodule init
$ git submodule update
$ make
```

To build with SPDK support you can use the default helper build target
build_spdk before building uDepot:
```
BUILD_SPDK=1 make build_spdk
BUILD_SPDK=1 make
```

## C++ usage example

```
#include "kv.hh"
#include "uDepot/kv-conf.hh"
#include "uDepot/kv-factory.hh"

...
{
...
	// this will use the default configuration
	udepot::KV_conf conf ("/dev/nvme0n1");
	// you can also manually set parameters if you want (or parse from argv, argc)
	// and use a test file instead of a block device, like below
	/*
	udepot::KV_conf conf (
		"/tmp/udepot-test", /* file path */
		(1UL << 30) + 20UL, /* file size, if file doesn't exist it will create one */
		true,               /* do not try to restore data (force destroy) */
		32,                 /* grain_size, in bytes */
		4096                /* segment size, in # grains */ );
	*/

	/*
	Alternatively, one can also manually set the configuration
	parameters that are not exposed in the constructor. Most
	notably one can change the capacity set aside for the GC
	process as overprovisioning, which by default is 20%.e.g.,
	setting it to 10%:

            udepot::KV_conf conf ("/dev/nvme0n1");
	    conf.overprovision_m = 100; // overprovisioning in 1/1000 over entire capacity.
	                                // a value of 100 -> 10% of capacity set aside
	    conf.validate_and_sanitize_parameters(); // good idea to call validation and sanitization
	                                             // after manually setting conf settings and before
						     // supplying conf to KV factory
	*/

	KV *const KV = udepot::KV_factory::KV_new(conf);
	assert(nullptr != KV);
	int rc = KV->init();
	assert(0 == rc);

	uint8_t key[31] = { 0 };
	key[0] = 0xBE;
	key[1] = 0xEF;
	uint8_t val[3078] = { 0 };
	rc = KV->put(key, sizeof(key), val, sizeof(val));
	assert(0 == rc);

	uint8_t val_out[3078];
	size_t val_size_read, val_size;
	rc = KV->get(key, sizeof(key), val_out, sizeof(val_out), val_size_read, val_size);
	assert(0 == rc);
	assert(0 == memcmp(val, val_out, sizeof(val)));
	assert(val_size_read == val_size);

	rc = KV->shutdown();
	assert(0 == rc);
	delete KV;
...
}
```

Notes: Add src/include to the list of directories searched for header
files, and link your external application against libudepot.a

## Python API usage

Python API uses keys and values provided as numpy arrays

Build libpyudepot.so
```
make python/pyudepot/libpyudepot.so
```

Minimal usage example:
```
from pyudepot import uDepot
import numpy as np

key=b'Key1\xff'
knp=np.frombuffer(key, dtype=np.uint8)
val=b'Val\xDE\xAD\xBE\xEF'
vnp=np.frombuffer(val, dtype=np.uint8)
kv_params={}

kv_params['file_name']='/dev/nvme0n1'
kv=uDepot(**kv_params)

ret = kv.put(knp, vnp)
val_out=np.empty(vnp.size,dtype=np.uint8)
ret = kv.get(knp, val_out)
```

## Java JNI API usage

Example usage using test/jni/uDepotJNITest.java:

Builds with `make` or `make uDepotJNITest`, which does
```
$ javac -cp src/uDepot/jni/classes -d test/jni test/jni/uDepotJNITest.java
```

Runs with:
```
$ java -cp src/uDepot/jni/classes/:test/jni/ uDepotJNITest
```

Note that you have to have libuDepotJNI.so (which builds with `make`
or `make src/uDepot/libuDepotJNI.so`) in the `LD_LIBRARY_PATH` path:
```
$ export LD_LIBRARY_PATH=$(pwd)/src/uDepot/jni/:$LD_LIBRARY_PATH
```

Another example can be found in the YCSB uDepot binding implementation
under:
`ycsb-binding/udepot/src/main/java/com/yahoo/ycsb/db/uDepotClient.java`

## Tests

A multithreaded, multi-client example use case can be found at
`test/uDepot/udepot-test.cpp` which also does data verification
tests. `bin/udepot-test -h` for options
Example run:
```
$ bin/udepot-test -f /dev/nvme0n1 -w 15000000 -r 15000000 -t 3 --thin --force-destroy
```

Unit tests can be found in `test/uDepot/udepot-utests.cc`.

JNI test in `test/jni/uDepotJNITest.java`.

## Notes

- Crash recovery: Data should be always recoverable from the uDepot
  data log when using a block device that is "enterprise" grade and
  has battery (or otherwise) protected data buffers), i.e., a REQ_FUA
  (e.g., for writes on fds opened with O_SYNC) will return
  immediately. Most datacenter-grade NVMe drives offer this.
  Otherwise, when using block devices that don't have this feature OR
  when using uDepot on top of a FileSystem, data integrity is _not_
  guaranteed in the case of a crash.

- Best performance is expected when using the trt spdk (or aio)
  subsystems, which require using trt from the client (see
  udepot-memcache-server.cc as an example of building a trt-based
  service on top of uDepot with the memcache protocol).

- Performance tests:
  Build with BUILD set to PERFORMANCE  (e.g., `BUILD=PERFORMANCE make -j20`).
  Use trt, spdk backend, and use zero copy on the client side. Examples udepot-test:
  aio (-u 5):
  `sudo bin/udepot-test -f /dev/nvme0n1 -w 1000000 -r 1000000 -t 20 --grain-size 4096 --val-size 3072 -u 5 --thin --zero-copy`
  io_uring (-u 6):
  `sudo bin/udepot-test -f /dev/nvme0n1 -w 1000000 -r 1000000 -t 20 --grain-size 4096 --val-size 3072 -u 6 --thin --zero-copy`
  spdk (-u 7):
  `sudo bin/udepot-test -f PHKS73350074375AGN,PHKS7335009N375AGN -w 10000000 -r 10000000 -t 20 --grain-size 4096 --val-size 3072 -u 8 --thin --zero-copy`

- JNI does not currently work with SPDK backend due to
  incompatibilites with dpdk version only being tested with static
  build

## License

This project is licensed under the BSD 3-Clause License.
If you would like to see the detailed LICENSE click [here](LICENSE).

## Contributing

Please see [CONTRIBUTING](CONTRIBUTING.md) for details.
Note that this repository has been configured with the [DCO bot](https://github.com/probot/dco).
