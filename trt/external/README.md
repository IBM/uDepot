# Build

For our tests, we apply the patches found in this directory. You might want to do the same.


```
make -j -C dpdk install T=x86_64-native-linuxapp-gcc DESTDIR=. CONFIG_RTE_BUILD_COMBINE_LIBS=y MODULE_CFLAGS="-Wno-implicit-fallthrough" EXTRA_CFLAGS="-msse4.2 -fPIC"
make -j -C spdk -j DPDK_DIR=$(pwd)/dpdk/x86_64-native-linuxapp-gcc
```
