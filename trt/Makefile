.PHONY: all clean builddirs

BUILD_SPDK   ?= 0
BUILD_RDMA   ?= 0
BUILD_PIC    ?= 1
BUILD_URING  ?= 1
USE_TCMALLOC ?= 0
BUILD_SDT    ?= 0 # tracing
BUILD_TYPE   ?= NORMAL
BUILD_DIR    ?= ./build

INCLUDES  = -Isrc

CC       ?= gcc
CXX      ?= g++
LD       ?= ld

# common for C/C++
CFLAGS_  += -Wall $(INCLUDES) -Wextra -Wno-unused-parameter -Wshadow -ggdb -D_GNU_SOURCE -D__STDC_FORMAT_MACROS
CFLAGS_  += -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free

ifeq (DEBUG,$(BUILD_TYPE))
CFLAGS_   += -O0
else ifeq (NORMAL,$(BUILD_TYPE))
CFLAGS_   += -O2
else ifeq (PERFORMANCE,$(BUILD_TYPE))
CFLAGS_   += -O3 -DNDEBUG
else
$(error "Unknown BUILD_TYPE: --->$(BUILD_TYPE)<---")
endif

#CFLAGS_ += #-fprofile-arcs -ftest-coverage
# C-only
CFLAGS    = $(CFLAGS_) -std=c11
# C++-only
CXXFLAGS  = $(CFLAGS_) -std=c++11
# linking-related
LIBS      =  -lpthread -lrt
LDFLAGS  +=

ifeq (1, $(BUILD_SDT))
        CFLAGS_   += -DTRT_CONFIG_SDT
        LDFLAGS   += -Wl,--build-id
endif

ifeq (1, $(BUILD_PIC))
      CFLAGS_ += -fPIC
endif

ifeq (1, $(BUILD_RDMA))
      LDFLAGS += -libverbs -lrdmacm
endif

ifeq (1, $(BUILD_URING))
	CXXFLAGS += -D_GNU_SOURCE -Iexternal/liburing/src/include
	LIBS += -Lexternal/liburing/src -luring
endif

build_DIR = $(BUILD_DIR)
src_DIR   = src

# transform source files (and only source files) by changing their suffix, and
# placing them into the output directory
transform_SRC = $(patsubst $(src_DIR)/%.cc, $(build_DIR)/$(2), $(filter %.cc, $(1))) \
                $(patsubst $(src_DIR)/%.c,  $(build_DIR)/$(2), $(filter %.c,  $(1))) \
                $(patsubst $(src_DIR)/%.S,  $(build_DIR)/$(2), $(filter %.S,  $(1)))


## jctx
jctx_src_DIR=$(src_DIR)/jctx
jctx_build_DIR=$(build_DIR)/jctx

jctx_SRC =                    \
 $(jctx_src_DIR)/jctx.c       \
 $(jctx_src_DIR)/jctx-asm.S   \

util_SRC =                   \
 $(src_DIR)/trt_util/net_helpers.cc  \
 $(src_DIR)/trt_util/cpumask_utils.c \

util_OBJ = $(call transform_SRC, $(util_SRC),%.o)
util_DEP = $(call transform_SRC, $(util_SRC),%.d)

trt_SRC = $(src_DIR)/trt/controller.cc     \
          $(src_DIR)/trt/future.cc         \
          $(src_DIR)/trt/scheduler.cc      \
          $(src_DIR)/trt/task_base.cc      \
          $(src_DIR)/trt/waitset.cc        \
          $(src_DIR)/trt/local_sync.cc     \
          $(src_DIR)/trt/uapi/trt.cc       \

trt_OBJ = $(call transform_SRC, $(trt_SRC),%.o)
trt_DEP = $(call transform_SRC, $(trt_SRC),%.d)

trt_backends_DIR=$(src_DIR)/trt_backends
trt_backends_SRC =                     \
 $(src_DIR)/trt_backends/trt_aio.cc        \
 $(src_DIR)/trt_backends/trt_epoll.cc      \

ifeq (1, $(BUILD_URING))
      trt_backends_SRC   +=  $(src_DIR)/trt_backends/trt_uring.cc
endif

jctx_TESTS =                  \
 $(jctx_build_DIR)/jctx-test-simple \
 $(jctx_build_DIR)/jctx-test-setjmp \
 $(jctx_build_DIR)/jctx-test        \


trt_TESTS_DISABLED = \
  $(build_DIR)/tests/trt_cache_slot                   \

trt_TESTS = \
  $(build_DIR)/tests/aio_preadv_pwritev               \
  $(build_DIR)/tests/trt_epoll_echo                   \
  $(build_DIR)/tests/trt_epoll_multi_threaded_server  \
  $(build_DIR)/tests/trt_hello_test                   \
  $(build_DIR)/tests/trt_spawn_fib                    \
  $(build_DIR)/tests/trt_spawn_test                   \
  $(build_DIR)/tests/trt_spawn_test_detached          \
  $(build_DIR)/tests/trt_spawn_test_yield             \
  $(build_DIR)/tests/trt_spawn_test_yield2            \
  $(build_DIR)/tests/net_echo_client                  \
  $(build_DIR)/tests/trt_local_single                 \
  $(build_DIR)/tests/trt_local_sync                   \
  $(build_DIR)/benchs/aio_bench                       \
  $(build_DIR)/benchs/trt_aio_bench                   \
  $(build_DIR)/benchs/trt_ubench                      \

trt_uring_TESTS = \
  $(build_DIR)/tests/uring_test                        \
  $(build_DIR)/benchs/trt_uring_bench                        \

ifeq (1, $(BUILD_URING))
      trt_TESTS   += $(trt_uring_TESTS)
endif

trt_spdk_TESTS = \
  $(build_DIR)/tests/spdk_test                        \
  $(build_DIR)/tests/trt_spdk_preadv_pwritev          \
  $(build_DIR)/tests/trt_spdk_test                    \
  $(build_DIR)/benchs/trt_spdk_ubench                 \
  $(build_DIR)/benchs/spdk_queues                     \

trt_spdk_TESTS_SRC = $(patsubst $(build_DIR)/%, $(src_DIR)/%.cc, $(trt_spdk_TESTS))
trt_spdk_TESTS_OBJ = $(patsubst %, %.o, $(trt_spdk_TESTS))

trt_rdma_TESTS = $(build_DIR)/tests/trt_rdma_echo

DPDK_INC   = -Iexternal/dpdk/build/include
SPDK_INC   = $(DPDK_INC) -Iexternal/spdk/include
SPDK_LIBS  = external/spdk/build/lib/libspdk_nvme.a     \
             external/spdk/build/lib/libspdk_util.a     \
             external/spdk/build/lib/libspdk_log.a      \
             external/spdk/build/lib/libspdk_env_dpdk.a \
             -lpciaccess \
             -Lexternal/dpdk/build/lib \
             -Wl,-rpath=external/dpdk/build/lib \
             -lrte_eal -lrte_mempool -lrte_ring \
             -ldl -lrt

spdk_SRC = $(trt_backends_DIR)/trt_spdk.cc  $(src_DIR)/trt_util/spdk.cc
spdk_OBJ = $(call transform_SRC, $(spdk_SRC),%.o)
spdk_DEP = $(call transform_SRC, $(spdk_SRC),%.d)

rdma_SRC = $(trt_backends_DIR)/trt_rdma.cc
rdma_OBJ = $(call transform_SRC, $(rdma_SRC),%.o)
rdma_DEP = $(call transform_SRC, $(rdma_SRC),%.d)


jctx_OBJ = $(call transform_SRC, $(jctx_SRC),%.o)
jctx_DEP = $(call transform_SRC, $(jctx_SRC),%.d)

trt_backends_OBJ = $(call transform_SRC, $(trt_backends_SRC),%.o)
trt_backends_DEP = $(call transform_SRC, $(trt_backends_SRC),%.d)

all_TESTS = $(jctx_TESTS) $(trt_TESTS)
ifeq (1, $(BUILD_SPDK))
      CFLAGS_          += -msse4
      all_TESTS        += $(trt_spdk_TESTS)
endif
ifeq (1, $(BUILD_RDMA))
      all_TESTS   += $(trt_rdma_TESTS)
endif

all_OBJ   = $(util_OBJ) $(jctx_OBJ) $(trt_OBJ) $(trt_backends_OBJ)
all_DEP   = $(util_DEP) $(jctx_DEP) $(trt_DEP) $(trt_backends_DEP)

all_OBJ  += $(patsubst %,%.o,$(all_TESTS))
all_DEP  += $(patsubst %,%.d,$(all_TESTS))

# top-level objects (incremental link)
top_OBJ = $(build_DIR)/jctx.o         \
	  $(build_DIR)/util.o         \
	  $(build_DIR)/trt-core.o     \
	  $(build_DIR)/trt-backends.o \
	  $(build_DIR)/libtrt.o


# we use a different lib so that we don't have to link everything with the spdk/dpdk libs
ifeq (1, $(BUILD_SPDK))
      top_OBJ += $(build_DIR)/libtrt-rte.o
endif

all_OBJ += $(top_OBJ)

test_DEP = \
 $(patsubst $(src_DIR)/%.c, $(build_DIR)/%.d,$(wildcard $(src_DIR)/tests/*.c))    \
 $(patsubst $(src_DIR)/%.cc,$(build_DIR)/%.d,$(wildcard $(src_DIR)/tests/*.cc))   \
 $(patsubst $(src_DIR)/%.c, $(build_DIR)/%.d,$(wildcard $(src_DIR)/benchs/*.c))   \
 $(patsubst $(src_DIR)/%.cc,$(build_DIR)/%.d,$(wildcard $(src_DIR)/benchs/*.cc))  \

all: builddirs $(all_OBJ) $(all_TESTS)


builddirs:
	[ -d $(build_DIR) ] || mkdir $(build_DIR)
	[ -d $(build_DIR)/jctx ] || mkdir $(build_DIR)/jctx
	[ -d $(build_DIR)/trt ] || mkdir $(build_DIR)/trt
	[ -d $(build_DIR)/trt/uapi ] || mkdir $(build_DIR)/trt/uapi
	[ -d $(build_DIR)/trt_util ] || mkdir $(build_DIR)/trt_util
	[ -d $(build_DIR)/trt_backends ] || mkdir $(build_DIR)/trt_backends
	[ -d $(build_DIR)/tests ] || mkdir $(build_DIR)/tests
	[ -d $(build_DIR)/benchs ] || mkdir $(build_DIR)/benchs

build_spdk:
	cd external/dpdk && git reset --hard && patch -p1 < ../dpdk.patch
	# cd external/dpdk && EXTRA_CFLAGS="-fPIC" make -j10 install T=x86_64-native-linuxapp-gcc DESTDIR=dpdk
	cd external/dpdk; make clean; make config T=x86_64-native-linuxapp-gcc && CONFIG_RTE_BUILD_COMBINE_LIBS=y MODULE_CFLAGS="-Wno-implicit-fallthrough -Wno-address-of-packed-member -Wno-stringop-overflow -Wno-incompatible-pointer-types" EXTRA_CFLAGS="-msse4.2 -fPIC" make -j10
	cd external/spdk && git reset --hard && patch -p1 < ../spdk.patch
	cd external/spdk; make clean; ./configure --with-dpdk=../dpdk && make -j DPDK_DIR=../dpdk/build

build_uring:
	cd external/liburing && CFLAGS="-fpic -fPIC" CPPFLAGS="-fpic -fPIC" LINK_FLAGS="-fpic -fPIC" make

$(build_DIR)/%.o: $(src_DIR)/%.cc
	$(CXX) $(CXXFLAGS) -c  $< -o $@
	$(CXX) $(CXXFLAGS) -MM $< -MT $@ -MF $(build_DIR)/$*.d

$(build_DIR)/%.o: $(src_DIR)/%.c
	$(CC) $(CFLAGS) -c  $< -o $@
	$(CC) $(CFLAGS) -MM $< -MT $@ -MF $(build_DIR)/$*.d

$(build_DIR)/%.o: $(src_DIR)/%.S
	$(CC) $(CFLAGS) -c  $< -o $@
	$(CC) $(CFLAGS) -MM $< -MT $@ -MF $(build_DIR)/$*.d

#JEMALLOC_PATH=jemalloc/install
#LIBS     += -L${JEMALLOC_PATH}/lib -Wl,-rpath,${JEMALLOC_PATH}/lib -ljemalloc
#LIBS     += -ljemalloc
#LIBS     += -ltcmalloc
ifeq (1,$(USE_TCMALLOC))
        LIBS       += -ltcmalloc
endif

$(build_DIR)/jctx.o: $(jctx_OBJ)
	$(LD) -i $^ -o $@

$(build_DIR)/util.o: $(util_OBJ)
	$(LD) -i $^ -o $@

$(build_DIR)/trt-core.o: $(trt_OBJ)
	$(LD) -i $^ -o $@

$(build_DIR)/trt-backends.o: $(trt_backends_OBJ)
	$(LD) -i $^ -o $@

$(build_DIR)/libtrt.o: $(build_DIR)/trt-core.o $(build_DIR)/trt-backends.o $(build_DIR)/util.o $(build_DIR)/jctx.o
	$(LD) -i $^ -o $@

$(build_DIR)/libtrt-rte.o: $(build_DIR)/libtrt.o $(spdk_OBJ)
	$(LD) -i $^ -o $@

$(jctx_build_DIR)/jctx_wr-test: $(jctx_src_DIR)/jctx_wr.cc $(jctx_src_DIR)/jctx_wr.hh $(jctx_build_DIR)/jctx.o $(jctx_build_DIR)/jctx-asm.o $(jctx_src_DIR)/jctx.h
	$(CXX) $(CXXFLAGS) -DJCTX_FN_TEST $(jctx_src_DIR)/jctx_wr.cc $(jctx_build_DIR)/jctx-asm.o  $(jctx_build_DIR)/jctx.o -o $@

$(jctx_build_DIR)/jctx-test-simple: $(jctx_build_DIR)/jctx-test-simple.o $(jctx_OBJ)
	$(CC) $^ $(LDFLAGS) $(LIBS) -o $@

$(jctx_build_DIR)/jctx-test-setjmp: $(jctx_build_DIR)/jctx-test-setjmp.o $(jctx_OBJ)
	$(CC) $^ $(LDFLAGS) $(LIBS) -o $@

$(jctx_build_DIR)/jctx-test: $(jctx_build_DIR)/jctx-test.o $(jctx_OBJ)
	$(CC) $^ $(LDFLAGS) $(LIBS) -o $@

# Objects that need spdk headers

$(spdk_OBJ): $(build_DIR)/%.o: $(src_DIR)/%.cc
	$(CXX) $(CXXFLAGS) $(SPDK_INC) -c $< -o $@
	$(CXX) $(CXXFLAGS) $(SPDK_INC) -MM $< -MT $@ -MF $(build_DIR)/$*.d

$(trt_spdk_TESTS_OBJ): $(build_DIR)/%.o: $(src_DIR)/%.cc
	$(CXX) $(CXXFLAGS) $(SPDK_INC) -c $< -o $@
	$(CXX) $(CXXFLAGS) $(SPDK_INC) -MM $< -MT $@ -MF $(build_DIR)/$*.d

# Binaries

$(trt_TESTS): %: $(build_DIR)/libtrt.o %.o
	$(CXX) $^ $(LDFLAGS) $(LIBS) -o $@

$(trt_spdk_TESTS): %: $(build_DIR)/libtrt.o $(spdk_OBJ) %.o
	$(CXX) $^ $(LDFLAGS) $(SPDK_LIBS) $(LIBS) -o $@

$(build_DIR)/tests/trt_rdma_echo: $(build_DIR)/libtrt.o $(rdma_OBJ) $(build_DIR)/tests/trt_rdma_echo.o
	$(CXX) $^ $(LDFLAGS) $(LIBS) -o $@

-include $(all_DEP)

clean:
	rm -f $(all_OBJ)
	rm -f $(all_DEP)
	rm -f $(spdk_OBJ) $(spdk_DEP)
	rm -f $(rdma_OBJ) $(rdma_DEP)
	rm -f $(build_DIR)/jctx.o $(build_DIR)/trt-core.o
	rm -f $(trt_TESTS)
	rm -f $(jctx_TESTS)
	rm -f $(trt_spdk_TESTS)
	rm -f $(trt_rdma_TESTS)
	cd external/liburing && make clean && cd -


