.PHONY: all submodules_ok build_trt

# Build configuration parameters.
BUILD_SPDK           ?= 0
BUILD_JNI            ?= 1
BUILD_SDT            ?= 0
BUILD_FPIC           ?= 1
BUILD_URING          ?= 1
# BUILD_TYPE options: DEBUG, NORMAL, PERFORMANCE
BUILD_TYPE           ?= NORMAL
USE_TCMALLOC         ?= 0

SHELL = /bin/bash
LIBCITYHASH_DIR  := external/cityhash
TRT_DIR          := trt
SALSA_DIR        := salsa
JNI_DIR          := src/uDepot/jni
JNI_TEST_DIR     := test/jni

INCLUDES    = -Isrc/include/                \
              -I$(SALSA_DIR)/src/include    \
              -I$(TRT_DIR)/src/             \
              -I$(LIBCITYHASH_DIR)/..
ifeq (1, $(BUILD_URING))
	INCLUDES += -Itrt/external/liburing/src/include
endif

CXX        ?= g++
CXXFLAGS   += -Wall -Werror -std=c++11 $(INCLUDES)
ifeq (DEBUG,$(BUILD_TYPE))
CXXFLAGS   += -O0 -ggdb
else ifeq (NORMAL,$(BUILD_TYPE))
CXXFLAGS   += -O2 -g
else ifeq (PERFORMANCE,$(BUILD_TYPE))
CXXFLAGS   += -O3 -DNDEBUG#-flto
else
$(error "Unknown BUILD_TYPE: --->$(BUILD_TYPE)<---")
endif

#CXXFLAGS  += -fconcepts
LIBS        = -lpthread -lrt
LIBS       += -lcityhash -lz                                 \
              -L$(LIBCITYHASH_DIR)/src/.libs/                \
              -Xlinker -rpath=$(LIBCITYHASH_DIR)/src/.libs/
LDFLAGS    += -Wl,--build-id

# Enable static profiling
ifeq (1,$(BUILD_SDT))
	CXXFLAGS   += -DUDEPOT_CONF_SDT
endif

udepot_SRC = src/uDepot/udepot.cc                            \
             src/uDepot/kv-conf.cc			     \
             src/uDepot/kv-factory.cc                        \
             src/uDepot/udepot-lsa.cc                        \
             src/uDepot/lsa/udepot-directory-map.cc          \
             src/uDepot/lsa/udepot-map.cc                    \
             src/uDepot/lsa/metadata.cc                      \
             src/uDepot/io/file-direct.cc                    \
             src/uDepot/io/trt-aio.cc                        \
             src/uDepot/io/trt-uring.cc                        \
             src/uDepot/rwlock-pagefault.cc                  \
             src/uDepot/rwlock-pagefault-trt.cc              \
             src/uDepot/net.cc                               \
             src/uDepot/net/socket.cc                        \
             src/uDepot/net/memcache.cc                      \
             src/uDepot/net/trt-memcache.cc                  \
             src/uDepot/net/trt-epoll.cc                     \
             src/uDepot/net/mc-helpers.cc                    \
             src/uDepot/net/helpers.cc                       \
             src/uDepot/net/connection.cc                    \
             src/uDepot/net/serve-kv-request.cc              \
             src/uDepot/socket-peer-conf.cc                  \
             src/uDepot/mbuff.cc                             \
             src/uDepot/stats.cc                             \
             src/uDepot/thread_id.cc                         \

JAVAC := $(shell command -v javac 2> /dev/null)
ifndef JAVAC
BUILD_JNI=0
endif

ifeq (1, $(BUILD_SPDK))
ifeq (1, $(BUILD_JNI))
$(info Disabling JNI, not compatible with spdk build)
BUILD_JNI=0
endif
endif

ifeq (1, $(BUILD_JNI))
$(info Disabling tcmalloc, not compatible with JNI build)
	# tcmalloc does not work with JNI: See Caveats paragraph at http://goog-perftools.sourceforge.net/doc/tcmalloc.html
	USE_TCMALLOC = 0
	BUILD_FPIC = 1
endif

ifeq (1,$(BUILD_FPIC))
CXXFLAGS   += -fPIC
LDFLAGS    += -fPIC
endif

ifeq (1,$(USE_TCMALLOC))
        LIBS       += -ltcmalloc
        CXXFLAGS   += -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free
endif

ifeq (1, $(BUILD_URING))
	LIBS += -Ltrt/external/liburing/src -luring
endif

DPDK_INC   = -I$(TRT_DIR)/external/dpdk/dpdk/include/dpdk
SPDK_INC   = $(DPDK_INC) -I$(TRT_DIR)/external/spdk/include
SPDK_LIBS  = $(TRT_DIR)/external/spdk/build/lib/libspdk_nvme.a     \
             $(TRT_DIR)/external/spdk/build/lib/libspdk_util.a     \
             $(TRT_DIR)/external/spdk/build/lib/libspdk_log.a      \
             $(TRT_DIR)/external/spdk/build/lib/libspdk_env_dpdk.a \
             -L$(TRT_DIR)/external/dpdk/dpdk/lib \
             -Wl,-rpath=$(TRT_DIR)/external/dpdk/dpdk/lib \
             -lrte_eal -lrte_mempool -lrte_ring \
             -ldl -lrt

LIBCITYHASH_LIB       := $(LIBCITYHASH_DIR)/src/.libs/libcityhash.a
LIBUSALSA_OBJ         := $(SALSA_DIR)/src/frontends/usalsa++/build/libusalsa++.o

ifeq (1, $(BUILD_SPDK))
	CXXFLAGS              += -msse4
	LIBTRT_OBJ            := $(TRT_DIR)/build/libtrt-rte.o
else
	LIBTRT_OBJ            := $(TRT_DIR)/build/libtrt.o
endif

TESTS = bin/udepot-test             \
        test/uDepot/udepot-utests           \
        test/uDepot/udepot-net-ubench       \
        test/uDepot/Mbuff-test                     \
        test/uDepot/io-helpers              \


MC_SERVER = bin/udepot-memcache-server
MC_TEST = test/uDepot/memcache/udepot-memcache-test
LIBUDEPOT = libudepot.a
LIBPYUDEPOT = python/pyudepot/libpyudepot.so

ifeq (1, $(BUILD_SPDK))
      CXXFLAGS  += -DUDEPOT_TRT_SPDK
      CXXFLAGS  += $(SPDK_INC)
      LIBS      += $(SPDK_LIBS)
      LIBS      += $(SPDK_LIBS)
      # DPDK and SPDK is not build with -fPIC, required by JNI. Dont build JNI
      # if we are building SPDK (at least for now). There are probably other
      # things that wouldn't work in JNI+TRT_SPDK (e.g., threading vs tasks
      # model).

      # add files that depend on SPDK
      udepot_SRC += src/uDepot/io/trt-spdk.cc               \
                    src/uDepot/io/trt-spdk-array.cc         \
                    src/uDepot/io/spdk.cc                   \

endif


ALL = $(TESTS) $(MC_SERVER) $(MC_TEST) $(LIBUDEPOT)

ifeq (1, $(BUILD_JNI))
	ALL   += uDepotJNITest
	ALL   += $(JNI_DIR)/libuDepotJNI.so
	JNI_OBJ = $(udepot_jni_OBJ) $(udepot_OBJ) $(LIBUSALSA_OBJ)
	ifeq (1, $(BUILD_SPDK))
		JNI_OBJ += $(LIBTRT_OBJ)
	endif
endif

all: submodules_ok $(ALL)

ifeq (1, $(BUILD_SPDK))
submodules_ok: $(LIBCITYHASH_DIR)/configure  $(TRT_DIR)/external/dpdk/Makefile  $(TRT_DIR)/external/spdk/configure
else
submodules_ok: $(LIBCITYHASH_DIR)/configure
endif

.deps/%.d: %.cc
	@mkdir -p $(dir $@)
	@echo DEPS: $<
	@set -e; $(CXX) $(CXXFLAGS) -MM -MP $< -MT $(patsubst %.cc, %.o, $<) $@ > $@ 2>/dev/null

$(LIBCITYHASH_DIR)/configure:
	@echo "It seems that you have not checked out libcityhash"
	@echo "Please checkout the cityhash submodule (see README for more details)"
	@exit 1

IS_PPC64 := $(shell uname -m | grep ppc64 2> /dev/null)
IS_SSE42 := $(shell cat /proc/cpuinfo  |grep flags|tail -n1|grep sse4_3| cat /proc/cpuinfo  |grep flags|tail -n1|grep sse4_2 2> /dev/null)
ifdef IS_PPC64
LIBCITYHASH_CONFIGURE_ARGS=--host=ppc64-linux --build=ppc64-linux
else ifdef IS_SSE42
LIBCITYHASH_CONFIGURE_ARGS=--enable-sse4.2
LIBCITYHASH_MAKE_ARGS=all check CXXFLAGS="-g -O3 -msse4.2"
endif
ifeq (1,$(BUILD_FPIC))
LIBCITYHASH_CONFIGURE_ARGS+= "--with-pic"
endif

$(LIBCITYHASH_DIR)/Makefile: $(LIBCITYHASH_DIR)/configure
	cd $(LIBCITYHASH_DIR) && ./configure $(LIBCITYHASH_CONFIGURE_ARGS)

$(LIBCITYHASH_DIR)/src/.libs/libcityhash.a:  $(LIBCITYHASH_DIR)/Makefile
	cd $(LIBCITYHASH_DIR) && make $(LIBCITYHASH_MAKE_ARGS)

$(TRT_DIR)/external/dpdk/Makefile:
	@echo "It seems that you have not checked out dpdk"
	@echo "Please checkout the dpdk submodule (see README for more details)"
	@exit 1

$(LIBUSALSA_OBJ): $(SALSA_DIR)/src/frontends/usalsa++/Makefile
	make -C $(SALSA_DIR)/src/frontends/usalsa++/ BUILD_TYPE=$(BUILD_TYPE)

$(TRT_DIR)/external/spdk/configure:
	@echo "It seems that you have not checked out spdk"
	@echo "Please checkout the spdk submodule (see README for more details)"
	@exit 1

ifeq (1,$(BUILD_SPDK))
build_spdk:
	$(MAKE) -C $(TRT_DIR) BUILD_SPDK=$(BUILD_SPDK) BUILD_PIC=$(BUILD_FPIC) BUILD_TYPE=$(BUILD_TYPE) BUILD_SDT=$(BUILD_SDT) BUILD_URING=$(BUILD_URING) build_spdk
endif

build_trt:
	$(MAKE) -C $(TRT_DIR) BUILD_SPDK=$(BUILD_SPDK) BUILD_PIC=$(BUILD_FPIC) BUILD_TYPE=$(BUILD_TYPE) BUILD_SDT=$(BUILD_SDT) BUILD_URING=$(BUILD_URING) builddirs
ifeq (1,$(BUILD_URING))
	$(MAKE) -C $(TRT_DIR) BUILD_SPDK=$(BUILD_SPDK) BUILD_PIC=$(BUILD_FPIC) BUILD_TYPE=$(BUILD_TYPE) BUILD_SDT=$(BUILD_SDT) BUILD_URING=$(BUILD_URING) build_uring
endif
	$(MAKE) -C $(TRT_DIR) BUILD_SPDK=$(BUILD_SPDK) BUILD_PIC=$(BUILD_FPIC) BUILD_TYPE=$(BUILD_TYPE) BUILD_SDT=$(BUILD_SDT) BUILD_URING=$(BUILD_URING) $(LIBTRT_OBJ:$(TRT_DIR)/%=%)

$(LIBTRT_OBJ): build_trt
	@true # dummy recipe, so that Makefile cannot be smart and deduce that $(LIBTRT_OBJ) cannot change (as it does with an empty recipe)

udepot_OBJ = $(patsubst %.cc, %.o, ${udepot_SRC})

udepot_test_SRC = test/uDepot/udepot-test.cc
udepot_test_OBJ = $(patsubst %.cc, %.o, ${udepot_test_SRC})

udepot_utests_SRC = test/uDepot/udepot-utests.cc test/uDepot/uDepotMapTest.cc
udepot_utests_OBJ = $(patsubst %.cc, %.o, ${udepot_utests_SRC})

udepot_memcache_SRC = test/uDepot/memcache/udepot-memcache-server.cc
udepot_memcache_OBJ = $(patsubst %.cc, %.o, ${udepot_memcache_SRC})

udepot_memcache_test_SRC = test/uDepot/memcache/udepot-memcache-test.cc
udepot_memcache_test_OBJ = $(patsubst %.cc, %.o, ${udepot_memcache_test_SRC})

udepot_net_ubench_SRC = test/uDepot/udepot-net-ubench.cc
udepot_net_ubench_OBJ = $(patsubst %.cc, %.o, ${udepot_net_ubench_SRC})

udepot_all_SRC = $(udepot_SRC)                     \
                 $(udepot_jni_SRC)                 \
                 $(udepot_test_SRC)                \
                 $(udepot_utests_SRC)              \
                 $(udepot_memcache_SRC)            \
                 $(udepot_memcache_test_SRC)       \
                 test/misc/inline_cache.cc         \
                 test/uDepot/io-helpers.cc         \
                 src/uDepot/lsa/udepot-dir-map-or.cc \
                 test/uDepot/uDepotDirMapORTest.cc \
                 python/wrapper/pyudepot.cc \

udepot_all_DEP = $(patsubst %.cc, .deps/%.d, ${udepot_all_SRC})

$(LIBUDEPOT): $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBTRT_OBJ) $(LIBCITYHASH_LIB)
	gcc-ar cr $@ $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBTRT_OBJ)
	strip --strip-debug $@

bin/udepot-test:  $(LIBUSALSA_OBJ) $(LIBTRT_OBJ) $(udepot_OBJ) $(udepot_test_OBJ) $(LIBCITYHASH_LIB) Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $(udepot_test_OBJ) $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBTRT_OBJ) $(LIBS) -o $@

bin/udepot-memcache-server:  $(LIBTRT_OBJ)  $(udepot_memcache_OBJ) $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBCITYHASH_LIB) Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $(udepot_memcache_OBJ) $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBTRT_OBJ) $(LIBS) -o $@

test/uDepot/memcache/udepot-memcache-test: $(LIBTRT_OBJ) $(udepot_memcache_test_OBJ) $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBCITYHASH_LIB) Makefile
	$(CXX) $(LDFLAGS) $(udepot_memcache_test_OBJ) $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBTRT_OBJ) $(LIBS) -o $@

test/uDepot/udepot-utests: $(LIBTRT_OBJ) $(udepot_utests_OBJ) $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBCITYHASH_LIB) Makefile
	$(CXX) $(LDFLAGS) $(udepot_utests_OBJ) $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBTRT_OBJ) $(LIBS) -o $@

test/uDepot/uDepotDirMapORTest: $(LIBUDEPOT) src/uDepot/lsa/udepot-dir-map-or.o test/uDepot/uDepotDirMapORTest.o Makefile
	$(CXX) $(LDFLAGS) src/uDepot/lsa/udepot-dir-map-or.o test/uDepot/uDepotDirMapORTest.o $(LIBUDEPOT) $(LIBS) -o $@

test/uDepot/udepot-net-ubench: $(LIBTRT_OBJ) $(udepot_net_ubench_OBJ) $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBCITYHASH_LIB)
	$(CXX) $(LDFLAGS) $^ $(LIBS) -o $@

test/uDepot/io-helpers:  $(LIBTRT_OBJ) test/uDepot/io-helpers.o $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBCITYHASH_LIB)
	$(CXX) $(LDFLAGS) $^ $(LIBS) -o $@

test/uDepot/Mbuff-test: src/uDepot/mbuff.cc src/include/uDepot/mbuff.hh ./src/include/util/inline-cache.hh
	$(CXX) -DMBUFF_TESTS $(CXXFLAGS) $(LDFLAGS) -UNDEBUG $< -o $@

test/misc/inline_cache: test/misc/inline_cache.cc
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(LIBS) $^ -o $@

# uDepot python

$(LIBPYUDEPOT): $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBTRT_OBJ) $(LIBCITYHASH_LIB) python/wrapper/pyudepot.o python/wrapper/pyudepot.hh
	$(CXX) -shared -Wl,-soname,$@ $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBTRT_OBJ) python/wrapper/pyudepot.o $(LIBS) -o $@
	strip --strip-debug $@

#
# uDepot JNI

#JAVA_DIR        = /usr/java/jdk1.8.0_60
#JAVA_DIR        = /usr/lib/jvm/java-8-openjdk-amd64
JAVA_DIR        = $(shell ./scripts/get_java_home.sh)
JNI_INCLUDES    = -I /usr/lib/jvm/default-java/include   \
                  -I $(JAVA_DIR)/include                 \
                  -I $(JAVA_DIR)/include/linux

JNI_LDFLAGS     = -fPIC -shared -z noexecstack -Wl,-soname,uDepotJNI.so
JNI_CXXFLAGS    = $(JNI_INCLUDES) -fPIC -lc -static

udepot_jni_SRC=$(JNI_DIR)/uDepotJNI.cc
udepot_jni_OBJ = $(patsubst %.cc, %.o, ${udepot_jni_SRC})
udepot_jni_DEP = $(patsubst %.cc, .deps/%.d, ${udepot_jni_SRC})

JNI_CLASSDIR=$(JNI_DIR)/classes
uDepotJNI_CLASSFILE=$(JNI_CLASSDIR)/com/ibm/udepot/uDepotJNI.class
uDepotJNI_C_HEADER=$(JNI_DIR)/com_ibm_udepot_uDepotJNI.h

$(uDepotJNI_CLASSFILE):  $(JNI_DIR)/uDepotJNI.java
	[ -d $(JNI_CLASSDIR) ] || mkdir $(JNI_CLASSDIR)
	javac -h $(JNI_DIR) -d $(JNI_CLASSDIR) $<

 # C Header is generated together with the classfile (javac -h)
 $(uDepotJNI_C_HEADER):  $(uDepotJNI_CLASSFILE)

.PHONY: uDepotJNI
.PHONY: uDepotJNITest

uDepotJNI: $(JNI_DIR)/libuDepotJNI.so
uDepotJNITest: $(uDepotJNI) test/jni/uDepotJNITest.class

test/jni/uDepotJNITest.class: $(uDepotJNI_CLASSFILE) $(JNI_TEST_DIR)/uDepotJNITest.java
	javac -cp  $(JNI_CLASSDIR) -d $(JNI_TEST_DIR) $(JNI_TEST_DIR)/uDepotJNITest.java

.deps/$(JNI_DIR)/%.d: $(JNI_DIR)/%.cc
	@# XXX: This is ugly, but adding a dependency to uDepotJNIJava leads to an infinite loop
	[ -d $(JNI_CLASSDIR) ] || mkdir $(JNI_CLASSDIR)
	javac -h $(JNI_DIR) -d $(JNI_CLASSDIR) $(JNI_DIR)/uDepotJNI.java
	@mkdir -p $(dir $@)
	@echo DEPS: $<
	@set -e; $(CXX) $(CXXFLAGS) $(JNI_CXXFLAGS) -MM -MP $< > $@

$(JNI_DIR)/%.o: $(JNI_DIR)/%.cc $(uDepotJNI_C_HEADER) Makefile
	$(CXX) $(CXXFLAGS) $(JNI_CXXFLAGS) -c $< -o $@

$(JNI_DIR)/libuDepotJNI.so: $(JNI_OBJ) Makefile
	$(CXX) $(LDFLAGS) $(JNI_LDFLAGS) $(udepot_jni_OBJ) $(udepot_OBJ) $(LIBUSALSA_OBJ) $(LIBTRT_OBJ) -o $@ $(LIBS)

do_run_test = echo -n "RUNNING TEST: $(1) ... ";           \
              errfile=`mktemp /tmp/udepot-log-XXXX.log`;   \
              $(1) 1>/dev/null 2>$$errfile;                \
              if [  $$? -ne 0 ]; then                      \
                  echo "FAILURE. ";                        \
                  cat $$errfile | sed -e 's/^/ stderr: /'; \
              else                                         \
                  echo "SUCCESS.";                         \
              fi;                                          \
              rm $$errfile

# run tests

udepot-gc-test: $(TESTS)
	rm -f /dev/shm/udepot-test
	@$(call do_run_test, bin/udepot-test -f /dev/shm/udepot-test --segment-size 262144 --size $$(((1048576+4096)*1024+1)) -w 180000 -r 180000 -t 1 --force-destroy --gc --grain-size 32 --val-size 3072)
	@$(call do_run_test, bin/udepot-test -f /dev/shm/udepot-test --segment-size 262144 --size $$(((1048576+4096)*1024+1)) -w 180000 -r 180000 -t 1 --gc --grain-size 32 --val-size 3072)
	rm -f /dev/shm/udepot-test

udepot-grow-test: $(TESTS)
	rm -f /dev/shm/udepot-test
	@$(call do_run_test, bin/udepot-test -f /dev/shm/udepot-test --segment-size 4096 --size $$(((1048576+4096)*1024+1)) -w 100000 -r 100000 -t 17 --thin --force-destroy --grain-size 32 --val-size 3072)
	@$(call do_run_test, bin/udepot-test -f /dev/shm/udepot-test --segment-size 4096 --size $$(((1048576+4096)*1024+1)) -w 100000 -r 100000 -t 23 --thin --grain-size 32 --val-size 3072)
	rm -f /dev/shm/udepot-test

ifndef JAVAC
run_jni_test: $(TESTS)
	@echo "Not running JNI test: no javac command found"

else
run_jni_test: $(TESTS) $(JNI_DIR)/libuDepotJNI.so uDepotJNITest
	rm -f /dev/shm/udepot-test
	@$(call do_run_test,LD_LIBRARY_PATH=$(JNI_DIR):$$LD_LIBRARY_PATH java -cp src/uDepot/jni/classes/:test/jni/ uDepotJNITest /dev/shm/udepot-test)
endif

udepot-memcache-test: $(MC_SERVER) $(MC_TEST)
	@$(call do_run_test, test/uDepot/memcache/unit-test.sh bin/udepot-memcache-server test/uDepot/memcache/udepot-memcache-test)

run_tests: $(TESTS)
	rm -f /dev/shm/udepot-test
	@$(call do_run_test,bin/udepot-test -f /dev/shm/udepot-test -w 1000 -r 1000 --size $$(((1048576+4096)*1024+1)) -t 1 --force-destroy --grain-size 32 --val-size 3072)
	@$(call do_run_test,bin/udepot-test -f /dev/shm/udepot-test -w 1000 -r 1000 -t 1 --del --grain-size 32 --val-size 3072)
	@$(call do_run_test,test/uDepot/udepot-utests -u)
	@$(call do_run_test,test/uDepot/Mbuff-test)
	@$(call do_run_test,test/rwlock-pagefault/resizable_table)
	rm -f /dev/shm/udepot-test
	make udepot-grow-test
	make udepot-gc-test
	make run_jni_test
	make udepot-memcache-test

run_trt_tests: $(TESTS)
	rm -f /tmp/udepot-test-XXXX
	@$(call do_run_test,bin/udepot-test -u 5 -f /tmp/udepot-test-XXXX -w 1000 -r 1000 --size $$(((1048576+4096)*1024+1)) -t 1 --force-destroy --grain-size 512)
	@$(call do_run_test,bin/udepot-test -u 5 -f /tmp/udepot-test-XXXX -w 1000 -r 1000 -t 1 --del --grain-size 512)
	rm -f /tmp/udepot-test-XXXX

run_pyudepot_test: python/test-pyudepot.py $(LIBPYUDEPOT)
	@$(call do_run_test, LD_LIBRARY_PATH=python/pyudepot/:$$LD_LIBRARY_PATH PYTHONPATH=python/:$$PYTHONPATH python3 python/test-pyudepot.py)

%.o: %.cc Makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@

lclean:
	rm  -f $(TESTS) test/jni/uDepotJNITest.class
	rm  -f $(udepot_OBJ)
	rm  -f $(udepot_test_OBJ)
	rm  -f $(udepot_trt_test_OBJ) $(udepot_trt_test_DEP)
	rm  -f $(udepot_utests_OBJ)
	rm  -f $(MC_SERVER) $(MC_TEST)
	rm  -f $(udepot_memcache_OBJ)
	rm  -f $(udepot_memcache_test_OBJ)
	rm  -f $(JNI_DIR)/libuDepotJNI.so $(udepot_jni_OBJ) $(udepot_jni_DEP)
	rm -rf $(JNI_CLASSDIR)
	rm  -f $(uDepotJNI_C_HEADER)
	rm  -f $(LIBUDEPOT)
	rm  -f $(LIBPYUDEPOT)
	rm  -f $(udepot_all_DEP)
	rm  -f $(udepot_net_ubench_OBJ)
	rm  -f scripts/docker/udepot-memcache-server
	rm  -f scripts/docker/libcityhash.so.0
	rm  -f src/uDepot/lsa/udepot-dir-map-or.o
	rm  -f test/uDepot/io-helpers.o
	rm  -f test/uDepot/uDepotDirMapORTest test/uDepot/uDepotDirMapORTest.o
	rm  -f python/wrapper/pyudepot.o
	make -C $(TRT_DIR) clean;

clean: lclean
	make -C $(SALSA_DIR)/src/frontends/usalsa++/ clean
	make -C $(LIBCITYHASH_DIR) distclean || true
	rm   -f $(LIBCITYHASH_DIR)/Makefile

#-include $(udepot_trt_test_DEP)
ifneq ($(MAKECMDGOALS),clean)
-include $(udepot_all_DEP)
endif


.PHONY: docker-package

docker-package: $(MC_SERVER) $(LIBCITYHASH_LIB)
	cp external/cityhash/src/.libs/libcityhash.so.0 scripts/docker/
	cp $(MC_SERVER) scripts/docker/
	docker build --tag "udepot-ubuntu-18.04:`git rev-parse --short HEAD`" scripts/docker
